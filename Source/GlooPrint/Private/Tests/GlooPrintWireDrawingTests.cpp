// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWireCheck final : public IAutomationLatentCommand
{
public:
    explicit FWireCheck(FAutomationTestBase& InTest, bool bInOffscreen = false,
        EGlooPrintWireStyle InStyle = EGlooPrintWireStyle::Rounded90)
        : Test(InTest), Style(InStyle), bOffscreen(bInOffscreen) {}
    virtual ~FWireCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalCursor = Slate.GetCursorPos();
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalStyle = Settings->WireStyle; bRestore = true;
            Settings->WireStyle = Style; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>();
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Output->LinkedTo[0];
            Input->GetOwningNode()->SetPosition({bOffscreen ? -700 : 700, 280});
            Fixture->Print->SetPosition(bOffscreen ? FVector2f(1000, 1000) : FVector2f(320, 30));
            Key = {Output->GetOwningNode()->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId};
            FString NativeTooltip;
            Fixture->Print->GetPinHoverText(*Fixture->Print->FindPinChecked(TEXT("InString")), NativeTooltip);
            Test.TestTrue(TEXT("Native function tooltip is available before opening the graph"), !NativeTooltip.IsEmpty());
            Before = SerializeNodes(*Fixture->Graph);
            BeforeProperties = DescribeNodes(*Fixture->Graph);
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(bOffscreen ? TEXT("GlooPrint offscreen return wire") : TEXT("GlooPrint rounded routes")))
                .ClientSize(bOffscreen ? FVector2f(480, 360) : FVector2f(1200, 850))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -200), 1.f);
            Deadline = FPlatformTime::Seconds() + 30;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Wire fixture did not settle within 30 seconds.")); return Finish(); }
        if (!bOffscreen && Phase == 4)
        {
            if (++Frames < 8) { return false; }
            Test.TestFalse(TEXT("Closing the graph releases its route cache"), WeakCache.IsValid());
            return Finish();
        }
        SGraphPanel* Panel = Editor->GetGraphPanel();
        auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        const auto* Route = Cache->GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Open graph creates a route without pressing F"), Route && !Route->Curves.IsEmpty())) { return Finish(); }
        if (bOffscreen) { return CheckOffscreen(*Panel, *Cache, *Route); }
        if (Phase == 0)
        {
            if (++Frames < 8) { return false; }
            if (!Test.TestTrue(TEXT("Automatic routing preserves every serialized node/pin byte"), Before == SerializeNodes(*Fixture->Graph)))
            {
                ReportNodeDifferences(Test, BeforeProperties, *Fixture->Graph);
            }
            OriginalPoints = Route->Points;
            Builds = Cache->GetBuildCount();
            Hover(*Panel, *Route);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            if (++Frames < 8) { return false; }
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestTrue(TEXT("Native hover finds the visible routed wire"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B));
            Test.TestTrue(TEXT("Visible routed wire identifies its original pin pair"), (A == Output && B == Input) || (A == Input && B == Output));
            Test.TestEqual(TEXT("Idle paints do not rebuild routes"), Cache->GetBuildCount(), Builds);
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture rounded route fixture"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save rounded wire screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-RoundedWires.png"))));
            }
            const int32 NodesBeforeClick = Fixture->Graph->Nodes.Num();
            const FPointerEvent Click(0, Mouse, Mouse, TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
            Panel->OnMouseButtonDoubleClick(Panel->GetCachedGeometry(), Click);
            if (!Test.TestEqual(TEXT("Double-click on the rendered wire inserts one native reroute"), Fixture->Graph->Nodes.Num(), NodesBeforeClick + 1)) { return Finish(); }
            auto* Knot = Output->LinkedTo.IsEmpty() ? nullptr : Cast<UK2Node_Knot>(Output->LinkedTo[0]->GetOwningNode());
            Test.TestTrue(TEXT("Reroute insertion splits the identified pin pair"), Knot && Knot->GetOutputPin()->LinkedTo.Contains(Input));
            Test.TestTrue(TEXT("Reroute insertion remains one native undo"), GEditor->UndoTransaction());
            Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            if (++Frames < 8) { return false; }
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Output->LinkedTo[0];
            Test.TestTrue(TEXT("Undoing reroute insertion restores every original graph value"), Before == SerializeNodes(*Fixture->Graph));
            Builds = Cache->GetBuildCount();
            Editor->SetViewLocation(FVector2f(-60, -140), 0.75f);
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            if (++Frames < 8) { return false; }
            Hover(*Panel, *Route);
            Phase = 7; Frames = 0; return false;
        }
        if (Phase == 7)
        {
            if (++Frames < 8) { return false; }
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestTrue(TEXT("Pan and zoom preserve visible-path hover"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
                ((A == Output && B == Input) || (A == Input && B == Output)));
            Test.TestEqual(TEXT("Pan and zoom reuse graph-coordinate routes"), Cache->GetBuildCount(), Builds);
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("Move wire obstacle")));
                Fixture->Print->Modify();
                Test.TestFalse(TEXT("Modifying an unrelated obstacle immediately invalidates this wire"), Cache->IsReady());
                Fixture->Print->SetPosition({350, -60});
            }
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            if (++Frames < 8) { return false; }
            Test.TestTrue(TEXT("Obstacle edit rebuilds routes outside paint"), Cache->GetBuildCount() > Builds);
            Test.TestTrue(TEXT("Native undo of obstacle movement succeeds"), GEditor->UndoTransaction());
            Test.TestFalse(TEXT("Undo invalidates routes"), Cache->IsReady());
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            if (++Frames < 8) { return false; }
            Test.TestTrue(TEXT("Undo restores the original deterministic wire"), Route->Points == OriginalPoints);
            Test.TestTrue(TEXT("Route rebuild after undo changes no graph values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Obstacle move also supports native redo"), GEditor->RedoTransaction());
            WeakCache = Cache;
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Phase = 4; Frames = 0;
            return false;
        }
        return false;
    }
    bool Finish()
    {
        Restore(); return true;
    }
