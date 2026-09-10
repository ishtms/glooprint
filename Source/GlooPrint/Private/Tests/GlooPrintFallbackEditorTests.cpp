// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineSchema.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Input/HittestGrid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "ScopedTransaction.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FFallbackCheck final : public IAutomationLatentCommand
{
public:
    FFallbackCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle) : Test(InTest), Style(InStyle) {}
    virtual ~FFallbackCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
            OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = Style; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Fixture = MakeUnique<FFixture>();
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then); Input = Output->LinkedTo[0];
            Input->GetOwningNode()->SetPosition({700, 280}); Fixture->Print->SetPosition({320, 30});
            Key = {Output->GetOwningNode()->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId};
            auto* IndependentFrom = Fixture->Add<UK2Node_IfThenElse>({0, 520});
            auto* IndependentTo = Fixture->Add<UK2Node_IfThenElse>({600, 640});
            auto* IndependentOutput = IndependentFrom->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            auto* IndependentInput = IndependentTo->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            if (!Test.TestTrue(TEXT("Native schema accepts the independent comparison wire"),
                Fixture->Graph->GetSchema()->TryCreateConnection(IndependentOutput, IndependentInput))) { return Finish(); }
            IndependentKey = {IndependentFrom->NodeGuid, IndependentOutput->PinId, IndependentTo->NodeGuid, IndependentInput->PinId};
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
            Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            bDirty = Fixture->Graph->GetOutermost()->IsDirty();
            Open(); Deadline = FPlatformTime::Seconds() + 40; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native fallback fixture exceeded 40 seconds.")); return Finish(); }
        if (++Frames < 12) { return false; }
        if (Phase == 4)
        {
            Test.TestFalse(TEXT("Closed panel releases the previous route cache"), WeakCache.IsValid());
            Open(); return Next(5);
        }
        auto* Panel = Editor->GetGraphPanel();
        auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache) { return false; }
        if (Phase == 0)
        {
            if (!Cache->IsReady()) { return false; }
            Test.TestEqual(TEXT("Initial native graph routes all three original connections"), Cache->GetRoutes().Wires.Num(), 3);
            Test.TestEqual(TEXT("Initial connections need no fallback"), Cache->GetRoutes().FallbackCount, 0);
            OriginalRoutes = Cache->GetRoutes(); Builds = Cache->GetBuildCount();
            Unchanged(Before, Queue);
            Delayed = MakeShared<FDelayedNodeFactory>(); Delayed->Target = Fixture->Print;
            FEdGraphUtilities::RegisterVisualNodeFactory(Delayed);
            Cache->Invalidate();
            Test.TestFalse(TEXT("Invalidation immediately makes the route unavailable"), Cache->IsReady());
            Test.TestTrue(TEXT("Invalidation discards stale curves"), Cache->GetRoutes().Wires.IsEmpty());
            CompareDrawing(*Cache, true);
            Test.TestEqual(TEXT("Painting a stale cache does not rebuild it"), Cache->GetBuildCount(), Builds);
            return Next(1);
        }
        if (Phase == 1)
        {
            Test.TestFalse(TEXT("Unavailable measurement leaves native fallback active"), Cache->IsReady());
            Test.TestEqual(TEXT("Route measurement stops after initial attempt and three retries"), Cache->GetBuildCount() - Builds, 4);
            Test.TestEqual(TEXT("Every attempt uses a fresh delayed native widget"), Delayed->Readiness->Creations, 4);
            Builds = Cache->GetBuildCount();
            if (!CompareDrawing(*Cache, true)) { return Finish(); }
            Hover(); return Next(2);
        }
        if (Phase == 2)
        {
            CheckHover(); Capture(TEXT("unavailable")); Unchanged(Before, Queue);
            Test.TestEqual(TEXT("Unavailable fallback stays dormant during idle paints and hover"), Cache->GetBuildCount(), Builds);
            FEdGraphUtilities::UnregisterVisualNodeFactory(Delayed); Delayed.Reset();
            Fixture->Graph->NotifyGraphChanged(); return Next(3);
        }
        if (Phase == 3 || Phase == 5 || Phase == 8)
        {
            if (!Cache->IsReady()) { return false; }
            CheckRecovered(*Cache); Unchanged(Before, Phase == 8 ? Queue + 1 : Queue);
            if (Phase == 3)
            {
                WeakCache = Cache;
                Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
                return Next(4);
            }
            if (Phase == 8)
            {
                Test.TestTrue(TEXT("Blocked-obstacle edit supports native redo"), GEditor->RedoTransaction());
                return Next(9);
            }
            CompareDrawing(*Cache, false); Capture(TEXT("reopened"));
            Test.AddInfo(TEXT("Reopened graph rebuilt all three original custom routes without an F command."));
            const auto Factory = MakeShared<FWireDrawing>(); FSlateWindowElementList Elements(Window);
            for (const UEdGraphSchema* Schema : {static_cast<const UEdGraphSchema*>(GetDefault<UAnimationGraphSchema>()),
                static_cast<const UEdGraphSchema*>(GetDefault<UAnimationStateMachineSchema>())})
            {
                TStrongObjectPtr<UEdGraph> Unsupported(FBlueprintEditorUtils::CreateNewGraph(Fixture->Blueprint.Get(),
                    MakeUniqueObjectName(Fixture->Blueprint.Get(), UEdGraph::StaticClass(), TEXT("UnsupportedFallback")),
                    UEdGraph::StaticClass(), Schema->GetClass()));
                TUniquePtr<FConnectionDrawingPolicy> Policy(Factory->CreateConnectionPolicy(Unsupported->GetSchema(), 0, 1, 1,
                    FSlateRect(0, 0, 1000, 1000), Elements, Unsupported.Get()));
                Test.TestFalse(TEXT("Unsupported pose/state schemas yield connection-factory ownership"), Policy.IsValid());
            }
            FLayoutGraph Snapshot; FString Reason;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Blocked endpoint uses actual native measurements"), CaptureGraphForRouting(Fixture->Graph, Scale, Snapshot, Reason)))
            { Test.AddError(Reason); return Finish(); }
            const auto* Source = Snapshot.Nodes.FindByPredicate([this](const auto& N) { return N.Geometry.Id == Key.FromNode; });
            const auto* Pin = Snapshot.Pins.FindByPredicate([this](const auto& P) { return P.Id == Key.FromPin; });
            if (!Test.TestTrue(TEXT("Original source retains measured geometry"), Source && Pin && Pin->Offset.IsSet())) { return Finish(); }
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("Crowd a native wire endpoint")));
                Fixture->Print->Modify();
                Fixture->Print->SetPosition(FVector2f(Source->Geometry.Position) + FVector2f(Source->Geometry.BodySize.X + 4, Pin->Offset.GetValue().Y - 20));
            }
            Fixture->Graph->NotifyGraphChanged(); Blocked = SerializeNodes(*Fixture->Graph);
            return Next(6);
        }
        if (!Cache->IsReady()) { return false; }
        const auto* BlockedRoute = Cache->GetRoutes().Wires.Find(Key);
        Test.TestTrue(TEXT("Crowded source preserves an explicit blocked-endpoint entry"), BlockedRoute &&
            BlockedRoute->Fallback == ERouteFallback::BlockedEndpoint && BlockedRoute->Curves.IsEmpty());
        Test.TestEqual(TEXT("Blocked endpoint preserves all three original connection entries"), Cache->GetRoutes().Wires.Num(), 3);
        const auto* Independent = Cache->GetRoutes().Wires.Find(IndependentKey);
        const auto* OriginalIndependent = OriginalRoutes.Wires.Find(IndependentKey);
        Test.TestTrue(TEXT("Blocked neighbors preserve the independent custom corridor exactly"), Independent && OriginalIndependent &&
            Independent->Fallback == ERouteFallback::None && Independent->Curves.Num() > 1 && Independent->Points == OriginalIndependent->Points);
        if (Phase == 6)
        {
            if (!CompareDrawing(*Cache, false)) { return Finish(); }
            Hover(); return Next(7);
        }
        if (Phase == 7)
        {
            CheckHover(); Capture(TEXT("blocked"));
            Test.TestTrue(TEXT("Automatic fallback preserves the user's crowded layout exactly"), Blocked == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Blocked-obstacle edit supports native undo"), GEditor->UndoTransaction());
            return Next(8);
        }
        Test.TestTrue(TEXT("Redo restores the exact crowded graph values"), Blocked == SerializeNodes(*Fixture->Graph));
        CompareDrawing(*Cache, false);
        Test.TestEqual(TEXT("Only the deliberate obstacle edit adds an undo entry"), GEditor->Trans->GetQueueLength(), Queue + 1);
        return Finish();
    }
