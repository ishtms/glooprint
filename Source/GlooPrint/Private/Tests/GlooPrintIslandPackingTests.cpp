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
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FIslandPackingCheck final : public IAutomationLatentCommand
{
public:
    explicit FIslandPackingCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FIslandPackingCheck()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        if (bRestore)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
            Settings->CommentPadding = OriginalSettings.CommentPadding;
            Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled;
            Settings->NotifyChanged();
        }
    }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("PackingExample");
            Groups.Add({Entry});
            UEdGraphNode* Previous = Entry;
            for (int32 I = 0; I < 3; ++I)
            {
                auto* Node = Call(TEXT("PrintString"), {float(-400 - I * 300), float(200 + I * 150)});
                Flow(Previous, Node); Previous = Node; Groups[0].Add(Node);
            }
            auto* LoopA = Fixture->Add<UK2Node_IfThenElse>({900, -300});
            auto* LoopB = Fixture->Add<UK2Node_IfThenElse>({400, -600});
            Flow(Previous, LoopA); Flow(LoopA, LoopB); Flow(LoopB, LoopA);
            Groups[0].Append({LoopA, LoopB});
            for (int32 I = 0; I < 3; ++I)
            {
                auto* Literal = Call(TEXT("MakeLiteralString"), {float(200 + I * 300), 700});
                Literal->FindPinChecked(TEXT("Value"))->DefaultValue = FString::Printf(TEXT("Independent value %d"), I + 1);
                Groups.Add({Literal});
            }
            auto* First = Call(TEXT("MakeLiteralString"), {1600, 400});
            auto* Second = Call(TEXT("MakeLiteralInt"), {1600, 750});
            Inner = Comment({1500, 200}, {900, 850}, TEXT("Independent values"));
            Outer = Comment({1400, 100}, {1100, 1100}, TEXT("Keep the nested group together"));
            Groups.Add({First, Second, Inner, Outer});
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            if (!bConnectionsValid) { return true; }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Packing fixture compiles without executing its feedback loop"), Compile.NumErrors, 0)) { return true; }
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint disconnected packing")))
                .ClientSize(FVector2f(1500, 1050))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetNodeSelection(Inner, true); Editor->SetNodeSelection(Outer, true);
            Editor->SetNodeSelection(Entry, true);
            Editor->SetViewLocation(FVector2f(-150, -180), 0.5f);
            Deadline = FPlatformTime::Seconds() + 45; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Packing fixture did not settle.")); return true; }
        if (++Frames < 12) { return false; }
        FString Reason;
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        auto* Panel = Editor->GetGraphPanel();
        if (Phase == 3)
        {
            Test.TestTrue(TEXT("Repeated F leaves packed graph unchanged"), After == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Repeated F creates no extra undo entry"), GEditor->Trans->GetQueueLength(), Queue);
            const auto InnerWidget = Panel->GetNodeWidgetFromGuid(Inner->NodeGuid);
            const auto OuterWidget = Panel->GetNodeWidgetFromGuid(Outer->NodeGuid);
            if (Test.TestTrue(TEXT("Nested comments have normal-detail native widgets"), InnerWidget && OuterWidget))
            {
                Test.TestTrue(TEXT("Nested comment clears the painted outer title"), Inner->NodePosY >= OuterWidget->GetTitleRect().Bottom + 31.5f);
                for (int32 I = 0; I < 2; ++I)
                {
                    Test.TestTrue(TEXT("Packed values clear the painted inner title"), Groups[4][I]->NodePosY >= InnerWidget->GetTitleRect().Bottom + 31.5f);
                }
            }
            Capture(TEXT("GlooPrint-IslandPacking-Comments.png"));
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset(); return true;
        }
        if (Phase == 0)
        {
            if (!bPreparedSelection)
            {
                Test.TestEqual(TEXT("Native initial inner membership is populated"), Inner->GetNodesUnderComment().Num(), 2);
                Test.TestEqual(TEXT("Native initial outer membership is populated"), Outer->GetNodesUnderComment().Num(), 3);
                Editor->ClearSelectionSet(); Editor->SetNodeSelection(Entry, true);
                bPreparedSelection = true; Frames = 0; return false;
            }
            Before = SerializeNodes(*Fixture->Graph);
            BeforeValues = SerializeTransactionValues(*Fixture->Graph);
            Properties = DescribeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Disconnected groups plan with comments and feedback wires"),
                PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return true; }
            Test.TestTrue(TEXT("Packing plan leaves graph bytes unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Every original execution and feedback edge is retained"), Plan.Routes.Wires.Num(), 6);
            Test.TestEqual(TEXT("Packed groups need no native routing fallback"), Plan.Routes.FallbackCount, 0);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F packs the entire graph"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 4; return false;
        }
        if (Phase == 4)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            Test.TestTrue(TEXT("Packing changes the original layout"), Before != SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Packing has exactly one native undo step"), GEditor->Trans->GetQueueLength(), Queue + 1);
            After = SerializeNodes(*Fixture->Graph);
            const auto AfterValues = SerializeTransactionValues(*Fixture->Graph);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY")) ||
                    Pair.Key.EndsWith(TEXT(".NodeWidth")) || Pair.Key.EndsWith(TEXT(".NodeHeight"))) { continue; }
                const auto* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" survives packing"), Value && *Value == Pair.Value);
            }
            Test.TestEqual(TEXT("Packing preserves the selected anchor position"), FIntPoint(Entry->NodePosX, Entry->NodePosY), FIntPoint(0, 0));
            Test.TestTrue(TEXT("Packing preserves the exact selection"), Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Entry));
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestTrue(TEXT("Packing preserves the camera"), View == AfterView && Zoom == AfterZoom);
            Test.TestTrue(TEXT("Packing undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Packing undo restores every serialized value"), BeforeValues == SerializeTransactionValues(*Fixture->Graph));
            Test.TestTrue(TEXT("Packing redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Packing redo restores every serialized value"), AfterValues == SerializeTransactionValues(*Fixture->Graph));
            Editor->SetNodeSelection(Inner, true); Editor->SetNodeSelection(Outer, true);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            Editor->ClearSelectionSet(); Editor->SetNodeSelection(Entry, true);
            Phase = 2; Frames = 0; return false;
        }
        FFormatPlan Cold;
        if (Test.TestTrue(TEXT("Packed graph plans identically with cold geometry"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
        {
            Test.TestTrue(TEXT("Packed positions and comment sizes are cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.Sizes == Plan.Layout.Sizes);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key); const auto* Fresh = Cold.Routes.Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Every original live wire matches preflight and cold routes"),
                    Live && Fresh && Live->Points == Pair.Value.Points && Fresh->Points == Pair.Value.Points &&
                    Live->Fallback == ERouteFallback::None && Fresh->Fallback == ERouteFallback::None);
            }
        }
        else { Test.AddError(Reason); }
        TMap<FGuid, int32> GroupOf;
        TArray<FBox2f> Bounds; Bounds.Init(FBox2f(ForceInit), Groups.Num());
        for (int32 G = 0; G < Groups.Num(); ++G)
        {
            for (UEdGraphNode* Node : Groups[G])
            {
                GroupOf.Add(Node->NodeGuid, G);
                const int32 I = Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; });
                const auto& Geometry = Plan.Snapshot.Nodes[I].Geometry;
                const FVector2f Position(float(Node->NodePosX), float(Node->NodePosY));
                if (Plan.Snapshot.Nodes[I].bComment) { Bounds[G] += Position; Bounds[G] += Position + FVector2f(Plan.Layout.Sizes[I]); }
                else { Bounds[G] += Position + Geometry.VisualBounds.Min; Bounds[G] += Position + Geometry.VisualBounds.Max; }
            }
        }
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const int32 G = GroupOf.FindChecked(Pair.Key.FromNode);
            Test.TestEqual(TEXT("Packed wire still connects within its original island"), GroupOf.FindChecked(Pair.Key.ToNode), G);
            Bounds[G] += Pair.Value.Bounds.Min; Bounds[G] += Pair.Value.Bounds.Max;
        }
        for (int32 A = 0; A < Bounds.Num(); ++A)
        {
            for (int32 B = 0; B < A; ++B)
            {
                Test.TestFalse(TEXT("Packed visual and actual wire envelopes remain separate"),
                    Bounds[A].Min.X < Bounds[B].Max.X && Bounds[A].Max.X > Bounds[B].Min.X &&
                    Bounds[A].Min.Y < Bounds[B].Max.Y && Bounds[A].Max.Y > Bounds[B].Min.Y);
            }
        }
        Test.TestEqual(TEXT("Independent small groups share a compact row"), Groups[1][0]->NodePosY, Groups[2][0]->NodePosY);
        Test.TestTrue(TEXT("Stable island order reads left to right"), Groups[1][0]->NodePosX < Groups[2][0]->NodePosX);
        Test.TestEqual(TEXT("Inner comment retains its two original members"), Inner->GetNodesUnderComment().Num(), 2);
        Test.TestEqual(TEXT("Outer comment retains its nested comment and two members"), Outer->GetNodesUnderComment().Num(), 3);
        for (int32 I = 0; I < 2; ++I)
        {
            Test.TestTrue(TEXT("Both native comments retain their exact original value nodes"),
                Inner->GetNodesUnderComment().Contains(Groups[4][I]) && Outer->GetNodesUnderComment().Contains(Groups[4][I]));
        }
        Test.TestTrue(TEXT("Outer comment retains the original inner comment"), Outer->GetNodesUnderComment().Contains(Inner));
        Queue = GEditor->Trans->GetQueueLength();
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        Capture(TEXT("GlooPrint-IslandPacking.png"));
        FBox2f All(ForceInit); for (const auto& Box : Bounds) { All += Box.Min; All += Box.Max; }
        Test.AddInfo(FString::Printf(TEXT("Native packed visual/wire envelope: %.0f x %.0f graph units, %d repairs, %d fallbacks."),
            All.Max.X - All.Min.X, All.Max.Y - All.Min.Y, Plan.SpacingRepairs, Plan.Routes.FallbackCount));
        Editor->SetViewLocation(FVector2f(Outer->NodePosX - 60, Outer->NodePosY - 80), 1.f);
        Phase = 3; Frames = 0; return false;
    }