private:
    bool CheckOffscreen(SGraphPanel& Panel, const FRouteCache& Cache, const FWireRoute& Route)
    {
        if (++Frames < 8) { return false; }
        if (Phase == 0)
        {
            FVector2f FromMin, FromMax, ToMin, ToMax;
            if (!Test.TestTrue(TEXT("Both native endpoint node bounds are available"),
                Panel.GetBoundsForNode(Output->GetOwningNode(), FromMin, FromMax) &&
                Panel.GetBoundsForNode(Input->GetOwningNode(), ToMin, ToMax))) { return Finish(); }
            OffscreenPoint = Route.Curves[0].Start;
            for (const auto& Curve : Route.Curves)
            {
                const FVector2f Point = EvaluateRouteCurve(Curve, Curve.Length * 0.5f);
                if (Point.X > OffscreenPoint.X) { OffscreenPoint = Point; }
            }
            const float NodeRight = FMath::Max(FromMax.X, ToMax.X);
            if (!Test.TestTrue(TEXT("Return lane extends beyond both endpoint node bodies"), OffscreenPoint.X > NodeRight + 8)) { return Finish(); }
            Builds = Cache.GetBuildCount();
            Editor->SetViewLocation(FVector2f((NodeRight + OffscreenPoint.X) * 0.5f,
                OffscreenPoint.Y - Panel.GetCachedGeometry().GetLocalSize().Y * 0.25f), 2.f);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            FVector2f Min, Max;
            for (UEdGraphPin* Pin : {Output, Input})
            {
                if (!Test.TestTrue(TEXT("Entire endpoint node is off the left of the actual viewport"),
                    Panel.GetBoundsForNode(Pin->GetOwningNode(), Min, Max) && Max.X < Panel.GetViewOffset().X)) { return Finish(); }
            }
            const FVector2f Local = (OffscreenPoint - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount();
            const FVector2f Size = Panel.GetCachedGeometry().GetLocalSize();
            if (!Test.TestTrue(TEXT("Actual return-lane point is inside the viewport"),
                Local.X > 0 && Local.X < Size.X && Local.Y > 0 && Local.Y < Size.Y)) { return Finish(); }
            Mouse = Panel.GetCachedGeometry().LocalToAbsolute(Local);
            MoveMouseOverGraph(Test, Window.ToSharedRef(), Panel, Mouse);
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestEqual(TEXT("Offscreen panning and idle painting reuse the route cache"), Cache.GetBuildCount(), Builds);
            Test.TestFalse(TEXT("Offscreen idle paint leaves no capture or routing pending"), Cache.HasPendingRouting());
            Test.TestTrue(TEXT("Offscreen drawing preserves all original graph values"), Before == SerializeNodes(*Fixture->Graph));
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture actual offscreen-endpoint wire"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const TCHAR* Name = Style == EGlooPrintWireStyle::Rounded90 ? TEXT("GlooPrint-OffscreenRounded.png") : TEXT("GlooPrint-OffscreenDiagonal.png");
                Test.TestTrue(TEXT("Save offscreen-endpoint capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
            }
            if (!Test.TestTrue(TEXT("Visible return lane retains native hover with both nodes offscreen"),
                Panel.GetPreviousFrameSplineOverlap().GetPins(Panel, A, B) &&
                ((A == Output && B == Input) || (A == Input && B == Output)))) { return Finish(); }
            const int32 NodeCount = Fixture->Graph->Nodes.Num();
            const FPointerEvent Click(0, Mouse, Mouse, TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
            Panel.OnMouseButtonDoubleClick(Panel.GetCachedGeometry(), Click);
            if (!Test.TestEqual(TEXT("Offscreen-endpoint wire double-click inserts one native reroute"), Fixture->Graph->Nodes.Num(), NodeCount + 1)) { return Finish(); }
            auto* Knot = Output->LinkedTo.IsEmpty() ? nullptr : Cast<UK2Node_Knot>(Output->LinkedTo[0]->GetOwningNode());
            Test.TestTrue(TEXT("Offscreen wire interaction splits its original pin pair"), Knot && Knot->GetOutputPin()->LinkedTo.Contains(Input));
            WithOffscreenReroute = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Offscreen wire insertion supports native undo"), GEditor->UndoTransaction());
            Phase = 3; Frames = 0; return false;
        }
        Test.TestTrue(TEXT("Undo offscreen reroute insertion restores every original graph value"), Before == SerializeNodes(*Fixture->Graph));
        Test.TestTrue(TEXT("Offscreen wire insertion also supports native redo"), GEditor->RedoTransaction());
        Test.TestTrue(TEXT("Redo restores the exact offscreen reroute insertion"), WithOffscreenReroute == SerializeNodes(*Fixture->Graph));
        return Finish();
    }
    void Restore()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset();
        if (bRestore)
        {
            bRestore = false;
            auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = OriginalStyle; Settings->NotifyChanged();
            FSlateApplication::Get().SetCursorPos(OriginalCursor);
        }
    }
    void Hover(SGraphPanel& Panel, const FWireRoute& Route)
    {
        const FVector2f GraphPoint = EvaluateRoute(Route, Route.Length * 0.5f);
        Mouse = Panel.GetCachedGeometry().LocalToAbsolute((GraphPoint - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount());
        MoveMouseOverGraph(Test, Window.ToSharedRef(), Panel, Mouse);
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> WeakCache;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key;
    TArray<uint8> Before;
    TArray<uint8> WithOffscreenReroute;
    TMap<FString, FString> BeforeProperties;
    TArray<FVector2f> OriginalPoints;
    FVector2f Mouse;
    FVector2f OffscreenPoint;
    FVector2D OriginalCursor;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    bool bOffscreen = false, bRestore = false;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Builds = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWireDrawingTest, "GlooPrint.Editor.RoutedWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWireDrawingTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireCheck(*this)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOffscreenRoundedWireTest, "GlooPrint.Editor.RoutedWiresOffscreen.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOffscreenRoundedWireTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireCheck(*this, true, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOffscreenDiagonalWireTest, "GlooPrint.Editor.RoutedWiresOffscreen.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOffscreenDiagonalWireTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireCheck(*this, true, EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
