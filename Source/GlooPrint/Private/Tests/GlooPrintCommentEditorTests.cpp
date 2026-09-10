// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FCommentCheck final : public IAutomationLatentCommand
{
public:
    FCommentCheck(FAutomationTestBase& InTest, bool bInAmbiguous) : Test(InTest), bAmbiguous(bInAmbiguous) {}
    virtual ~FCommentCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->bFormattingEnabled = true; Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("CommentMembershipExample");
            auto* Branch = Fixture->Add<UK2Node_IfThenElse>(bAmbiguous ? FVector2f(500, 450) : FVector2f(400, 400));
            auto* First = Call(bAmbiguous ? FVector2f(800, 450) : FVector2f(500, 900));
            auto* Second = Call(bAmbiguous ? FVector2f(1200, 750) : FVector2f(1300, 300));
            auto* Tail = Call(bAmbiguous ? FVector2f(1800, 100) : FVector2f(2200, 100));
            Link(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then), First->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(First->FindPinChecked(UEdGraphSchema_K2::PN_Then), Second->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Second->FindPinChecked(UEdGraphSchema_K2::PN_Then), Tail->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            if (!bConnectionsValid) { return Finish(); }
            if (bAmbiguous)
            {
                auto* Left = Comment({400, 300}, {850, 750}, TEXT("Left group — preserve overlap"), FLinearColor(0.18f, 0.4f, 0.7f));
                auto* Right = Comment({700, 380}, {850, 750}, TEXT("Right group — shared member"), FLinearColor(0.65f, 0.25f, 0.18f));
                Members.Add(Left, {Branch, First}); Members.Add(Right, {First, Second});
                Rigid = {Branch, First, Second, Left, Right}; MovementComment = Left;
            }
            else
            {
                auto* Inner = Comment({300, 200}, {850, 1000}, TEXT("Inner steps"), FLinearColor(0.18f, 0.45f, 0.7f));
                auto* Outer = Comment({200, 100}, {1700, 1400}, TEXT("Outer group — keep the inner steps together"), FLinearColor(0.2f, 0.5f, 0.25f));
                Inner->MoveMode = ECommentBoxMode::NoGroupMovement;
                Members.Add(Inner, {Branch, First}); Members.Add(Outer, {Branch, First, Second, Inner});
                DirectMembers.Add(Inner, {Branch, First}); DirectMembers.Add(Outer, {Inner, Second});
                MovementComment = Outer; NoMovementComment = Inner;
            }
            Empty = Comment({-600, -350}, {280, 140}, TEXT("Empty: keep here"), FLinearColor(0.45f, 0.35f, 0.15f));
            Members.Add(Empty, {});
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Comment fixture compiles as native Actor Blueprint"), Compile.NumErrors, 0)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(bAmbiguous ? TEXT("GlooPrint overlapping comments") : TEXT("GlooPrint nested comments")))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            for (auto* C : Comments) { Editor->SetNodeSelection(C, true); }
            Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false); Slate.SetCursorPos(FVector2D::ZeroVector);
            Deadline = FPlatformTime::Seconds() + 45; CaptureAfter = FPlatformTime::Seconds() + 1;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native comment fixture did not settle in 45 seconds.")); return Finish(); }
        if (++Frames < 10 || FPlatformTime::Seconds() < CaptureAfter) { return false; }
        auto* Panel = Editor->GetGraphPanel(); const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            CheckMembership(); Capture(TEXT("Before"));
            Before = SerializeNodes(*Fixture->Graph); Properties = DescribeNodes(*Fixture->Graph);
            for (UEdGraphNode* Node : Fixture->Graph->Nodes) { OriginalPositions.Add(Node, {Node->NodePosX, Node->NodePosY}); }
            if (!Test.TestTrue(TEXT("Original comment memberships admit a complete format plan"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            Test.TestTrue(TEXT("Comment planning preserves all serialized data"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Only ambiguous regions report limited formatting"), Plan.Layout.bLimitedComments, bAmbiguous);
            Test.TestEqual(TEXT("All original comment and ordinary identities are planned"), Plan.Snapshot.Nodes.Num(), 8);
            Test.TestEqual(TEXT("Comments preserve all four execution connections"), Plan.Routes.Wires.Num(), 4);
            Test.TestEqual(TEXT("Comment fixture routes clear node bodies and headers"), Plan.Routes.FallbackCount, 0);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats with comments selected alongside the entry anchor"), Slate.ProcessKeyDownEvent(KeyEvent()));
            Phase = 5; return false;
        }
        if (Phase == 5)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph); Test.TestTrue(TEXT("Comment fixture layout changes"), Before != After);
            Test.TestEqual(TEXT("Comment formatting creates exactly one undo step"), GEditor->Trans->GetQueueLength(), Queue + 1);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY")) ||
                    Pair.Key.EndsWith(TEXT(".NodeWidth")) || Pair.Key.EndsWith(TEXT(".NodeHeight"))) { continue; }
                const FString* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" retained by comment formatting"), Value && *Value == Pair.Value);
            }
            CheckPositions();
            Test.TestTrue(TEXT("Comment layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Comment undo restores every serialized value"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Comment layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Comment redo restores every serialized value"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            CheckMembership(); CheckPositions(); CheckPaintedBounds();
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Cold native comment planning succeeds"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Cold layout preserves positions and comment sizes"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.Sizes == Plan.Layout.Sizes);
                for (const auto& Pair : Plan.Routes.Wires)
                {
                    const auto* Repeated = Cold.Routes.Wires.Find(Pair.Key); const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                    Test.TestTrue(TEXT("Cold and live comment routes retain the planned original pin pair and path"),
                        Repeated && Live && Repeated->Points == Pair.Value.Points && Live->Points == Pair.Value.Points);
                }
            }
            Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly); Slate.ProcessKeyDownEvent(KeyEvent());
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            Test.TestTrue(TEXT("Repeated actual F preserves all comment values"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Comment no-op adds no undo step"), GEditor->Trans->GetQueueLength(), Queue);
            FVector2f CurrentView; float CurrentZoom; Editor->GetViewLocation(CurrentView, CurrentZoom);
            Test.TestEqual(TEXT("Comment formatting preserves the camera"), CurrentView, View);
            Test.TestEqual(TEXT("Comment formatting preserves zoom"), CurrentZoom, Zoom);
            Test.TestEqual(TEXT("F preserves the entry and comment selection"), Editor->GetSelectedNodes().Num(), 4);
            Editor->ZoomToFit(false); CaptureAfter = FPlatformTime::Seconds() + 1; Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            Capture(TEXT("After"));
            Editor->ClearSelectionSet(); Editor->SetNodeSelection(MovementComment, true);
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            CheckMovement(MovementComment, true);
            if (NoMovementComment)
            {
                Editor->ClearSelectionSet(); Editor->SetNodeSelection(NoMovementComment, true);
                Phase = 4; Frames = 0; return false;
            }
        }
        else { CheckMovement(NoMovementComment, false); }
        Test.AddInfo(FString::Printf(TEXT("Native %s comments: %d spacing repairs, %d fallbacks; original native memberships and movement modes retained."),
            bAmbiguous ? TEXT("overlapping") : TEXT("nested"), Plan.SpacingRepairs, Plan.Routes.FallbackCount));
        return Finish();
    }
