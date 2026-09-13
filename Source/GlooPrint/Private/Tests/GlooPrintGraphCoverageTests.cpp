// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintWireDrawing.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineSchema.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/AsyncActionHandleSaveGame.h"
#include "ImageUtils.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SpawnActorFromClass.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
struct FGraphCoverageFixture
{
    FFixture Actor {AActor::StaticClass(), false};
    TStrongObjectPtr<UBlueprint> Animation;
    TArray<UEdGraph*> Targets;
    TArray<UEdGraph*> AllGraphs;
    TStrongObjectPtr<UEdGraph> Pose;
    TStrongObjectPtr<UEdGraph> StateMachine;
    UK2Node_CallFunction* Decorated = nullptr;
    UK2Node_AsyncAction* Async = nullptr;
    UK2Node_SpawnActorFromClass* Spawn = nullptr;
    UK2Node_FormatText* FormatText = nullptr;
    UK2Node_CallFunction* ErrorDecorated = nullptr;
    TMap<FGuid, FIntPoint> InitialPositions;
    bool bConnectionsValid = true;

    UK2Node_CallFunction* Call(UClass* Library, FName Function, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Actor.Graph);
        Node->SetFromFunction(Library->FindFunctionByName(Function));
        Actor.Initialize(*Node, Position);
        Node->PostPlacedNewNode();
        return Node;
    }
    UK2Node_CallFunction* Print(FVector2f Position)
    {
        return Call(UKismetSystemLibrary::StaticClass(), TEXT("PrintString"), Position);
    }
    void Link(UEdGraphPin* From, UEdGraphPin* To)
    {
        bConnectionsValid &= From && To && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To);
    }
    void Flow(UEdGraphNode* From, UEdGraphNode* To)
    {
        Link(From->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), To->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
    }
    UEdGraph* Function(FName Name)
    {
        auto* Graph = FBlueprintEditorUtils::CreateNewGraph(Actor.Blueprint.Get(), Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(Actor.Blueprint.Get(), Graph, true, nullptr);
        return Graph;
    }
    void TunnelBody(UEdGraph* Graph)
    {
        Actor.Graph = Graph;
        UK2Node_Tunnel* Entry = nullptr;
        UK2Node_Tunnel* Exit = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node))
            {
                if (Tunnel->bCanHaveOutputs) { Entry = Tunnel; }
                if (Tunnel->bCanHaveInputs) { Exit = Tunnel; }
            }
        }
        check(Entry && Exit);
        FEdGraphPinType Exec; Exec.PinCategory = UEdGraphSchema_K2::PC_Exec;
        Entry->CreateUserDefinedPin(UEdGraphSchema_K2::PN_Execute, Exec, EGPD_Output);
        Exit->CreateUserDefinedPin(UEdGraphSchema_K2::PN_Then, Exec, EGPD_Input);
        auto* Body = Print(FVector2f(-300, 170));
        Link(Entry->FindPin(UEdGraphSchema_K2::PN_Execute), Body->FindPin(UEdGraphSchema_K2::PN_Execute));
        Link(Body->FindPin(UEdGraphSchema_K2::PN_Then), Exit->FindPin(UEdGraphSchema_K2::PN_Then));
        Targets.Add(Graph);
    }
    FGraphCoverageFixture()
    {
        UEdGraph* Event = Actor.Graph;
        Targets.Add(Event);
        UEdGraph* Callback = Function(TEXT("TimerCallback"));
        UEdGraph* FunctionGraph = Function(TEXT("FunctionWithTwoReturns"));
        Actor.Graph = FunctionGraph;
        auto* Branch = Actor.Add<UK2Node_IfThenElse>(FVector2f(-300, 300));
        auto* ReturnA = Actor.Add<UK2Node_FunctionResult>(FVector2f(-550, -180)); ReturnA->PostPlacedNewNode();
        auto* ReturnB = Actor.Add<UK2Node_FunctionResult>(FVector2f(50, 500)); ReturnB->PostPlacedNewNode();
        Flow(FunctionGraph->Nodes[0], Branch);
        Link(Branch->FindPin(UEdGraphSchema_K2::PN_Then), ReturnA->FindPin(UEdGraphSchema_K2::PN_Execute));
        Link(Branch->FindPin(UEdGraphSchema_K2::PN_Else), ReturnB->FindPin(UEdGraphSchema_K2::PN_Execute));
        Targets.Add(FunctionGraph);

        UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(Actor.Blueprint.Get(), TEXT("MacroExample"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddMacroGraph(Actor.Blueprint.Get(), Macro, true, nullptr);
        TunnelBody(Macro);
        Actor.Graph = Event;
        auto* Composite = Actor.Add<UK2Node_Composite>(FVector2f(700, 700));
        Composite->PostPlacedNewNode(); Composite->OnRenameNode(TEXT("CollapsedExample"));
        TunnelBody(Composite->BoundGraph);

        UEdGraph* Construction = FBlueprintEditorUtils::FindUserConstructionScript(Actor.Blueprint.Get());
        check(Construction);
        Actor.Graph = Construction;
        auto* ConstructionPrint = Print(FVector2f(-300, 180));
        Flow(Construction->Nodes[0], ConstructionPrint);
        Targets.Add(Construction);

        Actor.Graph = Event;
        auto* Entry = Actor.Add<UK2Node_CustomEvent>(FVector2f(0, 0)); Entry->CustomFunctionName = TEXT("FormatExample");
        auto* Sequence = Actor.Add<UK2Node_ExecutionSequence>(FVector2f(600, 260));
        Sequence->AddInputPin(); Sequence->AddInputPin();
        Flow(Entry, Sequence);
        auto* First = Print(FVector2f(-400, 0));
        auto* Second = Print(FVector2f(-400, 450));
        auto* Shared = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralString"), FVector2f(250, -300));
        Shared->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("One shared value");
        Link(Shared->FindPin(UEdGraphSchema_K2::PN_ReturnValue), First->FindPin(TEXT("InString")));
        Link(Shared->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Second->FindPin(TEXT("InString")));
        Link(Sequence->GetThenPinGivenIndex(0), First->FindPin(UEdGraphSchema_K2::PN_Execute));
        Link(Sequence->GetThenPinGivenIndex(1), Second->FindPin(UEdGraphSchema_K2::PN_Execute));
        Link(Sequence->GetThenPinGivenIndex(2), Composite->FindPin(UEdGraphSchema_K2::PN_Execute));
        auto* MacroInstance = NewObject<UK2Node_MacroInstance>(Event);
        MacroInstance->SetMacroGraph(Macro); Actor.Initialize(*MacroInstance, FVector2f(400, 600));
        Link(Sequence->GetThenPinGivenIndex(3), MacroInstance->FindPin(UEdGraphSchema_K2::PN_Execute));
        auto* Delay = Call(UKismetSystemLibrary::StaticClass(), TEXT("Delay"), FVector2f(-100, 850));
        Flow(First, Delay);
        auto* Timer = Call(UKismetSystemLibrary::StaticClass(), TEXT("K2_SetTimerDelegate"), FVector2f(350, 1000));
        Timer->FindPinChecked(TEXT("Time"))->DefaultValue = TEXT("1.0");
        Flow(Delay, Timer);
        auto* Delegate = Actor.Add<UK2Node_CreateDelegate>(FVector2f(-200, 1100));
        Delegate->SetFunction(Callback->GetFName());
        Link(Delegate->GetDelegateOutPin(), Timer->FindPin(TEXT("Delegate")));
        auto* Location = Call(AActor::StaticClass(), TEXT("K2_GetActorLocation"), FVector2f(900, -400));
        GetDefault<UEdGraphSchema_K2>()->SplitPin(Location->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue));
        Actor.Add<UK2Node_Knot>(FVector2f(1600, -900));
        Decorated = Second;

        UEdGraph* Pure = FBlueprintEditorUtils::CreateNewGraph(Actor.Blueprint.Get(), TEXT("PureOnly"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Actor.Blueprint.Get(), Pure); Actor.Graph = Pure;
        auto* Literal = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralInt"), FVector2f(400, 350));
        auto* Add = Call(UKismetMathLibrary::StaticClass(), TEXT("Add_IntInt"), FVector2f(-400, 180));
        auto* String = Call(UKismetStringLibrary::StaticClass(), TEXT("Conv_IntToString"), FVector2f(-150, -200));
        Link(Literal->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Add->FindPin(TEXT("A")));
        Link(Add->FindPin(UEdGraphSchema_K2::PN_ReturnValue), String->FindPin(TEXT("InInt")));
        Targets.Add(Pure);

        UEdGraph* AsyncGraph = FBlueprintEditorUtils::CreateNewGraph(Actor.Blueprint.Get(), TEXT("AsyncCompletion"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Actor.Blueprint.Get(), AsyncGraph); Actor.Graph = AsyncGraph;
        auto* AsyncEntry = Actor.Add<UK2Node_CustomEvent>(FVector2f(0, 0)); AsyncEntry->CustomFunctionName = TEXT("AsyncLayoutExample");
        Async = NewObject<UK2Node_AsyncAction>(AsyncGraph);
        Async->InitializeProxyFromFunction(UAsyncActionHandleSaveGame::StaticClass()->FindFunctionByName(TEXT("AsyncLoadGameFromSlot")));
        Actor.Initialize(*Async, FVector2f(640, 220)); Async->PostPlacedNewNode();
        Async->FindPinChecked(TEXT("SlotName"))->DefaultValue = TEXT("GlooPrint_Measurement_Only");
        auto* Started = Print(FVector2f(-400, -100));
        Started->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("Started");
        auto* Completed = Actor.Add<UK2Node_IfThenElse>(FVector2f(-400, 420));
        auto* Success = Print(FVector2f(250, 680));
        auto* Failed = Print(FVector2f(550, 500));
        Failed->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("Failed");
        auto* ObjectName = Call(UKismetSystemLibrary::StaticClass(), TEXT("GetDisplayName"), FVector2f(-100, 850));
        Flow(AsyncEntry, Async); Flow(Async, Started);
        Link(Async->FindPin(TEXT("Completed")), Completed->FindPin(UEdGraphSchema_K2::PN_Execute));
        Link(Async->FindPin(TEXT("bSuccess")), Completed->FindPin(UEdGraphSchema_K2::PN_Condition));
        Link(Async->FindPin(TEXT("SaveGame")), ObjectName->FindPin(TEXT("Object")));
        Link(ObjectName->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Success->FindPin(TEXT("InString")));
        Flow(Completed, Success);
        Link(Completed->FindPin(UEdGraphSchema_K2::PN_Else), Failed->FindPin(UEdGraphSchema_K2::PN_Execute));
        Targets.Add(AsyncGraph);

        UEdGraph* CustomGraph = FBlueprintEditorUtils::CreateNewGraph(Actor.Blueprint.Get(), TEXT("CustomWidgets"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Actor.Blueprint.Get(), CustomGraph); Actor.Graph = CustomGraph;
        auto* CustomEntry = Actor.Add<UK2Node_CustomEvent>(FVector2f(0, 0)); CustomEntry->CustomFunctionName = TEXT("CustomWidgetLayoutExample");
        Spawn = Actor.Add<UK2Node_SpawnActorFromClass>(FVector2f(700, 250)); Spawn->PostPlacedNewNode();
        GetDefault<UEdGraphSchema_K2>()->TrySetDefaultObject(*Spawn->GetClassPin(), AActor::StaticClass());
        auto* Transform = Call(UKismetMathLibrary::StaticClass(), TEXT("MakeTransform"), FVector2f(-400, -300));
        auto* SpawnedName = Call(UKismetSystemLibrary::StaticClass(), TEXT("GetDisplayName"), FVector2f(-300, 900));
        auto* Count = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralInt"), FVector2f(800, -200));
        Count->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("3");
        FormatText = Actor.Add<UK2Node_FormatText>(FVector2f(100, 600));
        GetDefault<UEdGraphSchema_K2>()->TrySetDefaultText(*FormatText->GetFormatPin(),
            FText::FromString(TEXT("Spawned {Name}\nCount: {Count}")));
        ErrorDecorated = Call(UKismetSystemLibrary::StaticClass(), TEXT("PrintText"), FVector2f(-400, 300));
        Flow(CustomEntry, Spawn); Flow(Spawn, ErrorDecorated);
        Link(Transform->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Spawn->FindPin(TEXT("SpawnTransform")));
        Link(Spawn->GetResultPin(), SpawnedName->FindPin(TEXT("Object")));
        Link(SpawnedName->FindPin(UEdGraphSchema_K2::PN_ReturnValue), FormatText->FindArgumentPin(TEXT("Name")));
        Link(Count->FindPin(UEdGraphSchema_K2::PN_ReturnValue), FormatText->FindArgumentPin(TEXT("Count")));
        Link(FormatText->FindPin(TEXT("Result")), ErrorDecorated->FindPin(TEXT("InText")));
        Targets.Add(CustomGraph);

        Animation.Reset(FKismetEditorUtilities::CreateBlueprint(UAnimInstance::StaticClass(), GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UAnimBlueprint::StaticClass(), TEXT("GlooPrintAnimCoverage")), BPTYPE_Normal,
            UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
        Actor.Graph = FBlueprintEditorUtils::CreateNewGraph(Animation.Get(), TEXT("K2EventCoverage"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Animation.Get(), Actor.Graph);
        auto* AnimEntry = Actor.Add<UK2Node_CustomEvent>(FVector2f(100, 100)); AnimEntry->CustomFunctionName = TEXT("AnimEventCoverage");
        Flow(AnimEntry, Print(FVector2f(-200, 250))); Targets.Add(Actor.Graph);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Animation.Get());
        AnimEntry->UpdateDelegatePin(true);
        Pose.Reset(FBlueprintEditorUtils::CreateNewGraph(Animation.Get(), TEXT("UnsupportedPose"), UEdGraph::StaticClass(), UAnimationGraphSchema::StaticClass()));
        StateMachine.Reset(FBlueprintEditorUtils::CreateNewGraph(Animation.Get(), TEXT("UnsupportedStateMachine"), UEdGraph::StaticClass(), UAnimationStateMachineSchema::StaticClass()));
        Actor.Blueprint->GetAllGraphs(AllGraphs); Animation->GetAllGraphs(AllGraphs);
    }
};

class FGraphCoverageCheck final : public IAutomationLatentCommand
{
public:
    explicit FGraphCoverageCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            Fixture = MakeUnique<FGraphCoverageFixture>();
            if (!Test.TestTrue(TEXT("Fixture connections accepted by native K2 schema"), Fixture->bConnectionsValid)) { return true; }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Actor.Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Actor coverage fixture compiles before formatting"), Compile.NumErrors, 0)) { return true; }
            FString AsyncTooltip;
            Fixture->Async->GetPinHoverText(*Fixture->Async->FindPinChecked(TEXT("SlotName")), AsyncTooltip);
            Test.TestFalse(TEXT("Native async pin tooltip initializes before graph baselines"), AsyncTooltip.IsEmpty());
            Test.TestFalse(TEXT("Native async node supplies its corner decoration"), Fixture->Async->GetCornerIcon().IsNone());
            Test.TestEqual(TEXT("Async completion retains a typed object output"), Fixture->Async->FindPinChecked(TEXT("SaveGame"))->PinType.PinCategory, UEdGraphSchema_K2::PC_Object);
            Test.TestEqual(TEXT("Async completion retains a typed success output"), Fixture->Async->FindPinChecked(TEXT("bSuccess"))->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
            Fixture->Decorated->bHasCompilerMessage = true;
            Fixture->Decorated->ErrorType = EMessageSeverity::Warning;
            Fixture->Decorated->ErrorMsg = TEXT("Representative warning decoration\nwith a second line");
            for (UEdGraph* Graph : Fixture->Targets)
            {
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    Fixture->InitialPositions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        FString Tooltip;
                        Node->GetPinHoverText(*Pin, Tooltip);
                    }
                }
            }
            Fixture->ErrorDecorated->bHasCompilerMessage = true;
            Fixture->ErrorDecorated->ErrorType = EMessageSeverity::Error;
            Fixture->ErrorDecorated->ErrorMsg = TEXT("Representative error decoration\nwith a second line");
            Test.TestEqual(TEXT("Native format pattern creates both named argument pins"), Fixture->FormatText->GetArgumentCount(), 2);
            Test.TestEqual(TEXT("Format Text infers the connected string argument"), Fixture->FormatText->FindArgumentPin(TEXT("Name"))->PinType.PinCategory, UEdGraphSchema_K2::PC_String);
            Test.TestEqual(TEXT("Format Text infers the connected integer argument"), Fixture->FormatText->FindArgumentPin(TEXT("Count"))->PinType.PinCategory, UEdGraphSchema_K2::PC_Int);
            Test.TestTrue(TEXT("Spawn class selection survives compilation"), Fixture->Spawn->GetClassPin()->DefaultObject == AActor::StaticClass());
            FString Reason; int32 Changed = 0;
            for (UEdGraph* Unsupported : {Fixture->Pose.Get(), Fixture->StateMachine.Get()})
            {
                Test.TestFalse(TEXT("Animation pose/state schema rejects formatting"), FormatGraph(Unsupported, 1, {}, Changed, Reason));
                Test.TestFalse(TEXT("Unsupported schema supplies reason"), Reason.IsEmpty());
            }
            OpenTarget(); return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Supported graph visibility case did not settle in 60 seconds.")); return Finish(); }
        if (++Frames < 8) { return false; }
        UEdGraph* Graph = Fixture->Targets[Target];
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            CheckSelection(TEXT("BeforeMeasurement offscreen measurement"));
            const auto BeforeMeasurement = SerializeNodes(*Graph);
            if (!Test.TestTrue(Graph->GetName() + TEXT(" measures initially offscreen"), MeasureGraph(Graph, Scale, Measurement, Reason, Options)))
            {
                Test.AddError(Reason); return Finish();
            }
            Test.TestTrue(TEXT("Wide native measurement leaves all node and pin data unchanged"), BeforeMeasurement == SerializeNodes(*Graph));
            {
                FFormatPlan Plan;
                if (!Test.TestTrue(TEXT("Initially offscreen pin-mode graph plans formatting"),
                    PlanFormatGraph(Graph, Scale, {Graph->Nodes[0]->NodeGuid}, Plan, Reason, Options)))
                {
                    Test.AddError(Reason); return Finish();
                }
                OffscreenPositions.Reset();
                for (int32 I = 0; I < Plan.Snapshot.Nodes.Num(); ++I)
                {
                    OffscreenPositions.Add(Plan.Snapshot.Nodes[I].Geometry.Id, Plan.Layout.Positions[I]);
                }
                Test.TestTrue(TEXT("Offscreen pin-mode planning preserves all original graph values"), BeforeMeasurement == SerializeNodes(*Graph));
            }
            CheckSelection(TEXT("After offscreen measurement"));
            NodeIndex = 0; PanToNode(); Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            UEdGraphNode* Node = Graph->Nodes[NodeIndex];
            const auto Native = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Node->NodeGuid);
            const FMeasuredNode& Measured = Find(Measurement, Node->NodeGuid);
            if (Test.TestTrue(TEXT("Target native widget exists"), Native.IsValid()))
            {
                Test.TestTrue(Node->GetName() + TEXT(" measured body matches painted native body"),
                    Measured.BodySize.Equals(FVector2f(Native->GetCachedGeometry().GetLocalSize()), 0.1f));
                if (Node == Fixture->Async)
                {
                    TArray<FOverlayBrushInfo> Overlays;
                    Native->GetOverlayBrushes(false, FVector2f(Native->GetCachedGeometry().GetLocalSize()), Overlays);
                    Test.TestFalse(TEXT("Painted async node has its native clock overlay"), Overlays.IsEmpty());
                    for (const auto& Overlay : Overlays)
                    {
                        if (!Test.TestNotNull(TEXT("Native async overlay has a brush"), Overlay.Brush)) { continue; }
                        const FVector2f Min(Overlay.OverlayOffset);
                        const FVector2f Max = Min + FVector2f(Overlay.Brush->ImageSize);
                        Test.TestTrue(TEXT("Measured visual clearance includes the painted async corner icon"),
                            Measured.VisualBounds.Min.X <= Min.X + 0.1f && Measured.VisualBounds.Min.Y <= Min.Y + 0.1f &&
                            Measured.VisualBounds.Max.X >= Max.X - 0.1f && Measured.VisualBounds.Max.Y >= Max.Y - 0.1f);
                    }
                }
                if (Node == Fixture->Spawn)
                {
                    Test.TestEqual(TEXT("Spawn Actor uses its specialized native widget"), Native->GetType(), FName(TEXT("SGraphNodeSpawnActorFromClass")));
                }
                if (Node == Fixture->FormatText)
                {
                    Test.TestEqual(TEXT("Format Text uses its specialized native widget"), Native->GetType(), FName(TEXT("SGraphNodeFormatText")));
                }
                if (Node == Fixture->ErrorDecorated || Node == Fixture->Decorated)
                {
                    bool bPaintedDecoration = false;
                    TArray<TSharedRef<SWidget>> Widgets {Native.ToSharedRef()};
                    for (int32 I = 0; I < Widgets.Num() && I < 16384; ++I)
                    {
                        const auto Widget = Widgets[I];
                        if (!Widget->GetVisibility().IsVisible()) { continue; }
                        if (Widget->GetType() == TEXT("SErrorText"))
                        {
                            const auto& Geometry = Widget->GetCachedGeometry();
                            const FVector2f Size(Geometry.GetLocalSize());
                            if (Size.X > 0 && Size.Y > 0)
                            {
                                bPaintedDecoration = true;
                                const FVector2f Min = Native->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2f::ZeroVector));
                                const FVector2f Max = Native->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(Size));
                                Test.TestTrue(TEXT("Measurement includes the painted error/warning banner"),
                                    Measured.VisualBounds.Min.X <= Min.X + 0.1f && Measured.VisualBounds.Min.Y <= Min.Y + 0.1f &&
                                    Measured.VisualBounds.Max.X >= Max.X - 0.1f && Measured.VisualBounds.Max.Y >= Max.Y - 0.1f);
                            }
                        }
                        FChildren* Children = Widget->GetChildren();
                        if (Widgets.Num() + Children->Num() > 16384)
                        {
                            Test.AddError(TEXT("Native decoration widget hierarchy exceeds the inspection bound.")); break;
                        }
                        for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
                    }
                    Test.TestTrue(TEXT("Native compiler decoration has positive painted geometry"), bPaintedDecoration);
                }
                TArray<TSharedRef<SWidget>> Pins; Native->GetPins(Pins);
                TSet<FGuid> Painted;
                for (const auto& PinWidget : Pins)
                {
                    const UEdGraphPin* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj();
                    const auto* Expected = Measured.Pins.FindByPredicate([Pin](const FMeasuredPin& P) { return P.Id == Pin->PinId; });
                    const auto& Geometry = PinWidget->GetCachedGeometry();
                    const FVector2f Size(Geometry.GetLocalSize());
                    if (!PinWidget->GetVisibility().IsVisible() || Size.X <= 0 || Size.Y <= 0) { continue; }
                    if (!Test.TestTrue(Node->GetName() + TEXT(".") + Pin->PinName.ToString() + TEXT(" painted pin has measured geometry"),
                        Expected && Expected->AttachmentOffset.IsSet())) { continue; }
                    const FVector2f Point = Native->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(
                        FVector2f(Pin->Direction == EGPD_Output ? Size.X : 0, Size.Y * 0.5f)));
                    Test.TestTrue(Node->GetName() + TEXT(".") + Pin->PinName.ToString() + TEXT(" matches painted pin"), Expected->AttachmentOffset->Equals(Point, 0.1f));
                    Painted.Add(Pin->PinId);
                }
                for (const auto& Pin : Measured.Pins)
                {
                    if (Pin.AttachmentOffset) { Test.TestTrue(TEXT("Required measured pin has a painted counterpart"), Painted.Contains(Pin.Id)); }
                }
            }
            if (++NodeIndex < Graph->Nodes.Num()) { PanToNode(); Frames = 0; return false; }
            FGraphMeasurement Visible;
            CheckSelection(TEXT("Before visible measurement"));
            if (Test.TestTrue(TEXT("Cold measurement after viewing all nodes"), MeasureGraph(Graph, Scale, Visible, Reason, Options))) { Compare(Test, Measurement, Visible); }
            else { Test.AddError(Reason); }
            CheckSelection(TEXT("After visible measurement"));
            Phase = 2;
        }
        if (Phase == 2)
        {
            BeforeAll.Reset(); BeforeBytes.Reset(); AfterBytes.Reset();
            for (UEdGraph* Other : Fixture->AllGraphs) { BeforeAll.Add(Other, SerializeNodes(*Other)); }
            Before = SerializeNodes(*Graph, &BeforeBytes);
            BeforeValues = SerializeTransactionValues(*Graph);
            Properties = DescribeNodes(*Graph);
            Anchor = FIntPoint(Graph->Nodes[0]->NodePosX, Graph->Nodes[0]->NodePosY);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            CheckSelection(TEXT("Immediately before F"));
            Test.TestTrue(TEXT("Actual F handles supported graph"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 4; return false;
        }
        if (Phase == 4)
        {
            if (Before == SerializeNodes(*Graph)) { return false; }
            After = SerializeNodes(*Graph, &AfterBytes);
            AfterValues = SerializeTransactionValues(*Graph);
            Test.TestEqual(TEXT("Every original planned node remains after F"), Graph->Nodes.Num(), OffscreenPositions.Num());
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const auto* Planned = OffscreenPositions.Find(Node->NodeGuid);
                Test.TestTrue(TEXT("Visible F matches the initially offscreen formatting plan"),
                    Planned && *Planned == FIntPoint(Node->NodePosX, Node->NodePosY));
            }
            if (IsSpecializedTarget(Graph))
            {
                Test.TestEqual(TEXT("Specialized graph retains all seven original nodes"), Graph->Nodes.Num(), 7);
            }
            Test.TestTrue(Graph->GetName() + TEXT(" actually formats unselected nodes"), Before != After);
            Test.TestEqual(TEXT("Supported graph formats with one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestEqual(Graph->GetName() + TEXT(" selected anchor stays fixed"), FIntPoint(Graph->Nodes[0]->NodePosX, Graph->Nodes[0]->NodePosY), Anchor);
            CheckSelection(TEXT("Immediately after F"));
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Formatting preserves graph camera"), AfterView, View); Test.TestEqual(TEXT("Formatting preserves graph zoom"), AfterZoom, Zoom);
            AfterProperties = DescribeNodes(*Graph);
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const FString* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" preserved across formatting"), Value && *Value == Pair.Value);
            }
            for (const auto& Pair : BeforeAll)
            {
                if (Pair.Key != Graph) { Test.TestTrue(Pair.Key->GetName() + TEXT(" untouched while another graph formats"), Pair.Value == SerializeNodes(*Pair.Key)); }
            }
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            Test.TestTrue(TEXT("Second F is a cold no-op across graph types"), After == SerializeNodes(*Graph));
            Test.TestEqual(TEXT("Second F adds no transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Supported graph undo succeeds"), GEditor->UndoTransaction());
            if (!Test.TestTrue(Graph->GetName() + TEXT(" undo restores all serialized values"), BeforeValues == SerializeTransactionValues(*Graph)))
            {
                ReportNodeDifferences(Test, Properties, *Graph, &BeforeBytes);
            }
            Test.TestTrue(TEXT("Supported graph redo succeeds"), GEditor->RedoTransaction());
            if (!Test.TestTrue(Graph->GetName() + TEXT(" redo restores all serialized formatted values"), AfterValues == SerializeTransactionValues(*Graph)))
            {
                ReportNodeDifferences(Test, AfterProperties, *Graph, &AfterBytes);
            }
            Editor->ZoomToFit(false);
            CaptureAfter = FPlatformTime::Seconds() + 1.0;
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            if (FPlatformTime::Seconds() < CaptureAfter) { return false; }
            const auto Cache = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
            if ((!Cache || !Cache->IsReady()) && Frames < 120) { return false; }
            if (Test.TestTrue(TEXT("Expanded graph has live custom routes after undo/redo"), Cache && Cache->IsReady()))
            {
                FLayoutGraph RoutingGraph; FRouteSet Routing;
                if (Test.TestTrue(TEXT("Capture expanded routing"), CaptureGraph(Graph, Scale, {}, RoutingGraph, Reason, Options)) &&
                    Test.TestTrue(TEXT("Compute cold expanded routing"), ComputeRoutes(RoutingGraph, Routing, Reason)))
                {
                    if (IsSpecializedTarget(Graph))
                    {
                        Test.TestEqual(TEXT("Pin-mode graph retains every original connection"), RoutingGraph.Edges.Num(), Graph == Fixture->Async->GetGraph() ? 8 : 7);
                    }
                    Test.TestEqual(TEXT("Expanded execution, shared data and delegate wires need no native fallback"), Routing.FallbackCount, 0);
                    Test.TestEqual(TEXT("Live expanded routing keeps all connections"), Cache->GetRoutes().Wires.Num(), RoutingGraph.Edges.Num());
                    for (const auto& Pair : Routing.Wires)
                    {
                        const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                        Test.TestTrue(TEXT("Live expanded path matches cold routing"), Live && Live->Points == Pair.Value.Points && Live->Fallback == Pair.Value.Fallback);
                    }
                }
            }
            TArray<FColor> Pixels; FIntVector Size = FIntVector::ZeroValue;
            if ((Target == 0 || IsSpecializedTarget(Graph)) &&
                Test.TestTrue(TEXT("Capture expanded native fixture"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const FString Name = IsSpecializedTarget(Graph)
                    ? FString::Printf(TEXT("GlooPrint-%s-PinMode%d.png"), Graph == Fixture->Async->GetGraph() ? TEXT("AsyncCoverage") : TEXT("CustomWidgetCoverage"), PinMode)
                    : FString::Printf(TEXT("GlooPrint-ExpandedCoverage-PinMode%d.png"), PinMode);
                Test.TestTrue(TEXT("Save expanded fixture screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
            }
        }
        if (Phase >= 2)
        {
            Window->RequestDestroyWindow(); Editor.Reset(); Window.Reset();
            if (PinMode < 2)
            {
                ++PinMode;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    const auto* Position = Fixture->InitialPositions.Find(Node->NodeGuid);
                    if (!Test.TestNotNull(TEXT("Node identity survives the preceding visibility case"), Position)) { return Finish(); }
                    Node->SetPosition(FVector2f(float(Position->X), float(Position->Y)));
                }
                Graph->NotifyGraphChanged(); OpenTarget(); return false;
            }
            if (++Target == Fixture->Targets.Num()) { return true; }
            PinMode = 0; OpenTarget();
        }
        return false;
    }
private:
    bool IsSpecializedTarget(const UEdGraph* Graph) const
    {
        return Graph == Fixture->Async->GetGraph() || Graph == Fixture->Spawn->GetGraph();
    }
    void CheckSelection(const TCHAR* Context)
    {
        UEdGraph* Graph = Fixture->Targets[Target];
        const auto& Selected = Editor->GetSelectedNodes();
        Test.TestTrue(Graph->GetName() + TEXT(": ") + Context + TEXT(" retains the exact selected anchor"),
            Selected.Num() == 1 && Selected.Contains(Graph->Nodes[0]));
    }
    void OpenTarget()
    {
        UEdGraph* Graph = Fixture->Targets[Target];
        const SGraphEditor::EPinVisibility Modes[] = {SGraphEditor::Pin_Show, SGraphEditor::Pin_HideNoConnection, SGraphEditor::Pin_HideNoConnectionNoDefault};
        Options.PinVisibility = Modes[PinMode];
        Editor = SNew(SGraphEditor).GraphToEdit(Graph).IsEditable(true);
        Editor->SetPinVisibility(Options.PinVisibility);
        Window = SNew(SWindow).Title(FText::FromString(Graph->GetName())).ClientSize(FVector2f(1100, 800))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        Editor->SetNodeSelection(Graph->Nodes[0], true);
        CheckSelection(TEXT("Opening the native graph"));
        Editor->SetViewLocation(FVector2f(100000, 100000), 0.25f);
        Phase = 0; Frames = 0; Deadline = FPlatformTime::Seconds() + 60;
    }
    void PanToNode()
    {
        const UEdGraphNode* Node = Fixture->Targets[Target]->Nodes[NodeIndex];
        Editor->SetViewLocation(FVector2f(Node->NodePosX - 80, Node->NodePosY - 100), 1.0f);
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FGraphCoverageFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FGraphMeasurement Measurement;
    FMeasurementOptions Options;
    TMap<FGuid, FIntPoint> OffscreenPositions;
    TMap<UEdGraph*, TArray<uint8>> BeforeAll;
    TMap<FGuid, TArray<uint8>> BeforeBytes, AfterBytes;
    TMap<FString, FString> Properties, AfterProperties;
    TArray<uint8> Before, BeforeValues, After, AfterValues;
    FIntPoint Anchor;
    FVector2f View;
    float Zoom = 0;
    int32 Target = 0, NodeIndex = 0, Phase = 0, Frames = 0, PinMode = 0, Queue = 0;
    double CaptureAfter = 0, Deadline = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphCoverageTest, "GlooPrint.Editor.SupportedGraphsAndNativeNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphCoverageTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FGraphCoverageCheck(*this));
    return true;
}
}
#endif
