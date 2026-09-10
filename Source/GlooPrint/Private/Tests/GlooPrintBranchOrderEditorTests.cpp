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
#include "K2Node_SwitchInteger.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FBranchOrderCheck final : public IAutomationLatentCommand
{
public:
    explicit FBranchOrderCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FBranchOrderCheck() { Restore(); }
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
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("NestedSwitchExample");
            Switch = Fixture->Add<UK2Node_SwitchInteger>({100, 200});
            for (int32 I = 0; I < 3; ++I) { Switch->AddPinToSwitchNode(); }
            auto* Outer = Fixture->Add<UK2Node_IfThenElse>({850, 300});
            auto* Inner = Fixture->Add<UK2Node_IfThenElse>({450, 100});
            auto* InnerTrue = Print(TEXT("Inner true"));
            auto* InnerFalse = Print(TEXT("Inner false"));
            auto* OuterFalse = Print(TEXT("Outer false"));
            auto* LocalJoin = Print(TEXT("Nested join"));
            auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>({600, 650});
            auto* Then0 = Print(TEXT("Sequence zero"));
            auto* Then1 = Print(TEXT("Sequence one"));
            auto* Case2 = Print(TEXT("Case two"));
            auto* Default = Print(TEXT("Default case"));
            auto* FinalJoin = Print(TEXT("Final join"));
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            Flow(Entry, Switch); Flow(Switch, Outer, TEXT("0")); Flow(Switch, Sequence, TEXT("1"));
            Flow(Switch, Case2, TEXT("2")); Flow(Switch, Default, TEXT("Default"));
            Flow(Outer, Inner); Flow(Outer, OuterFalse, UEdGraphSchema_K2::PN_Else);
            Flow(Inner, InnerTrue); Flow(Inner, InnerFalse, UEdGraphSchema_K2::PN_Else);
            for (auto* Node : {InnerTrue, InnerFalse, OuterFalse}) { Flow(Node, LocalJoin); }
            Flow(LocalJoin, FinalJoin);
            Link(Sequence->GetThenPinGivenIndex(0), Then0->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Sequence->GetThenPinGivenIndex(1), Then1->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            for (auto* Node : {Then0, Then1, Case2, Default}) { Flow(Node, FinalJoin); }
            if (!bConnectionsValid) { return Finish(); }
            Siblings = {{Outer, Sequence, Case2, Default}, {Inner, OuterFalse}, {InnerTrue, InnerFalse}, {Then0, Then1}};
            MainFlow = {Entry, Switch, Outer, Inner, InnerTrue, LocalJoin, FinalJoin};
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Nested Branch/Switch/Sequence and joins compile as native Blueprint"), Compile.NumErrors, 0)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint nested branches, switch and joins")))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-100, -100), 1);
            Editor->SetNodeSelection(Entry, true); Slate.SetCursorPos(FVector2D::ZeroVector);
            Deadline = FPlatformTime::Seconds() + 40; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Nested branch fixture did not settle in 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Nested native graph has a complete format plan"),
                PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Nested graph planning is read-only"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestEqual(TEXT("Nested graph retains all nineteen connections"), Plan.Routes.Wires.Num(), 19);
            Test.TestEqual(TEXT("Nested graph needs no native fallback wires"), Plan.Routes.FallbackCount, 0);
            Test.AddInfo(FString::Printf(TEXT("Native nested branches: %d spacing repairs, %d fallbacks."), Plan.SpacingRepairs, Plan.Routes.FallbackCount));
            CheckSwitchGeometry(); CheckPlacement();
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats nested branches and switch"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 3; return false;
        }
        if (Phase == 3)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Nested formatting moves the scattered graph"), Before != After);
            Test.TestEqual(TEXT("Nested formatting is one undo step"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Nested layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Nested undo restores every serialized node/pin value"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Nested layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Nested redo restores every serialized node/pin value"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            Test.TestEqual(TEXT("Nested branches and shared joins keep their fourteen original nodes"), Fixture->Graph->Nodes.Num(), 14);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Native nested route agrees with its original preflight pin pair"), Live && Live->Points == Pair.Value.Points);
            }
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Nested graph replans with cold native measurement"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Nested layout is cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
            }
            Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            Test.TestTrue(TEXT("Repeated F leaves the nested graph exactly unchanged"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Nested no-op creates no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Nested F preserves camera"), AfterView, View); Test.TestEqual(TEXT("Nested F preserves zoom"), AfterZoom, Zoom);
            Editor->ZoomToFit(false); CaptureAfter = FPlatformTime::Seconds() + 1;
            Phase = 2; Frames = 0; return false;
        }
        if (FPlatformTime::Seconds() < CaptureAfter) { return false; }
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native nested branch layout"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save nested branch capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-NestedSwitch.png"))));
        }
        return Finish();
    }