private:
    void Open()
    {
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint native fallback and reopening")))
            .ClientSize(FVector2f(1400, 1150))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        Editor->SetViewLocation(FVector2f(-100, -200), 1.f);
    }
    void Unchanged(const TArray<uint8>& Expected, int32 ExpectedQueue)
    {
        Test.TestTrue(TEXT("Routing preserves every serialized node and pin value"), Expected == SerializeNodes(*Fixture->Graph));
        Test.TestEqual(TEXT("Automatic routing creates no transaction"), GEditor->Trans->GetQueueLength(), ExpectedQueue);
        Test.TestEqual(TEXT("Automatic routing preserves package dirty state"), Fixture->Graph->GetOutermost()->IsDirty(), bDirty);
    }
    void CheckRecovered(const FRouteCache& Cache)
    {
        Test.TestEqual(TEXT("Recovery and reopening have no fallback"), Cache.GetRoutes().FallbackCount, 0);
        Test.TestEqual(TEXT("Recovery retains every original connection"), Cache.GetRoutes().Wires.Num(), OriginalRoutes.Wires.Num());
        for (const auto& Pair : OriginalRoutes.Wires)
        {
            const auto* Route = Cache.GetRoutes().Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Fresh routing restores the exact original path"), Route && Route->Points == Pair.Value.Points);
        }
    }
    bool CompareDrawing(const FRouteCache& Cache, bool bAllNative)
    {
        auto* Panel = Editor->GetGraphPanel();
        FArrangedChildren Nodes(EVisibility::Visible);
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Fallback retains actual native node widgets"), Widget.IsValid())) { return false; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const auto Factory = MakeShared<FWireDrawing>();
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        bool bFoundHover = false;
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        for (UEdGraphPin* From : Node->Pins)
        {
            if (From->Direction != EGPD_Output) { continue; }
            for (UEdGraphPin* To : From->LinkedTo)
            {
                TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
                for (UEdGraphPin* Pin : {From, To})
                {
                    const auto Widget = Panel->GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid)->FindWidgetForPin(Pin);
                    if (!Test.TestTrue(TEXT("Fallback retains both native pin widgets"), Widget.IsValid())) { return false; }
                    Pins.Add(Widget.ToSharedRef(), FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
                }
                FSlateWindowElementList NativeElements(Window), CustomElements(Window);
                FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Fixture->Graph);
                TUniquePtr<FConnectionDrawingPolicy> Custom(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, Scale, Clip, CustomElements, Fixture->Graph));
                if (!Test.TestTrue(TEXT("Fallback uses the registered custom policy implementation"), Custom.IsValid())) { return false; }
                Native.SetAbsoluteMousePosition(FVector2f(-100000, -100000)); Custom->SetAbsoluteMousePosition(FVector2f(-100000, -100000));
                Native.Draw(Pins, Nodes); Custom->Draw(Pins, Nodes);
                const auto& Expected = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                const auto& Actual = CustomElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                if (!Test.TestEqual(TEXT("Native comparison contains the requested original connection"), Expected.Num(), 1) ||
                    !Test.TestTrue(TEXT("Every original fallback/custom connection remains visible"), !Actual.IsEmpty())) { return false; }
                Test.TestTrue(TEXT("Visible wire stays attached to its original native pins"), Actual[0].P0.Equals(Expected[0].P0, 0.1f) && Actual.Last().P3.Equals(Expected[0].P3, 0.1f));
                const auto* Route = Cache.GetRoutes().Wires.Find({Node->NodeGuid, From->PinId, To->GetOwningNode()->NodeGuid, To->PinId});
                if (bAllNative || !Route || Route->Curves.IsEmpty())
                {
                    Test.TestEqual(TEXT("Fallback renders one complete native curve"), Actual.Num(), 1);
                    Test.TestTrue(TEXT("Fallback preserves native control points, color and thickness"),
                        Actual[0].P1.Equals(Expected[0].P1, 0.1f) && Actual[0].P2.Equals(Expected[0].P2, 0.1f) &&
                        Actual[0].GetTint().Equals(Expected[0].GetTint(), 0.001f) && FMath::IsNearlyEqual(Actual[0].GetThickness(), Expected[0].GetThickness(), 0.001f));
                }
                else { Test.TestEqual(TEXT("A valid neighboring route retains every custom piece"), Actual.Num(), Route->Curves.Num()); }
                if (From != Output || To != Input) { continue; }
                for (int32 Sample = 2; Sample < 15 && !bFoundHover; ++Sample)
                {
                    const float T = float(Sample) / 16.f, U = 1 - T;
                    const auto& C = Expected[0];
                    const FVector2f Point = U*U*U*C.P0 + 3*U*U*T*C.P1 + 3*U*T*T*C.P2 + T*T*T*C.P3;
                    bool bBlocked = false;
                    for (int32 I = 0; I < Nodes.Num(); ++I)
                    {
                        if (Fixture->Graph->Nodes[I]->IsA<UEdGraphNode_Comment>()) { continue; }
                        const auto& G = Nodes[I].Geometry;
                        const FBox2f Body(FVector2f(G.GetAbsolutePosition()) - FVector2f(6), FVector2f(G.GetAbsolutePosition() + G.GetAbsoluteSize()) + FVector2f(6));
                        bBlocked |= Body.IsInside(Point);
                    }
                    if (!bBlocked) { HoverPoint = Point; bFoundHover = true; }
                }
            }
        }
        return Test.TestTrue(TEXT("Native fallback has an exposed segment for hover"), bFoundHover);
    }
    void Hover()
    {
        auto& Slate = FSlateApplication::Get(); const FVector2f Previous = Slate.GetCursorPos();
        Slate.SetCursorPos(FVector2D::ZeroVector);
        const FPointerEvent Leave(0, FVector2f::ZeroVector, Previous, TSet<FKey>(), EKeys::Invalid, 0, FModifierKeysState());
        const FWidgetPath Outside(Window->GetHittestGrid().GetBubblePath(FVector2f::ZeroVector, 0, false, 0));
        Slate.RoutePointerMoveEvent(Outside, Leave, false);
        MoveMouseOverGraph(Test, Window.ToSharedRef(), *Editor->GetGraphPanel(), HoverPoint);
    }
    void CheckHover()
    {
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        Test.TestTrue(TEXT("Visible native fallback hover identifies the exact original pins"),
            Editor->GetGraphPanel()->GetPreviousFrameSplineOverlap().GetPins(*Editor->GetGraphPanel(), A, B) &&
            ((A == Output && B == Input) || (A == Input && B == Output)));
    }
    void Capture(const TCHAR* State)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture actual native fallback/recovery"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            const FString Name = FString::Printf(TEXT("GlooPrint-Fallback-%s-%s.png"), Style == EGlooPrintWireStyle::Rounded90 ? TEXT("rounded") : TEXT("diagonal"), State);
            Test.TestTrue(TEXT("Save fallback evidence"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    bool Next(int32 Value) { Phase = Value; Frames = 0; return false; }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (Delayed) { FEdGraphUtilities::UnregisterVisualNodeFactory(Delayed); Delayed.Reset(); }
        if (Window) { Window->RequestDestroyWindow(); } Editor.Reset(); Window.Reset();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
    }
    bool Finish() { Restore(); return true; }
    FAutomationTestBase& Test;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TSharedPtr<FDelayedNodeFactory> Delayed;
    TWeakPtr<FRouteCache> WeakCache;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key, IndependentKey;
    FRouteSet OriginalRoutes;
    TArray<uint8> Before, Blocked;
    FVector2D OriginalCursor;
    FVector2f HoverPoint;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, Queue = 0;
    bool bRestore = false, bDirty = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFallbackRoundedTest, "GlooPrint.Editor.NativeFallbackAndReopen.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFallbackRoundedTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFallbackCheck(*this, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFallbackDiagonalTest, "GlooPrint.Editor.NativeFallbackAndReopen.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFallbackDiagonalTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFallbackCheck(*this, EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