private:
    static FKeyEvent KeyEvent() { return FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0); }
    void CheckMembership()
    {
        for (const auto& Pair : Members)
        {
            const auto& Actual = Pair.Key->GetNodesUnderComment();
            Test.TestEqual(Pair.Key->NodeComment + TEXT(" native member count"), Actual.Num(), Pair.Value.Num());
            for (auto* Member : Pair.Value) { Test.TestTrue(TEXT("Native comment grouping retains the exact original member object"), Actual.Contains(Member)); }
        }
    }
    void CheckPositions()
    {
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const int32 I = Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; });
            if (!Test.TestTrue(TEXT("Original node identity survives comment formatting"), I != INDEX_NONE)) { continue; }
            Test.TestEqual(TEXT("Actual comment layout matches preflight"), FIntPoint(Node->NodePosX, Node->NodePosY), Plan.Layout.Positions[I]);
            Test.TestEqual(TEXT("Only planned comment sizes change"), FIntPoint(Node->NodeWidth, Node->NodeHeight), Plan.Layout.Sizes[I]);
            if (Rigid.Contains(Node))
            {
                Test.TestEqual(TEXT("Every touching member of an ambiguous region has the same translation"),
                    FIntPoint(Node->NodePosX, Node->NodePosY) - OriginalPositions.FindChecked(Node),
                    FIntPoint(Rigid[0]->NodePosX, Rigid[0]->NodePosY) - OriginalPositions.FindChecked(Rigid[0]));
                Test.TestEqual(TEXT("Ambiguous region preserves all original sizes"), Plan.Layout.Sizes[I], Plan.Snapshot.Nodes[I].OriginalSize);
            }
        }
        Test.TestEqual(TEXT("Empty comment stays at its original location"), FIntPoint(Empty->NodePosX, Empty->NodePosY), OriginalPositions.FindChecked(Empty));
        Test.TestEqual(TEXT("Empty comment keeps its exact original size"), FIntPoint(Empty->NodeWidth, Empty->NodeHeight), FIntPoint(280, 140));
        Test.TestEqual(TEXT("Selected execution entry stays anchored"), FIntPoint(Entry->NodePosX, Entry->NodePosY), OriginalPositions.FindChecked(Entry));
    }
    void CheckPaintedBounds()
    {
        auto* Panel = Editor->GetGraphPanel();
        for (auto* C : Comments)
        {
            const auto Native = Panel->GetNodeWidgetFromGuid(C->NodeGuid);
            if (!Test.TestTrue(TEXT("Comment has a native widget"), Native.IsValid())) { continue; }
            Test.TestEqual(TEXT("Painted comment bounds use the applied size"), FVector2f(Native->GetDesiredSize()), FVector2f(C->NodeWidth, C->NodeHeight));
            const auto Title = Native->GetTitleRect(); FMeasuredRect Header; FString Reason;
            if (Test.TestTrue(TEXT("Measure comment title at actual painted text scale"), MeasureCommentHeader(C,
                Native->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale(), C->NodeWidth, Header, Reason)))
            {
                Test.TestTrue(TEXT("Measured header agrees with the native painted title"), FVector2f(Title.Right - C->NodePosX, Title.Bottom - C->NodePosY).Equals(Header.Max, 0.1f));
            }
            const auto* Direct = DirectMembers.Find(C);
            if (!Direct) { continue; }
            for (auto* Member : *Direct)
            {
                const auto Child = Panel->GetNodeWidgetFromGuid(Member->NodeGuid);
                const FVector2f Size(Child->GetDesiredSize());
                Test.TestTrue(TEXT("Nested direct members clear the painted header and all padded edges"),
                    Member->NodePosX >= C->NodePosX + 31.5f && Member->NodePosY >= Title.Bottom + 31.5f &&
                    Member->NodePosX + Size.X <= C->NodePosX + C->NodeWidth - 31.5f &&
                    Member->NodePosY + Size.Y <= C->NodePosY + C->NodeHeight - 31.5f);
            }
        }
        for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I)
        {
            auto* A = Fixture->Graph->Nodes[I].Get(); const auto WA = Panel->GetNodeWidgetFromGuid(A->NodeGuid);
            const FSlateRect RA(A->NodePosX, A->NodePosY, A->NodePosX + WA->GetDesiredSize().X, A->NodePosY + WA->GetDesiredSize().Y);
            for (int32 J = 0; J < I; ++J)
            {
                auto* B = Fixture->Graph->Nodes[J].Get();
                if (Members.FindRef(Cast<UEdGraphNode_Comment>(A)).Contains(B) || Members.FindRef(Cast<UEdGraphNode_Comment>(B)).Contains(A) ||
                    (Rigid.Contains(A) && Rigid.Contains(B))) { continue; }
                const auto WB = Panel->GetNodeWidgetFromGuid(B->NodeGuid);
                const FSlateRect RB(B->NodePosX, B->NodePosY, B->NodePosX + WB->GetDesiredSize().X, B->NodePosY + WB->GetDesiredSize().Y);
                Test.TestFalse(TEXT("Formatted native bodies/comments have no new external overlap"), FSlateRect::DoRectanglesIntersect(RA, RB));
            }
        }
    }
    void CheckMovement(UEdGraphNode_Comment* C, bool bMoveMembers)
    {
        const auto BeforeMove = SerializeNodes(*Fixture->Graph); TMap<UEdGraphNode*, FIntPoint> Positions;
        for (UEdGraphNode* Node : Fixture->Graph->Nodes) { Positions.Add(Node, {Node->NodePosX, Node->NodePosY}); }
        {
            const FScopedTransaction Transaction(FText::FromString(TEXT("GlooPrint test native comment movement")));
            SGraphNode::FNodeSet Filter;
            Editor->GetGraphPanel()->GetNodeWidgetFromGuid(C->NodeGuid)->MoveTo(C->GetPosition() + FVector2f(64, 48), Filter);
        }
        for (const auto& Pair : Positions)
        {
            const bool bMoves = Pair.Key == C || (bMoveMembers && Members.FindChecked(C).Contains(Pair.Key));
            Test.TestEqual(TEXT("Native movement moves exactly the intended original members once"), FIntPoint(Pair.Key->NodePosX, Pair.Key->NodePosY),
                Pair.Value + (bMoves ? FIntPoint(64, 48) : FIntPoint::ZeroValue));
        }
        const auto Moved = SerializeNodes(*Fixture->Graph);
        Test.TestTrue(TEXT("Native comment movement undoes normally"), GEditor->UndoTransaction());
        Test.TestTrue(TEXT("Movement undo restores the complete formatted graph"), SerializeNodes(*Fixture->Graph) == BeforeMove);
        Test.TestTrue(TEXT("Native comment movement redoes normally"), GEditor->RedoTransaction());
        Test.TestTrue(TEXT("Movement redo restores exactly the moved native group"), SerializeNodes(*Fixture->Graph) == Moved);
    }
    void Capture(const TCHAR* Stage)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture native comment graph"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        const FString Name = FString::Printf(TEXT("GlooPrint-%sComments-%s.png"), bAmbiguous ? TEXT("Overlapping") : TEXT("Nested"), Stage);
        Test.TestTrue(TEXT("Save native comment capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
    }
    UEdGraphNode_Comment* Comment(FVector2f Position, FIntPoint Size, const TCHAR* Title, FLinearColor Color)
    {
        auto* C = Fixture->Add<UEdGraphNode_Comment>(Position); C->NodeWidth = Size.X; C->NodeHeight = Size.Y;
        C->NodeComment = Title; C->CommentColor = Color; C->FontSize = 22; C->NodeDetails = FText::FromString(TEXT("Preserve these user-authored details."));
        C->bColorCommentBubble = true; Comments.Add(C); return C;
    }
    UK2Node_CallFunction* Call(FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
        Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native schema accepts comment fixture execution link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B));
    }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
        Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); Restore(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_CustomEvent* Entry = nullptr;
    TArray<UEdGraphNode_Comment*> Comments;
    TArray<UEdGraphNode*> Rigid;
    TMap<UEdGraphNode_Comment*, TArray<UEdGraphNode*>> Members, DirectMembers;
    TMap<UEdGraphNode*, FIntPoint> OriginalPositions;
    UEdGraphNode_Comment* Empty = nullptr;
    UEdGraphNode_Comment* MovementComment = nullptr;
    UEdGraphNode_Comment* NoMovementComment = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    TMap<FString, FString> Properties;
    FVector2D OriginalCursor;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0, CaptureAfter = 0;
    int32 Phase = 0, Frames = 0, Queue = 0;
    bool bAmbiguous, bOriginalEnabled = true, bRestore = false, bConnectionsValid = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedCommentEditorTest, "GlooPrint.Editor.NestedCommentMembership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNestedCommentEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FCommentCheck(*this, false)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlappingCommentEditorTest, "GlooPrint.Editor.OverlappingCommentMembership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOverlappingCommentEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FCommentCheck(*this, true)); return true;
}
}
#endif
