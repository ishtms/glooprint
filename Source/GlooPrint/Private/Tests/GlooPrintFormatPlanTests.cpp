// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FFormatPlanCheck final : public IAutomationLatentCommand
{
public:
    explicit FFormatPlanCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FFormatPlanCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->HorizontalSpacing = 16; Settings->VerticalSpacing = 16; Settings->CommentPadding = 32;
            Settings->bFormattingEnabled = true; Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            Fixture->Branch = Fixture->Add<UK2Node_IfThenElse>({0, 200});
            Target = Fixture->Add<UK2Node_ExecutionSequence>({650, 500});
            Test.TestTrue(TEXT("Create representative native execution link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
                Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then), Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute)));
            Comment = Fixture->Add<UEdGraphNode_Comment>({-80, -80});
            Comment->NodeWidth = 2400; Comment->NodeHeight = 1100; Comment->FontSize = 28;
            Comment->NodeComment.Reset();
            for (int32 I = 0; I < 5; ++I) { Comment->NodeComment += TEXT("Keep both steps inside this comment, with space below its wrapped title. "); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint validated format and wrapped title")))
                .ClientSize(FVector2f(1250, 900))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetNodeSelection(Fixture->Branch, true);
            Editor->SetViewLocation(FVector2f(-180, -700), 0.75f);
            Deadline = FPlatformTime::Seconds() + 40;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Validated format fixture did not settle in 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph);
            const auto NativeComment = Panel->GetNodeWidgetFromGuid(Comment->NodeGuid);
            const auto OriginalWidgetReference = Comment->DEPRECATED_NodeWidget;
            const int32 PlanningQueue = GEditor->Trans->GetQueueLength();
            FormatQueue = PlanningQueue - GEditor->Trans->GetUndoCount();
            Test.AddInfo(FString::Printf(TEXT("Validated format starts with %d applied transactions and %d redo entries."),
                FormatQueue, GEditor->Trans->GetUndoCount()));
            const bool bDirty = Fixture->Graph->GetOutermost()->IsDirty();
            if (!Test.TestTrue(TEXT("Build read-only layout and routing plan"),
                PlanFormatGraph(Fixture->Graph, Scale, {Fixture->Branch->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Planning preserves every serialized node/pin value"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Planning never creates or removes graph nodes"), Fixture->Graph->Nodes.Num(), 3);
            Test.TestEqual(TEXT("Planning creates no transaction"), GEditor->Trans->GetQueueLength(), PlanningQueue);
            Test.TestEqual(TEXT("Planning preserves dirty state"), Fixture->Graph->GetOutermost()->IsDirty(), bDirty);
            Test.TestTrue(TEXT("Proposed title measurement preserves live widget identity"), Comment->DEPRECATED_NodeWidget == OriginalWidgetReference &&
                Panel->GetNodeWidgetFromGuid(Comment->NodeGuid) == NativeComment);
            Test.TestTrue(TEXT("Tight spacing invokes a bounded repair"), Plan.SpacingRepairs > 0 && Plan.SpacingRepairs <= 2);
            Test.TestTrue(TEXT("All layout/header passes stay bounded"), Plan.LayoutAttempts <= 9);
            Test.TestTrue(TEXT("Changed comment widths are measured natively"), Plan.CommentMeasurements > 0 && Plan.CommentMeasurements <= 9);
            Test.TestEqual(TEXT("Repair finds clear custom routing"), Plan.Routes.FallbackCount, 0);
            Test.TestEqual(TEXT("Repair does not change the user's spacing preference"), GetDefault<UGlooPrintSettings>()->HorizontalSpacing, 16.f);
            CommentIndex = Plan.Snapshot.Nodes.IndexOfByPredicate([this](const auto& N) { return N.Geometry.Id == Comment->NodeGuid; });
            if (!Test.TestTrue(TEXT("Plan contains original comment identity"), CommentIndex != INDEX_NONE)) { return Finish(); }
            Test.TestTrue(TEXT("Comment is compacted to its members"), Plan.Layout.Sizes[CommentIndex].X < Comment->NodeWidth);
            if (!Test.TestTrue(TEXT("Measure native title at final proposed width"),
                MeasureCommentHeader(Comment, Scale, Plan.Layout.Sizes[CommentIndex].X, ProposedHeader, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Narrow title is taller than the original wide title"),
                ProposedHeader.Max.Y > Plan.Snapshot.Nodes[CommentIndex].Geometry.CommentHeader.GetValue().Max.Y);
            FMeasuredRect InvalidHeader;
            Test.TestFalse(TEXT("Unreliable proposed width is refused"), MeasureCommentHeader(Comment, Scale, 32, InvalidHeader, Reason));
            Test.TestTrue(TEXT("Proposed measurement and refusal leave the graph unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F executes validated format"), Slate.ProcessKeyDownEvent(KeyEvent()));
            Phase = 10; Frames = 0; return false;
        }
        if (Phase == 10)
        {
            if (GEditor->Trans->GetQueueLength() == FormatQueue) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Validated F actually changes the layout"), After != Before);
            Test.TestEqual(TEXT("All stabilization/repair work leads to one transaction"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const int32 I = Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; });
                Test.TestEqual(TEXT("Applied positions match the read-only plan"), FIntPoint(Node->NodePosX, Node->NodePosY), Plan.Layout.Positions[I]);
            }
            Test.TestEqual(TEXT("Applied comment bounds match the plan"), FIntPoint(Comment->NodeWidth, Comment->NodeHeight), Plan.Layout.Sizes[CommentIndex]);
            Test.TestTrue(TEXT("Validated format undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores exact original values"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Validated format redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores exact planned result"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 2)
        {
            const auto Painted = Panel->GetNodeWidgetFromGuid(Comment->NodeGuid)->GetTitleRect();
            Test.TestEqual(TEXT("Canonical title comparison uses normal zoom"), Panel->GetZoomAmount(), 1.f);
            Test.TestTrue(TEXT("Proposed title agrees with the painted widget at normal zoom"),
                FVector2f(Painted.Right - Comment->NodePosX, Painted.Bottom - Comment->NodePosY).Equals(ProposedHeader.Max, 0.1f));
            Test.TestTrue(TEXT("Zoom leaves all formatted node/pin values unchanged"), SerializeNodes(*Fixture->Graph) == After);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Zoom preserves the validated route"), Live && Live->Points == Pair.Value.Points);
            }
            return Finish();
        }
        FGraphMeasurement Actual;
        if (!Test.TestTrue(TEXT("Cold measurement of formatted graph succeeds"), MeasureGraph(Fixture->Graph, Scale, Actual, Reason))) { Test.AddError(Reason); return Finish(); }
        const auto& Header = Find(Actual, Comment->NodeGuid).CommentHeader.GetValue();
        Test.TestTrue(TEXT("Proposed title minimum matches actual resized native title"), Header.Min.Equals(ProposedHeader.Min, 0.1f));
        Test.TestTrue(TEXT("Proposed title maximum matches actual resized native title"), Header.Max.Equals(ProposedHeader.Max, 0.1f));
        const auto NativeComment = Panel->GetNodeWidgetFromGuid(Comment->NodeGuid);
        const auto PaintedHeader = NativeComment->GetTitleRect();
        FMeasuredRect ZoomedHeader;
        if (Test.TestTrue(TEXT("Measure title at the painted widget's native text scale"), MeasureCommentHeader(Comment,
            NativeComment->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale(), Comment->NodeWidth, ZoomedHeader, Reason)))
        {
            Test.TestTrue(TEXT("Zoomed title agrees with native measurement at the same scale"),
                FVector2f(PaintedHeader.Right - Comment->NodePosX, PaintedHeader.Bottom - Comment->NodePosY).Equals(ZoomedHeader.Max, 0.1f));
        }
        for (UEdGraphNode* Node : {static_cast<UEdGraphNode*>(Fixture->Branch), static_cast<UEdGraphNode*>(Target)})
        {
            Test.TestTrue(TEXT("Members clear the wrapped native header and requested padding"), Node->NodePosY - Comment->NodePosY >= Header.Max.Y + 31.5f);
            Test.TestTrue(TEXT("Members also clear the painted zoomed header and requested padding"), Node->NodePosY >= PaintedHeader.Bottom + 31.5f);
        }
        Test.TestEqual(TEXT("Live cache also has no fallback after application"), Cache->GetRoutes().FallbackCount, 0);
        Test.TestEqual(TEXT("Every preflight connection is still visible"), Cache->GetRoutes().Wires.Num(), Plan.Routes.Wires.Num());
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Cold live routing uses the validated pin pair and path"), Live && Live->Points == Pair.Value.Points);
        }
        FFormatPlan Cold;
        if (Test.TestTrue(TEXT("Cold repeated format can be planned"), PlanFormatGraph(Fixture->Graph, Scale, {Fixture->Branch->NodeGuid}, Cold, Reason)))
        {
            Test.TestTrue(TEXT("Repair and title stabilization are cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.Sizes == Plan.Layout.Sizes);
            Test.TestEqual(TEXT("Cold plan chooses the same repair"), Cold.SpacingRepairs, Plan.SpacingRepairs);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Repeated = Cold.Routes.Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Cold repeated planning preserves each validated route"), Repeated && Repeated->Points == Pair.Value.Points);
            }
        }
        const int32 Queue = GEditor->Trans->GetQueueLength();
        Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Slate.ProcessKeyDownEvent(KeyEvent());
        Test.TestTrue(TEXT("Repeated actual F is an exact no-op"), SerializeNodes(*Fixture->Graph) == After);
        Test.TestEqual(TEXT("Validated no-op adds no transaction"), GEditor->Trans->GetQueueLength(), Queue);
        FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
        Test.TestEqual(TEXT("Planning/formatting never pans the view"), AfterView, View);
        Test.TestEqual(TEXT("Planning/formatting never changes zoom"), AfterZoom, Zoom);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native wrapped-title result"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save validated format capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-ValidatedFormat.png"))));
        }
        Test.AddInfo(FString::Printf(TEXT("Validated format: %d layouts, %d proposed title measurements, %d spacing repairs, %d native fallbacks."),
            Plan.LayoutAttempts, Plan.CommentMeasurements, Plan.SpacingRepairs, Plan.Routes.FallbackCount));
        Editor->SetViewLocation(View, 1.f);
        Phase = 2; Frames = 0; return false;
    }