private:
    const FLayoutPin& Pin(UEdGraphPin* Native) const
    {
        return *Plan.Snapshot.Pins.FindByPredicate([Native](const auto& P) { return P.Id == Native->PinId; });
    }
    float InputY(UEdGraphNode* Node) const
    {
        const auto& P = Pin(Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        return Plan.Layout.Positions[P.Node].Y + P.Offset.GetValue().Y;
    }
    void CheckSwitchGeometry()
    {
        const auto Native = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Switch->NodeGuid);
        if (!Test.TestTrue(TEXT("Switch has a native widget"), Native.IsValid())) { return; }
        TArray<TSharedRef<SWidget>> Widgets; Native->GetPins(Widgets);
        float PreviousY = -MAX_flt;
        for (const FName Name : {FName(TEXT("0")), FName(TEXT("1")), FName(TEXT("2")), FName(TEXT("Default"))})
        {
            auto* Output = Switch->FindPinChecked(Name); const auto& Measured = Pin(Output);
            const auto* Widget = Widgets.FindByPredicate([Output](const auto& W) { return StaticCastSharedRef<SGraphPin>(W)->GetPinObj() == Output; });
            if (!Test.TestTrue(TEXT("Switch output has native painted geometry"), Widget != nullptr)) { continue; }
            const auto& Geometry = (*Widget)->GetCachedGeometry(); const FVector2f Size(Geometry.GetLocalSize());
            const FVector2f Point = Native->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2f(Size.X, Size.Y * 0.5f)));
            Test.TestTrue(TEXT("Switch output measurement matches its native attachment"), Measured.Offset.GetValue().Equals(Point, 0.1f));
            Test.TestTrue(TEXT("Native Switch displays numbered cases before Default"), Point.Y > PreviousY); PreviousY = Point.Y;
            Test.TestEqual(TEXT("Snapshot preserves original pin array order"), Measured.Ordinal, Switch->Pins.IndexOfByKey(Output));
            Test.AddInfo(FString::Printf(TEXT("Switch %s: original ordinal %d, native Y %.1f, branch input Y %.1f."),
                *Name.ToString(), Measured.Ordinal, Point.Y, InputY(Output->LinkedTo[0]->GetOwningNode())));
        }
        Test.TestEqual(TEXT("Fixture exercises Default stored first despite being displayed last"), Switch->Pins.IndexOfByKey(Switch->GetDefaultPin()), 0);
    }
    void CheckPlacement()
    {
        for (const auto& Group : Siblings)
        {
            for (int32 I = 1; I < Group.Num(); ++I)
            {
                Test.TestTrue(TEXT("Nested siblings follow visible output order"), InputY(Group[I]) > InputY(Group[I - 1]));
            }
        }
        for (int32 I = 1; I < MainFlow.Num(); ++I)
        {
            const auto& Output = Pin(MainFlow[I - 1]->FindPinChecked(I == 2 ? FName(TEXT("0")) : UEdGraphSchema_K2::PN_Then));
            const float Delta = Plan.Layout.Positions[Output.Node].Y + Output.Offset.GetValue().Y - InputY(MainFlow[I]);
            Test.TestTrue(TEXT("Nested primary execution attachments align"), FMath::Abs(Delta) <= 0.5f);
        }
        for (const auto& Edge : Plan.Snapshot.Edges)
        {
            const int32 A = Plan.Snapshot.Pins[Edge.From].Node, B = Plan.Snapshot.Pins[Edge.To].Node;
            Test.TestTrue(TEXT("Nested joins sit after every incoming branch with body clearance"),
                Plan.Layout.Positions[A].X + Plan.Snapshot.Nodes[A].Geometry.VisualBounds.Max.X + 95.5f <=
                Plan.Layout.Positions[B].X + Plan.Snapshot.Nodes[B].Geometry.VisualBounds.Min.X);
        }
        for (int32 A = 0; A < Plan.Snapshot.Nodes.Num(); ++A)
        {
            const auto& GA = Plan.Snapshot.Nodes[A].Geometry;
            const FBox2f BA(FVector2f(Plan.Layout.Positions[A]) + GA.VisualBounds.Min, FVector2f(Plan.Layout.Positions[A]) + GA.VisualBounds.Max);
            for (int32 B = 0; B < A; ++B)
            {
                const auto& GB = Plan.Snapshot.Nodes[B].Geometry;
                const FBox2f BB(FVector2f(Plan.Layout.Positions[B]) + GB.VisualBounds.Min, FVector2f(Plan.Layout.Positions[B]) + GB.VisualBounds.Max);
                Test.TestFalse(TEXT("Nested measured visual bounds never overlap"), BA.Intersect(BB));
            }
        }
        FString Reason; FLayoutResult Original, Shuffled;
        if (!Test.TestTrue(TEXT("Nested value layout computes"), ComputeLayout(Plan.Snapshot, {}, Original, Reason))) { Test.AddError(Reason); return; }
        FLayoutGraph Graph = Plan.Snapshot; Algo::Reverse(Graph.Edges);
        for (auto& Node : Graph.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
        for (int32 I = 0; I < Graph.Edges.Num(); ++I)
        {
            const auto& E = Graph.Edges[I]; Graph.Nodes[Graph.Pins[E.From].Node].Outgoing.Add(I); Graph.Nodes[Graph.Pins[E.To].Node].Incoming.Add(I);
        }
        Test.TestTrue(TEXT("Reversed native edge enumeration computes"), ComputeLayout(Graph, {}, Shuffled, Reason));
        Test.TestTrue(TEXT("Nested branch placement ignores edge enumeration order"), Shuffled.Positions == Original.Positions);
    }
    UK2Node_CallFunction* Print(const TCHAR* Text)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
        const int32 I = Fixture->Graph->Nodes.Num(); Fixture->Initialize(*Node, {float(150 + I % 3 * 300), float(50 + I * 60)});
        Node->PostPlacedNewNode(); Node->FindPinChecked(TEXT("InString"))->DefaultValue = Text; return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native schema accepts nested fixture link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B));
    }
    void Flow(UEdGraphNode* A, UEdGraphNode* B, FName Output = UEdGraphSchema_K2::PN_Then)
    {
        Link(A->FindPinChecked(Output), B->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
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
    UK2Node_SwitchInteger* Switch = nullptr;
    TArray<TArray<UEdGraphNode*>> Siblings;
    TArray<UEdGraphNode*> MainFlow;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    FVector2D OriginalCursor;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0, CaptureAfter = 0;
    int32 Phase = 0, Frames = 0, Queue = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBranchOrderEditorTest, "GlooPrint.Editor.NestedBranchesSwitchAndJoins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBranchOrderEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FBranchOrderCheck(*this)); return true;
}
}
#endif
