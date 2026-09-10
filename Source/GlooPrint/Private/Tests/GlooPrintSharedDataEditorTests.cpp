// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintEditorSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "UObject/Script.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FSharedDataCheck final : public IAutomationLatentCommand
{
public:
    explicit FSharedDataCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FSharedDataCheck() { Restore(); }
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
            for (int32 Chain = 0; Chain < 2; ++Chain)
            {
                auto* Entry = Fixture->Add<UK2Node_CustomEvent>({0, float(Chain * 450)});
                Entry->CustomFunctionName = Chain ? TEXT("SharedInputSecondEvent") : TEXT("SharedInputFirstEvent");
                Flow[Chain].Add(Entry);
                for (int32 Step = 0; Step < 4; ++Step)
                {
                    auto* Branch = Fixture->Add<UK2Node_IfThenElse>({float(900 - Step * 200), float(Chain * 450 + (Step % 2) * 150)});
                    Link(Flow[Chain].Last()->FindPinChecked(UEdGraphSchema_K2::PN_Then), Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
                    Flow[Chain].Add(Branch);
                }
            }
            Input = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralBool"), {200, 900});
            Input->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("true");
            Shared = Call(UKismetMathLibrary::StaticClass(), TEXT("Not_PreBool"), {800, 900});
            Link(Input->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Shared->FindPinChecked(TEXT("A")));
            for (const auto& Chain : Flow)
            {
                Link(Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Chain.Last()->FindPinChecked(UEdGraphSchema_K2::PN_Condition));
            }
            if (!bConnectionsValid) { return Finish(); }
            Fixture->Blueprint->CompileMode = EBlueprintCompileMode::Development;
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint shared expressions and execution chains")))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -250), 0.75f);
            Editor->SetNodeSelection(Flow[0][0], true);
            Slate.SetCursorPos(FVector2D::ZeroVector);
            if (!CompileWithFormatRefusalCheck() || !SetUpDebugState()) { return Finish(); }
            Deadline = FPlatformTime::Seconds() + 40;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Shared expression fixture did not settle in 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph);
            Properties = DescribeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Native shared expression gets a complete format plan"),
                PlanFormatGraph(Fixture->Graph, Scale, {Flow[0][0]->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Shared input planning leaves native node/pin values unchanged"), SerializeNodes(*Fixture->Graph) == Before);
            CheckDebugState(TEXT("Planning"), OriginalStatus);
            const auto Index = [this](UEdGraphNode* Node) { return Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; }); };
            const int32 SharedIndex = Index(Shared), InputIndex = Index(Input);
            const auto& SharedGeometry = Plan.Snapshot.Nodes[SharedIndex].Geometry;
            for (const auto& Chain : Flow)
            {
                const int32 Consumer = Index(Chain.Last());
                const float Gap = Plan.Layout.Positions[Consumer].X - (Plan.Layout.Positions[SharedIndex].X + SharedGeometry.VisualBounds.Max.X);
                Test.TestTrue(TEXT("Native shared expression is close to both consumers with wire clearance"), Gap >= 95.5f && Gap <= 112.5f);
                for (int32 Step = 1; Step < Chain.Num(); ++Step)
                {
                    const int32 FromNode = Index(Chain[Step - 1]), ToNode = Index(Chain[Step]);
                    const FLayoutPin* Output = Plan.Snapshot.Pins.FindByPredicate([FromNode](const auto& P) { return P.Node == FromNode && P.bOutput && P.Kind == ELinkKind::Execution; });
                    const FLayoutPin* InputPin = Plan.Snapshot.Pins.FindByPredicate([ToNode](const auto& P) { return P.Node == ToNode && !P.bOutput && P.Kind == ELinkKind::Execution; });
                    if (Test.TestTrue(TEXT("Native chain retains measured execution attachments"), Output && InputPin && Output->Offset.IsSet() && InputPin->Offset.IsSet()))
                    {
                        const float Delta = Plan.Layout.Positions[FromNode].Y + Output->Offset.GetValue().Y - Plan.Layout.Positions[ToNode].Y - InputPin->Offset.GetValue().Y;
                        Test.TestTrue(TEXT("Shared expression placement preserves straight execution chains"), FMath::Abs(Delta) <= 0.5f);
                    }
                }
            }
            Test.TestTrue(TEXT("Native expression retains its own input's dependency order"),
                Plan.Layout.Positions[InputIndex].X + Plan.Snapshot.Nodes[InputIndex].Geometry.VisualBounds.Max.X + 95.5f <=
                    Plan.Layout.Positions[SharedIndex].X + SharedGeometry.VisualBounds.Min.X);
            Test.TestEqual(TEXT("Native fixture preserves all eleven connections with custom routes"), Plan.Routes.Wires.Num(), 11);
            Test.TestEqual(TEXT("Shared expression and flow lanes have no native fallback"), Plan.Routes.FallbackCount, 0);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats native shared expressions"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 2; return false;
        }
        if (Phase == 2)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Actual F changes the scattered graph"), After != Before);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            Test.TestEqual(TEXT("Formatting retains all node/pin property entries"), AfterProperties.Num(), Properties.Num());
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const FString* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" preserved across formatting"), Value && *Value == Pair.Value);
            }
            CheckDebugState(TEXT("F"), OriginalStatus);
            Test.TestEqual(TEXT("Native shared layout is one undo entry"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Shared expression layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Shared expression undo restores every serialized value"), SerializeNodes(*Fixture->Graph) == Before);
            CheckDebugState(TEXT("Undo"), BS_Dirty);
            Test.TestTrue(TEXT("Shared expression layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Shared expression redo restores every serialized value"), SerializeNodes(*Fixture->Graph) == After);
            CheckDebugState(TEXT("Redo"), BS_Dirty);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            Test.TestTrue(TEXT("Repeated F leaves the shared graph exactly unchanged"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Shared graph no-op creates no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
            CheckDebugState(TEXT("Cold plan and repeated F"), BS_Dirty);
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Shared layout preserves view"), AfterView, View); Test.TestEqual(TEXT("Shared layout preserves zoom"), AfterZoom, Zoom);
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture native shared expressions"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save native shared expression capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-SharedExpressions.png"))));
            }
            return Finish();
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        Test.TestEqual(TEXT("Formatting never duplicates the shared node or its input"), Fixture->Graph->Nodes.Num(), 12);
        Test.TestEqual(TEXT("The same native output still feeds both consumers"), Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue)->LinkedTo.Num(), 2);
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Painted shared-data route matches the preflight path"), Live && Live->Points == Pair.Value.Points);
        }
        FFormatPlan Cold;
        if (Test.TestTrue(TEXT("Native shared layout replans with a cold measurement"),
            PlanFormatGraph(Fixture->Graph, Scale, {Flow[0][0]->NodeGuid}, Cold, Reason)))
        {
            Test.TestTrue(TEXT("Native shared-data layout is cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
        }
        Queue = GEditor->Trans->GetQueueLength();
        Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        Phase = 3; Frames = 0; return false;
    }
private:
    bool CompileWithFormatRefusalCheck()
    {
        bool bSawCompiler = false;
        const auto Handle = GEditor->OnBlueprintPreCompile().AddLambda([this, &bSawCompiler](UBlueprint* Blueprint)
        {
            if (Blueprint != Fixture->Blueprint.Get()) { return; }
            bSawCompiler = true;
            Test.TestTrue(TEXT("Native compiler owns the Blueprint busy flag"), Blueprint->bBeingCompiled);
            const auto State = SerializeNodes(*Fixture->Graph);
            const int32 CompileQueue = GEditor->Trans->GetQueueLength();
            const bool bDirty = Blueprint->GetOutermost()->IsDirty();
            const auto Status = Blueprint->Status;
            auto& Slate = FSlateApplication::Get();
            FVector2f BeforeView; float BeforeZoom; Editor->GetViewLocation(BeforeView, BeforeZoom);
            FString Reason; int32 Changed = INDEX_NONE; bool bRetry = true;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            Test.TestFalse(TEXT("Real compilation rejects formatting"), FormatGraph(Fixture->Graph, Scale, {}, Changed, Reason, {}, &bRetry));
            Test.TestTrue(TEXT("Compiler refusal explains the busy context"), Reason.Contains(TEXT("compiled/reconstructed")));
            Test.TestTrue(TEXT("Compiler refusal changes no nodes and schedules no retry"), Changed == 0 && !bRetry);
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F handles refusal during native compilation"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Test.TestTrue(TEXT("F during compilation preserves every native node and pin value"), State == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("F during compilation adds no transaction"), GEditor->Trans->GetQueueLength(), CompileQueue);
            Test.TestEqual(TEXT("F during compilation preserves package dirty state"), Blueprint->GetOutermost()->IsDirty(), bDirty);
            Test.TestEqual(TEXT("F during compilation preserves compiler status"), Blueprint->Status, Status);
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestTrue(TEXT("F during compilation preserves view and selected anchor"), BeforeView == AfterView && BeforeZoom == AfterZoom &&
                Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Flow[0][0]));
        });
        FCompilerResultsLog Compile;
        FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Compile);
        GEditor->OnBlueprintPreCompile().Remove(Handle);
        Test.TestTrue(TEXT("Real compilation reached the native pre-compile callback"), bSawCompiler);
        Test.TestFalse(TEXT("Native compilation clears its busy flag"), Fixture->Blueprint->bBeingCompiled);
        return Test.TestEqual(TEXT("Shared-input Actor Blueprint compiles before formatting"), Compile.NumErrors, 0);
    }
    bool SetUpDebugState()
    {
        const auto* Blueprint = Fixture->Blueprint.Get();
        if (!Test.TestFalse(TEXT("The disposable Blueprint has no existing debug preferences"),
            GetDefault<UBlueprintEditorSettings>()->PerBlueprintSettings.Contains(Blueprint->GetPathName()))) { return false; }
        OriginalGeneratedClass = Blueprint->GeneratedClass;
        OriginalStatus = Blueprint->Status;
        if (!Test.TestNotNull(TEXT("Debug preservation uses a compiled Blueprint class"), OriginalGeneratedClass.Get())) { return false; }
        for (UEdGraphNode* Node : Fixture->Graph->Nodes) { OriginalNodes.Add(Node); }
        bOwnsDebugState = true;
        FKismetDebugUtilities::CreateBreakpoint(Blueprint, Flow[0][1], true);
        FKismetDebugUtilities::CreateBreakpoint(Blueprint, Flow[1][1], false);
        for (const auto* Node : {Input, Shared})
        {
            const auto* Pin = Node->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue);
            if (!Test.TestTrue(TEXT("Native K2 permits watching the shared-data output"), FKismetDebugUtilities::CanWatchPin(Blueprint, Pin))) { return false; }
            ExpectedWatches.Add(FBlueprintWatchedPin(Pin));
            FKismetDebugUtilities::TogglePinWatch(Blueprint, Pin);
        }
        CheckDebugState(TEXT("Compiled baseline"), OriginalStatus);
        return true;
    }
    void CheckDebugState(const FString& Context, EBlueprintStatus ExpectedStatus)
    {
        const auto* Blueprint = Fixture->Blueprint.Get();
        Test.TestTrue(Context + TEXT(" preserves the compiled class"), Blueprint->GeneratedClass == OriginalGeneratedClass.Get());
        Test.TestEqual(Context + TEXT(" retains the expected native Blueprint status"), int32(Blueprint->Status.GetValue()), int32(ExpectedStatus));
        Test.TestEqual(Context + TEXT(" preserves graph node count"), Fixture->Graph->Nodes.Num(), OriginalNodes.Num());
        for (int32 Index = 0; Index < FMath::Min(Fixture->Graph->Nodes.Num(), OriginalNodes.Num()); ++Index)
        {
            Test.TestTrue(Context + TEXT(" preserves each node object and its order"), Fixture->Graph->Nodes[Index] == OriginalNodes[Index].Get());
        }
        const auto* Debug = GetDefault<UBlueprintEditorSettings>()->PerBlueprintSettings.Find(Blueprint->GetPathName());
        if (!Test.TestNotNull(Context + TEXT(" retains native debug preferences"), Debug)) { return; }
        Test.TestEqual(Context + TEXT(" retains exactly two breakpoints"), Debug->Breakpoints.Num(), 2);
        for (int32 Chain = 0; Chain < 2; ++Chain)
        {
            const auto* Breakpoint = FKismetDebugUtilities::FindBreakpointForNode(Flow[Chain][1], Blueprint);
            if (!Test.TestNotNull(Context + TEXT(" retains the original breakpoint node"), Breakpoint)) { continue; }
            Test.TestTrue(Context + TEXT(" preserves breakpoint location"), Breakpoint->GetLocation() == Flow[Chain][1]);
            Test.TestEqual(Context + TEXT(" preserves breakpoint enabled state"), Breakpoint->IsEnabled(), Chain == 0);
            Test.TestEqual(Context + TEXT(" preserves user breakpoint state"), Breakpoint->IsEnabledByUser(), Chain == 0);
            Test.TestTrue(Context + TEXT(" retains a valid compiled breakpoint"), FKismetDebugUtilities::IsBreakpointValid(*Breakpoint));
            TArray<uint8*> Sites;
            FKismetDebugUtilities::GetBreakpointInstallationSites(*Breakpoint, Sites);
            Test.TestTrue(Context + TEXT(" retains compiled breakpoint sites"), !Sites.IsEmpty());
            for (const uint8* Site : Sites)
            {
                if (Test.TestNotNull(Context + TEXT(" has an installed debug instruction"), Site))
                {
                    Test.TestEqual(Context + TEXT(" preserves the native debug opcode"), *Site, uint8(Chain == 0 ? EX_Breakpoint : EX_Tracepoint));
                }
            }
        }
        Test.TestTrue(Context + TEXT(" preserves watch pin IDs, owners, order and property paths"), Debug->WatchedPins == ExpectedWatches);
        for (int32 Index = 0; Index < Debug->WatchedPins.Num(); ++Index)
        {
            if (Index >= 2) { break; }
            const auto* Pin = (Index == 0 ? Input : Shared)->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue);
            Test.TestTrue(Context + TEXT(" resolves the watch to its original live output"), Debug->WatchedPins[Index].Get() == Pin);
            Test.TestTrue(Context + TEXT(" keeps the output watched by Unreal"), FKismetDebugUtilities::IsPinBeingWatched(Blueprint, Pin));
        }
    }
    UK2Node_CallFunction* Call(UClass* Library, FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(Library->FindFunctionByName(Name)); Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native K2 schema accepts fixture connection"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B));
    }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (bOwnsDebugState)
        {
            FKismetDebugUtilities::ClearPinWatches(Fixture->Blueprint.Get());
            FKismetDebugUtilities::ClearBreakpoints(Fixture->Blueprint.Get());
            Test.TestFalse(TEXT("The fixture leaves no debug preferences entry"),
                GetDefault<UBlueprintEditorSettings>()->PerBlueprintSettings.Contains(Fixture->Blueprint->GetPathName()));
            bOwnsDebugState = false;
        }
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
    TArray<UEdGraphNode*> Flow[2];
    UK2Node_CallFunction* Input = nullptr;
    UK2Node_CallFunction* Shared = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    TMap<FString, FString> Properties;
    TArray<TWeakObjectPtr<UEdGraphNode>> OriginalNodes;
    TWeakObjectPtr<UClass> OriginalGeneratedClass;
    TEnumAsByte<EBlueprintStatus> OriginalStatus = BS_Unknown;
    TArray<FBlueprintWatchedPin> ExpectedWatches;
    FVector2D OriginalCursor;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Queue = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true, bOwnsDebugState = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSharedDataEditorTest, "GlooPrint.Editor.SharedExpressionsAndFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSharedDataEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSharedDataCheck(*this)); return true;
}
}
#endif