private:
    static FKeyEvent KeyEvent() { return FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0); }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
        Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled;
        Settings->NotifyChanged();
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); Restore(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    UK2Node_ExecutionSequence* Target = nullptr;
    UEdGraphNode_Comment* Comment = nullptr;
    FFormatPlan Plan;
    FMeasuredRect ProposedHeader;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TArray<uint8> Before, After;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, CommentIndex = INDEX_NONE, FormatQueue = 0;
    bool bOriginalEnabled = true, bRestore = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatPlanTest, "GlooPrint.Editor.ValidatedFormatAndWrappedTitle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFormatPlanTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFormatPlanCheck(*this)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatPlanLimitTest, "GlooPrint.Editor.FormatRepairLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFormatPlanLimitTest::RunTest(const FString& Parameters)
{
    FFixture Fixture(UObject::StaticClass(), false);
    auto* A = Fixture.Add<UK2Node_IfThenElse>({0, 80});
    auto* B = Fixture.Add<UK2Node_ExecutionSequence>({100, 80});
    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A->FindPinChecked(UEdGraphSchema_K2::PN_Then), B->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
    for (const auto Position : {FVector2f(-80, 0), FVector2f(-40, -20)})
    {
        auto* C = Fixture.Add<UEdGraphNode_Comment>(Position); C->NodeWidth = 650; C->NodeHeight = 320;
        C->NodeComment = TEXT("Preserve this ambiguous region");
    }
    const auto Before = SerializeNodes(*Fixture.Graph);
    FFormatPlan Plan; FString Reason;
    if (!TestTrue(TEXT("Bounded plan preserves a rigid ambiguous region"), PlanFormatGraph(Fixture.Graph, 1, {A->NodeGuid}, Plan, Reason))) { AddError(Reason); return false; }
    TestTrue(TEXT("Planning does not rewrite the graph to clear an impossible corridor"), Before == SerializeNodes(*Fixture.Graph));
    TestTrue(TEXT("Ambiguous grouping remains explicit"), Plan.Layout.bLimitedComments);
    TestEqual(TEXT("Initial layout plus two repairs exhausts the bound"), Plan.LayoutAttempts, 3);
    TestEqual(TEXT("Equal fallback counts retain the first valid layout"), Plan.SpacingRepairs, 0);
    TestEqual(TEXT("Blocked connection remains an explicit native fallback"), Plan.Routes.FallbackCount, 1);
    TestEqual(TEXT("Unroutable connection is not lost"), Plan.Routes.Wires.Num(), 1);
    for (const auto& Pair : Plan.Routes.Wires)
    {
        TestTrue(TEXT("No partial route is published for the blocked pin"), Pair.Value.Curves.IsEmpty() && Pair.Value.Fallback == ERouteFallback::BlockedEndpoint);
    }
    return true;
}
}
#endif