private:
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture actual native packing result"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save packing capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    UK2Node_CallFunction* Call(FName Function, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Function));
        Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Flow(UEdGraphNode* From, UEdGraphNode* To)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native K2 accepts fixture execution edge"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
            From->FindPinChecked(UEdGraphSchema_K2::PN_Then), To->FindPinChecked(UEdGraphSchema_K2::PN_Execute)));
    }
    UEdGraphNode_Comment* Comment(FVector2f Position, FIntPoint Size, const TCHAR* Text)
    {
        auto* Node = Fixture->Add<UEdGraphNode_Comment>(Position);
        Node->NodeWidth = Size.X; Node->NodeHeight = Size.Y; Node->NodeComment = Text; return Node;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_CustomEvent* Entry = nullptr;
    UEdGraphNode_Comment* Inner = nullptr;
    UEdGraphNode_Comment* Outer = nullptr;
    TArray<TArray<UEdGraphNode*>> Groups;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, BeforeValues, After;
    TMap<FString, FString> Properties;
    FVector2f View;
    float Zoom = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true, bPreparedSelection = false;
    int32 Frames = 0, Phase = 0, Queue = 0;
    double Deadline = 0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandPackingEditorTest, "GlooPrint.Editor.DisconnectedGroupPacking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIslandPackingEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FIslandPackingCheck(*this)); return true;
}
}
#endif
