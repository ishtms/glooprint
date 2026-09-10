// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Algo/Reverse.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FReturnBundleCheck final : public IAutomationLatentCommand
{
public:
    FReturnBundleCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle) : Test(InTest), Style(InStyle) {}
    virtual ~FReturnBundleCheck()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        if (bRestore)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Settings->HorizontalSpacing = Original.HorizontalSpacing; Settings->VerticalSpacing = Original.VerticalSpacing;
            Settings->CommentPadding = Original.CommentPadding; Settings->WireStyle = OriginalStyle;
            Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        }
    }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Original = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->WireStyle = Style; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("RetryStages");
            UEdGraphNode* Previous = Entry;
            for (int32 I = 0; I < 6; ++I)
            {
                auto* Branch = Fixture->Add<UK2Node_IfThenElse>({float(700 - I * 220), float(200 + I * 170)});
                Branches.Add(Branch);
                Link(Previous->FindPinChecked(UEdGraphSchema_K2::PN_Then), Branch->GetExecPin()); Previous = Branch;
            }
            auto* Print = Call(TEXT("PrintString"), {1600, 900});
            Link(Previous->FindPinChecked(UEdGraphSchema_K2::PN_Then), Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("Stages complete");
            for (int32 I = 0; I < 3; ++I)
            {
                ReturnPins.Add({Branches[I + 3]->GetElsePin(), Branches[I]->GetExecPin()});
                Link(ReturnPins.Last().Key, ReturnPins.Last().Value);
                auto* Value = Call(TEXT("MakeLiteralBool"), {float(-800 + I * 350), 1200});
                Value->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("true");
                auto* Output = Value->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue);
                Link(Output, Branches[I]->GetConditionPin()); Link(Output, Branches[I + 3]->GetConditionPin());
                LongPins.Add({Output, Branches[I + 3]->GetConditionPin()});
            }
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            if (!bLinksValid) { return true; }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Compile);
            if (!Test.TestEqual(TEXT("Native retry graph compiles without executing loops"), Compile.NumErrors, 0)) { return true; }
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                for (auto* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
            }
            Open(); Editor->SetNodeSelection(Entry, true);
            Deadline = FPlatformTime::Seconds() + 60; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native return bundle did not settle in 60 seconds.")); return true; }
        if (++Frames < 12) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Capture(TEXT("Before"));
            Before = SerializeNodes(*Fixture->Graph); BeforeValues = SerializeTransactionValues(*Fixture->Graph);
            Properties = DescribeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Native multi-return plan computes"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return true; }
            Test.TestTrue(TEXT("Planning leaves graph values unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("All sixteen original connections have routes"), Plan.Routes.Wires.Num(), 16);
            Test.TestEqual(TEXT("Multiple returns and long data need no fallback"), Plan.Routes.FallbackCount, 0);
            CheckGeometry();
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats the retry graph"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 6; return false;
        }
        if (Phase == 6)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            const auto AfterValues = SerializeTransactionValues(*Fixture->Graph);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("F moves the scattered graph"), Before != After);
            Test.TestEqual(TEXT("F creates exactly one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestEqual(TEXT("F retains all property identities"), Properties.Num(), AfterProperties.Num());
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const auto* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" survives return packing"), Value && *Value == Pair.Value);
            }
            Test.TestTrue(TEXT("F preserves the selected anchor"), Editor->GetSelectedNodes().Num() == 1 &&
                Editor->GetSelectedNodes().Contains(Entry) && Entry->NodePosX == 0 && Entry->NodePosY == 0);
            Test.TestTrue(TEXT("Native undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores all graph values"), BeforeValues == SerializeTransactionValues(*Fixture->Graph));
            Test.TestTrue(TEXT("Native redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores all graph values"), AfterValues == SerializeTransactionValues(*Fixture->Graph));
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Return bundle remeasures from cold widgets"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Cold layout is idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
                CheckPaths(Cold.Routes); CheckPaths(Cache->GetRoutes());
            }
            else { Test.AddError(Reason); }
            FLayoutGraph Shuffled = Plan.Snapshot;
            Algo::Reverse(Shuffled.Edges); FRouteSet ShuffledRoutes;
            if (Test.TestTrue(TEXT("Shuffled original links compute"), ComputeLayoutRoutes(Shuffled, Plan.Layout, ShuffledRoutes, Reason, Style))) { CheckPaths(ShuffledRoutes); }
            Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 7; Frames = 0; return false;
        }
        if (Phase == 7)
        {
            Test.TestEqual(TEXT("Repeated F creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
            Test.TestTrue(TEXT("Repeated F leaves every graph value unchanged"), After == SerializeNodes(*Fixture->Graph));
            Capture(TEXT("Overview"));
            OldCache = Cache; Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Open(); Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            Test.TestFalse(TEXT("Closing releases the original bundle cache"), OldCache.IsValid());
            CheckPaths(Cache->GetRoutes()); Builds = Cache->GetBuildCount();
            HoverPins = ReturnPins; HoverPins.Append(LongPins);
            Phase = 3;
        }
        if (Phase == 3)
        {
            const auto& Pins = HoverPins[HoverIndex];
            const auto* Route = Cache->GetRoutes().Wires.Find(Key(Pins));
            if (!Test.TestTrue(TEXT("Original connection is ready for hover"), Route && Route->Curves.Num() > 0)) { return true; }
            float Longest = 0;
            for (const auto& Curve : Route->Curves)
            {
                const float Length = FMath::Abs(Curve.End.X - Curve.Start.X);
                if (Curve.Start.Y == Curve.End.Y && Length > Longest)
                {
                    Longest = Length; HoverPoint = (Curve.Start + Curve.End) * 0.5f;
                }
            }
            Test.TestTrue(TEXT("Return or long data wire has an exposed horizontal run"), Longest > 100);
            Editor->SetViewLocation(HoverPoint - FVector2f(400, 280), 1.f);
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel,
                Panel->GetCachedGeometry().LocalToAbsolute((HoverPoint - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount()));
            Phase = 5; Frames = 0; return false;
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        const auto& Pins = HoverPins[HoverIndex];
        Test.TestTrue(FString::Printf(TEXT("Independent bundled wire %d hovers its exact original pin pair"), HoverIndex),
            Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
            ((A == Pins.Key && B == Pins.Value) || (A == Pins.Value && B == Pins.Key)));
        Test.TestEqual(TEXT("Pan, zoom and hover reuse cached bundle routing"), Cache->GetBuildCount(), Builds);
        CheckPaths(Cache->GetRoutes());
        if (HoverIndex == 1) { Capture(TEXT("Returns")); }
        if (HoverIndex == 4) { Capture(TEXT("LongData")); }
        if (++HoverIndex < HoverPins.Num()) { Phase = 3; Frames = 0; return false; }
        Test.TestTrue(TEXT("Reopen and every wire interaction preserve the graph"), After == SerializeNodes(*Fixture->Graph));
        return true;
    }
private:
    using FPinPair = TPair<UEdGraphPin*, UEdGraphPin*>;
    static FRouteKey Key(const FPinPair& Pair) { return {Pair.Key->GetOwningNode()->NodeGuid, Pair.Key->PinId, Pair.Value->GetOwningNode()->NodeGuid, Pair.Value->PinId}; }
    void Open()
    {
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint return bundles"))).ClientSize(FVector2f(1550, 950))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-100, -220), 0.5f);
    }
    UK2Node_CallFunction* Call(FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Name));
        Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* From, UEdGraphPin* To)
    {
        bLinksValid &= Test.TestTrue(TEXT("Native schema accepts retry fixture link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To));
    }
    void CheckPaths(const FRouteSet& Routes)
    {
        Test.TestEqual(TEXT("Every original wire remains available"), Routes.Wires.Num(), Plan.Routes.Wires.Num());
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const auto* Route = Routes.Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Original pin pair retains its exact custom corridor"), Route && Route->Points == Pair.Value.Points && Route->Fallback == ERouteFallback::None);
        }
    }
    void CheckGeometry()
    {
        TArray<float> ReturnY;
        FBox2f Envelope(ForceInit);
        for (int32 N = 0; N < Plan.Snapshot.Nodes.Num(); ++N)
        {
            const auto& Geometry = Plan.Snapshot.Nodes[N].Geometry;
            Envelope += FVector2f(Plan.Layout.Positions[N]) + Geometry.VisualBounds.Min;
            Envelope += FVector2f(Plan.Layout.Positions[N]) + Geometry.VisualBounds.Max;
        }
        for (const auto& Pair : ReturnPins)
        {
            const auto* Route = Plan.Routes.Wires.Find(Key(Pair));
            if (!Route) { Test.AddError(TEXT("Missing feedback route.")); continue; }
            float Longest = 0, Y = 0;
            for (int32 I = 1; I < Route->Points.Num(); ++I)
            {
                const auto A = Route->Points[I - 1], B = Route->Points[I];
                if (A.Y == B.Y && A.X - B.X > Longest) { Longest = A.X - B.X; Y = A.Y; }
            }
            Test.TestTrue(TEXT("Feedback crosses several columns on its own return lane"), Longest > 400);
            ReturnY.Add(Y); Test.AddInfo(FString::Printf(TEXT("Return lane Y%.2f, length%.2f, bends%d."), Y, Route->Length, Route->Points.Num() - 2));
        }
        ReturnY.Sort();
        for (int32 I = 1; I < ReturnY.Num(); ++I)
        {
            Test.TestTrue(TEXT("Overlapping return lanes remain separate"), ReturnY[I] - ReturnY[I - 1] >= WireLaneSpacing);
            Test.TestTrue(TEXT("Return bundle does not waste an empty lane gap"), ReturnY[I] - ReturnY[I - 1] <= 2 * WireLaneSpacing);
        }
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const auto& Route = Pair.Value; Envelope += Route.Bounds.Min; Envelope += Route.Bounds.Max;
            for (int32 C = 0; C < Route.Curves.Num(); ++C)
            {
                const auto& Curve = Route.Curves[C];
                for (int32 S = 1; S < 100; ++S)
                {
                    const auto P = FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, S / 100.f);
                    for (int32 N = 0; N < Plan.Snapshot.Nodes.Num(); ++N)
                    {
                        const auto& Node = Plan.Snapshot.Nodes[N];
                        const FVector2f Min(Plan.Layout.Positions[N]), Max = Min + Node.Geometry.BodySize;
                        const bool bTerminal = Curve.Start.Y == Curve.End.Y && Curve.StartTangent.Y == 0 && Curve.EndTangent.Y == 0 &&
                            ((C == 0 && Pair.Key.FromNode == Node.Geometry.Id) || (C == Route.Curves.Num() - 1 && Pair.Key.ToNode == Node.Geometry.Id));
                        Test.TestFalse(TEXT("Every bundled curve clears native bodies outside its own pin inset"), !bTerminal && P.X > Min.X && P.X < Max.X && P.Y > Min.Y && P.Y < Max.Y);
                    }
                }
            }
        }
        Test.AddInfo(FString::Printf(TEXT("Return bundle envelope %.0f x %.0f; repairs%d, fallbacks%d."),
            Envelope.Max.X - Envelope.Min.X, Envelope.Max.Y - Envelope.Min.Y, Plan.SpacingRepairs, Plan.Routes.FallbackCount));
    }
    void Capture(const TCHAR* Stage)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native return bundle"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            const FString Name = FString::Printf(TEXT("GlooPrint-ReturnBundle-%s-%s.png"), Style == EGlooPrintWireStyle::Rounded90 ? TEXT("Rounded") : TEXT("Diagonal"), Stage);
            Test.TestTrue(TEXT("Save native return bundle capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    FAutomationTestBase& Test;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FLayoutSettings Original;
    TUniquePtr<FFixture> Fixture;
    UK2Node_CustomEvent* Entry = nullptr;
    TArray<UK2Node_IfThenElse*> Branches;
    TArray<FPinPair> ReturnPins, LongPins, HoverPins;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> OldCache;
    FFormatPlan Plan;
    TArray<uint8> Before, BeforeValues, After;
    TMap<FString, FString> Properties;
    FVector2f HoverPoint;
    double Deadline = 0;
    int32 Frames = 0, Phase = 0, HoverIndex = 0, Builds = 0, Queue = 0;
    bool bRestore = false, bOriginalEnabled = true, bLinksValid = true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReturnBundleRoundedTest, "GlooPrint.Editor.ReturnAndLongBundles.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FReturnBundleRoundedTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FReturnBundleCheck(*this, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReturnBundleDiagonalTest, "GlooPrint.Editor.ReturnAndLongBundles.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FReturnBundleDiagonalTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FReturnBundleCheck(*this, EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
