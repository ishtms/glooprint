// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "BlueprintEditorSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FExecutionWireCheck final : public IAutomationLatentCommand
{
public:
    explicit FExecutionWireCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FExecutionWireCheck() { Restore(); }

    virtual bool Update() override
    {
        if (Phase == EPhase::Start) { return Start(); }
        if (Phase == EPhase::Stop)
        {
            if (GEditor->PlayWorld || GEditor->GetPlaySessionRequest().IsSet())
            {
                if (FPlatformTime::Seconds() < Deadline) { return false; }
                Test.AddError(TEXT("Owned PIE session did not stop within 30 seconds."));
            }
            else
            {
                bOwnsPIE = false;
                Test.TestTrue(TEXT("PIE leaves the original editor world active"), GEditor->GetEditorWorldContext().World() == OriginalWorld.Get());
                Test.TestTrue(TEXT("Read-only routing and execution leave graph data unchanged"), Before == SerializeNodes(*Fixture->Graph));
            }
            Restore(); return true;
        }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(TEXT("Execution wire fixture did not finish its current phase within 45 seconds."));
            return Stop();
        }
        if (Phase == EPhase::AwaitPIE)
        {
            if (!GEditor->PlayWorld) { return false; }
            Test.TestEqual(TEXT("The test is running in an actual PIE world"), GEditor->PlayWorld->WorldType, EWorldType::PIE);
            Actor = GEditor->PlayWorld->SpawnActor<AActor>(Fixture->Blueprint->GeneratedClass);
            if (!Test.TestNotNull(TEXT("Compiled fixture actor spawns in PIE"), Actor.Get())) { return Stop(); }
            Fixture->Blueprint->SetObjectBeingDebugged(Actor.Get());
            OpenGraph();
            Phase = EPhase::Playing; Frames = 0; Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (Actor.IsValid())
        {
            UFunction* Event = Actor->FindFunction(TEXT("TraceRoute"));
            if (!Test.TestNotNull(TEXT("Compiled custom event exists"), Event)) { return Stop(); }
            Actor->ProcessEvent(Event, nullptr);
        }
        if (++Frames < 12) { return false; }
        auto Cache = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
        if (!Cache || Cache->GetBuildCount() == 0) { return false; }
        if (Phase == EPhase::ReadOnly)
        {
            CheckRoutes(*Cache, TEXT("Read-only graph builds custom wires without F"));
            CheckFormattingBlocked(TEXT("read-only"));
            CloseGraph();
            Fixture->Graph->bEditable = true;
            PlaySettings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
            PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
            PlaySettings->SetRunUnderOneProcess(true);
            PlaySettings->SetPlayNumberOfClients(1);
            PlaySettings->NewWindowWidth = 320; PlaySettings->NewWindowHeight = 240;
            PlaySettings->EnableGameSound = false;
            FRequestPlaySessionParams Params;
            Params.EditorPlaySettings = PlaySettings.Get();
            Params.GlobalMapOverride = TEXT("/Engine/Maps/Entry");
            Params.GameModeOverride = AGameModeBase::StaticClass();
            Params.StartLocation = FVector(0, 0, 300);
            Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params);
            bOwnsPIE = true; Phase = EPhase::AwaitPIE; Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (Phase == EPhase::Playing)
        {
            CheckRoutes(*Cache, TEXT("Graph opened during PIE builds custom wires without F"));
            CheckFormattingBlocked(TEXT("play/simulation"));
            FIntProperty* Value = FindFProperty<FIntProperty>(Actor->GetClass(), TEXT("TraceCount"));
            if (Test.TestNotNull(TEXT("Compiled Blueprint retains its integer property"), Value))
            {
                Test.TestEqual(TEXT("The VM executed the true branch"), Value->GetPropertyValue_InContainer(Actor.Get()), 1);
            }
            const auto& Trace = FKismetDebugUtilities::GetTraceStack();
            auto* Generated = CastChecked<UBlueprintGeneratedClass>(Fixture->Blueprint->GeneratedClass);
            bool bTrueTrace = false, bFalseTrace = false, bBranchPinTrace = false;
            for (int32 I = 0; I < Trace.Num(); ++I)
            {
                const auto& Sample = Trace(I);
                if (Sample.Context.Get() != Actor.Get()) { continue; }
                UEdGraphNode* Node = Generated->GetDebugData().FindSourceNodeFromCodeLocation(Sample.Function.Get(), Sample.Offset, false);
                bTrueTrace |= Node == TrueSet; bFalseTrace |= Node == FalseSet;
                bBranchPinTrace |= Generated->GetDebugData().FindSourcePinFromCodeLocation(Sample.Function.Get(), Sample.Offset) ==
                    Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            }
            Test.TestTrue(TEXT("Native VM trace identifies the executed setter"), bTrueTrace);
            Test.TestFalse(TEXT("Native VM trace excludes the unexecuted setter"), bFalseTrace);
            Test.TestTrue(TEXT("Native wire trace identifies the Branch true output"), bBranchPinTrace);
            CheckDrawing(*Cache);
            Capture(TEXT("GlooPrint-ExecutionRounded.png"));
            Builds = Cache->GetBuildCount();
            Fixture->Graph->NotifyGraphChanged();
            Test.TestFalse(TEXT("A graph notification invalidates routes during PIE"), Cache->IsReady());
            Phase = EPhase::Rebuilt; Frames = 0;
            return false;
        }
        if (Phase == EPhase::Rebuilt)
        {
            CheckRoutes(*Cache, TEXT("Invalidated custom wires rebuild while PIE continues"));
            Test.TestTrue(TEXT("PIE route invalidation performs another rebuild"), Cache->GetBuildCount() > Builds);
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Diagonal45;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Phase = EPhase::Diagonal; Frames = 0;
            return false;
        }
        CheckRoutes(*Cache, TEXT("Changing wire style during PIE rebuilds custom routes"));
        CheckDrawing(*Cache);
        Capture(TEXT("GlooPrint-ExecutionDiagonal.png"));
        CheckPausedRoutes();
        return Stop();
    }

private:
    bool Start()
    {
        OriginalWorld = GEditor->GetEditorWorldContext().World();
        OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
        bOriginalArrows = GetDefault<UBlueprintEditorSettings>()->bDrawMidpointArrowsInBlueprints;
        bRestore = true;
        GetMutableDefault<UBlueprintEditorSettings>()->bDrawMidpointArrowsInBlueprints = true;
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
        GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
        Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
        Fixture->Blueprint->CompileMode = EBlueprintCompileMode::Development;
        FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
        if (!Test.TestTrue(TEXT("Create the fixture's integer member"), FBlueprintEditorUtils::AddMemberVariable(Fixture->Blueprint.Get(), TEXT("TraceCount"), Type, TEXT("0")))) { return Stop(); }
        Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("TraceRoute");
        Branch = Fixture->Add<UK2Node_IfThenElse>({280, 0});
        Branch->FindPinChecked(UEdGraphSchema_K2::PN_Condition)->DefaultValue = TEXT("true");
        TrueSet = AddSetter({620, -160}, TEXT("1"));
        FalseSet = AddSetter({620, 250}, TEXT("2"));
        const auto* Schema = Fixture->Graph->GetSchema();
        bool bLinks = Schema->TryCreateConnection(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        bLinks &= Schema->TryCreateConnection(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then), TrueSet->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        bLinks &= Schema->TryCreateConnection(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else), FalseSet->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        if (!Test.TestTrue(TEXT("Native schema creates all fixture connections"), bLinks)) { return Stop(); }
        FCompilerResultsLog Compile;
        FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Compile);
        if (!Test.TestEqual(TEXT("Executable wire fixture compiles"), Compile.NumErrors, 0)) { return Stop(); }
        FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(TrueSet);
        if (!Test.TestNotNull(TEXT("Native Blueprint debugger editor is open"),
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Fixture->Blueprint.Get(), false))) { return Stop(); }
        Before = SerializeNodes(*Fixture->Graph);
        Fixture->Graph->bEditable = false;
        OpenGraph(); Phase = EPhase::ReadOnly; Deadline = FPlatformTime::Seconds() + 45;
        return false;
    }
    UK2Node_VariableSet* AddSetter(FVector2f Position, const TCHAR* Value)
    {
        auto* Node = NewObject<UK2Node_VariableSet>(Fixture->Graph);
        Node->VariableReference.SetSelfMember(TEXT("TraceCount"));
        Fixture->Initialize(*Node, Position);
        Node->FindPinChecked(TEXT("TraceCount"))->DefaultValue = Value;
        return Node;
    }
    void OpenGraph()
    {
        TWeakObjectPtr<UEdGraph> Graph = Fixture->Graph;
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable_Lambda([Graph]
        {
            return Graph.IsValid() && Graph->bEditable && GEditor && !GEditor->PlayWorld;
        });
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint execution wire fixture")))
            .ClientSize(FVector2f(1150, 800))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        Editor->SetViewLocation(FVector2f(-120, -260), 1.f);
    }
    void CheckRoutes(const FRouteCache& Cache, const TCHAR* Message)
    {
        Test.TestTrue(Message, Cache.IsReady());
        if (!Cache.IsReady()) { return; }
        Test.TestEqual(TEXT("All three original execution connections have routes"), Cache.GetRoutes().Wires.Num(), 3);
        Test.TestEqual(TEXT("Execution fixture needs no native fallback"), Cache.GetRoutes().FallbackCount, 0);
        for (UEdGraphNode* Target : {static_cast<UEdGraphNode*>(TrueSet), static_cast<UEdGraphNode*>(FalseSet)})
        {
            UEdGraphPin* Input = Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            UEdGraphPin* Output = Input->LinkedTo[0];
            FRouteKey Key{Output->GetOwningNode()->NodeGuid, Output->PinId, Target->NodeGuid, Input->PinId};
            const auto* Route = Cache.GetRoutes().Wires.Find(Key);
            Test.TestTrue(TEXT("Branch connection is a bent custom route for its original pins"), Route && Route->Curves.Num() > 1);
        }
    }
    void CheckFormattingBlocked(const TCHAR* ExpectedReason)
    {
        const auto State = SerializeNodes(*Fixture->Graph);
        const int32 Queue = GEditor->Trans->GetQueueLength();
        FString Reason; int32 Changed = 0;
        const float Scale = Window->GetDPIScaleFactor() * FSlateApplication::Get().GetApplicationScale();
        Test.TestFalse(TEXT("Formatting remains forbidden in this context"), FormatGraph(Fixture->Graph, Scale, {}, Changed, Reason));
        Test.TestTrue(TEXT("Formatting explains the context restriction"), Reason.Contains(ExpectedReason));
        Test.TestEqual(TEXT("Rejected format changes no nodes"), Changed, 0);
        Test.TestEqual(TEXT("Rejected format opens no transaction"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestTrue(TEXT("Rejected format preserves every serialized node/pin value"), State == SerializeNodes(*Fixture->Graph));
    }
    void CheckDrawing(const FRouteCache& Cache)
    {
        if (!Cache.IsReady()) { return; }
        auto* Panel = Editor->GetGraphPanel();
        FArrangedChildren Nodes(EVisibility::Visible);
        TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
        for (const auto& Node : Fixture->Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Executed node has a live native widget"), Widget.IsValid())) { return; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
            TArray<TSharedRef<SWidget>> NativePins; Widget->GetPins(NativePins);
            for (const auto& Pin : NativePins) { Pins.Add(Pin, FArrangedWidget(Pin, Pin->GetCachedGeometry())); }
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        FSlateWindowElementList Elements(Window);
        const auto Factory = MakeShared<FWireDrawing>();
        TUniquePtr<FConnectionDrawingPolicy> Policy(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, Scale,
            FSlateRect(-100000, -100000, 100000, 100000), Elements, Fixture->Graph));
        if (!Test.TestTrue(TEXT("Custom drawing policy remains available during PIE"), Policy.IsValid())) { return; }
        Policy->SetAbsoluteMousePosition(FVector2f(-100000, -100000));
        Policy->Draw(Pins, Nodes);
        FConnectionParams Active, Inactive, EntryStyle;
        Policy->DetermineWiringStyle(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute), EntryStyle);
        Policy->DetermineWiringStyle(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then), TrueSet->FindPinChecked(UEdGraphSchema_K2::PN_Execute), Active);
        Policy->DetermineWiringStyle(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else), FalseSet->FindPinChecked(UEdGraphSchema_K2::PN_Execute), Inactive);
        Test.TestTrue(TEXT("Native trace enables pulses only on the executed branch"), Active.bDrawBubbles && !Inactive.bDrawBubbles);
        Test.TestTrue(TEXT("Executed wire receives native color and thickness emphasis"), Active.WireColor != Inactive.WireColor && Active.WireThickness > Inactive.WireThickness);
        const auto& Splines = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
        int32 ExpectedPieces = 0;
        for (const auto& Pair : Cache.GetRoutes().Wires) { ExpectedPieces += Pair.Value.Curves.Num(); }
        Test.TestEqual(TEXT("Every custom route piece is actually drawn during PIE"), Splines.Num(), ExpectedPieces);
        int32 ActivePieces = 0, InactivePieces = 0;
        for (const auto& Spline : Splines)
        {
            if (Spline.GetTint().Equals(Active.WireColor, 0.001f))
            {
                ++ActivePieces;
                Test.TestTrue(TEXT("Active custom pieces retain native trace thickness"), FMath::IsNearlyEqual(Spline.GetThickness(), Active.WireThickness, 0.001f));
            }
            else if (Spline.GetTint().Equals(Inactive.WireColor, 0.001f)) { ++InactivePieces; }
        }
        Test.TestTrue(TEXT("The visible active branch uses multiple custom pieces"), ActivePieces > 2);
        Test.TestTrue(TEXT("The visible inactive branch retains its native color"), InactivePieces > 1);
        const auto* Bubble = FAppStyle::GetBrush(TEXT("Graph.ExecutionBubble"))->GetRenderingResource().GetResourceProxy();
        const auto* Arrow = FAppStyle::GetBrush(TEXT("Graph.Arrow"))->GetRenderingResource().GetResourceProxy();
        int32 Bubbles = 0, Arrows = 0;
        for (const auto& Box : Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Box>())
        {
            const bool bBubble = Box.GetResourceProxy() == Bubble, bArrow = Box.GetResourceProxy() == Arrow;
            if (!bBubble && !bArrow) { continue; }
            Bubbles += bBubble; Arrows += bArrow;
            const FVector2f Center = TransformPoint(Box.GetRenderTransform(), FVector2f(Box.GetLocalSize()) * 0.5f);
            float BestDistance = MAX_flt; FVector2f Tangent = FVector2f::ZeroVector;
            for (const auto& Spline : Splines)
            {
                if (!Spline.GetTint().Equals(Box.GetTint(), 0.001f)) { continue; }
                FVector2f A = Spline.P0;
                for (int32 Step = 1; Step <= 32; ++Step)
                {
                    const float T = Step / 32.f, S = 1 - T;
                    const FVector2f B = S*S*S*Spline.P0 + 3*S*S*T*Spline.P1 + 3*S*T*T*Spline.P2 + T*T*T*Spline.P3;
                    const FVector2f Segment = B - A;
                    const float Along = FMath::Clamp(FVector2f::DotProduct(Center - A, Segment) / FMath::Max(Segment.SizeSquared(), SMALL_NUMBER), 0.f, 1.f);
                    const float Distance = (Center - A - Along * Segment).Size();
                    if (Distance < BestDistance) { BestDistance = Distance; Tangent = Segment.GetSafeNormal(); }
                    A = B;
                }
            }
            Test.TestTrue(TEXT("Native pulse or arrow is attached to its visible custom wire"), BestDistance < 1.f);
            if (bBubble)
            {
                Test.TestTrue(TEXT("Pulses use an executed connection's native color"),
                    Box.GetTint().Equals(Active.WireColor, 0.001f) || Box.GetTint().Equals(EntryStyle.WireColor, 0.001f));
            }
            if (bArrow)
            {
                const FVector2f Direction = TransformVector(Box.GetRenderTransform(), FVector2f(1, 0)).GetSafeNormal();
                Test.TestTrue(TEXT("Midpoint arrow points along the visible wire toward its input"), FVector2f::DotProduct(Direction, Tangent) > 0.99f);
            }
        }
        Test.TestTrue(TEXT("Executed custom wires draw native pulse brushes"), Bubbles > 1);
        Test.TestEqual(TEXT("Each original connection has one native midpoint arrow"), Arrows, 3);
    }
    void CheckPausedRoutes()
    {
        auto& Slate = FSlateApplication::Get();
        FKismetDebugUtilities::CreateBreakpoint(Fixture->Blueprint.Get(), TrueSet);
        bOwnsBreakpoint = true;
        Test.AddExpectedMessagePlain(TEXT("Hit breakpoint on node"), ELogVerbosity::Warning);
        bool bHit = false, bComplete = false, bInCallback = false;
        int32 DebugPhase = 0, DebugFrames = 0, PreviousBuilds = 0;
        const double DebugDeadline = FPlatformTime::Seconds() + 15;
        const FDelegateHandle Handle = Slate.OnPostTick().AddLambda([&](float)
        {
            if (Slate.IsNormalExecution() || bInCallback) { return; }
            TGuardValue<bool> Guard(bInCallback, true);
            const bool bStoppedHere = FKismetDebugUtilities::GetMostRecentBreakpointHit() == TrueSet &&
                FKismetDebugUtilities::GetCurrentInstruction() == TrueSet &&
                FKismetDebugUtilities::GetCurrentDebuggingWorld() == GEditor->PlayWorld;
            bHit |= bStoppedHere;
            if (FPlatformTime::Seconds() > DebugDeadline)
            {
                Test.AddError(TEXT("Breakpoint route checks exceeded their 15-second bound."));
                GEditor->SetPIEWorldsPaused(false); Slate.LeaveDebuggingMode(); return;
            }
            if (!bStoppedHere || ++DebugFrames < 12) { return; }
            auto PausedCache = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
            if (DebugPhase == 0)
            {
                Test.TestTrue(TEXT("The native VM is paused at the fixture breakpoint"), GEditor->PlayWorld->bDebugPauseExecution);
                if (!Test.TestTrue(TEXT("Paused graph retains its warm custom cache"), PausedCache && PausedCache->IsReady()))
                {
                    GEditor->SetPIEWorldsPaused(false); Slate.LeaveDebuggingMode(); return;
                }
                PreviousBuilds = PausedCache->GetBuildCount();
                Fixture->Graph->NotifyGraphChanged();
                Test.TestFalse(TEXT("A breakpoint-time notification invalidates custom routes"), PausedCache->IsReady());
                DebugPhase = 1; DebugFrames = 0; return;
            }
            if (DebugPhase == 1)
            {
                if (Test.TestTrue(TEXT("Paused graph keeps its panel cache"), PausedCache.IsValid()))
                {
                    CheckRoutes(*PausedCache, TEXT("Invalidated wires rebuild before the breakpoint resumes"));
                    Test.TestTrue(TEXT("Route computation runs inside the native debugger loop"), PausedCache->GetBuildCount() > PreviousBuilds);
                    CheckDrawing(*PausedCache);
                }
                CheckFormattingBlocked(TEXT("play/simulation"));
                CloseGraph(); OpenGraph();
                DebugPhase = 2; DebugFrames = 0; return;
            }
            if (Test.TestTrue(TEXT("A graph opened at a breakpoint creates a route cache"), PausedCache.IsValid()))
            {
                CheckRoutes(*PausedCache, DebugPhase == 2 ? TEXT("Cold custom routes build while stopped at a breakpoint") :
                    TEXT("Wire style changes rebuild routes while stopped at a breakpoint"));
                CheckDrawing(*PausedCache);
            }
            if (DebugPhase == 2)
            {
                Capture(TEXT("GlooPrint-ExecutionPausedDiagonal.png"));
                GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
                GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
                DebugPhase = 3; DebugFrames = 0; return;
            }
            Capture(TEXT("GlooPrint-ExecutionPausedRounded.png"));
            bComplete = true;
            GEditor->SetPIEWorldsPaused(false); Slate.LeaveDebuggingMode();
        });
        Actor->ProcessEvent(Actor->FindFunctionChecked(TEXT("TraceRoute")), nullptr);
        Slate.OnPostTick().Remove(Handle);
        Test.TestTrue(TEXT("Real Blueprint bytecode hit the registered breakpoint"), bHit);
        Test.TestTrue(TEXT("Both warm and cold routing checks completed before resume"), bComplete);
        Test.TestTrue(TEXT("The native debugger returns to normal execution"), Slate.IsNormalExecution());
        RemoveBreakpoint();
    }
    void RemoveBreakpoint()
    {
        if (!bOwnsBreakpoint) { return; }
        GetMutableDefault<UBlueprintEditorSettings>()->bDrawMidpointArrowsInBlueprints = bOriginalArrows;
        FKismetDebugUtilities::ClearBreakpoints(Fixture->Blueprint.Get());
        Test.TestFalse(TEXT("The fixture leaves no breakpoint settings entry"),
            GetDefault<UBlueprintEditorSettings>()->PerBlueprintSettings.Contains(Fixture->Blueprint->GetPathName()));
        bOwnsBreakpoint = false;
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture actual execution wires"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save execution wire capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    void CloseGraph()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset();
    }
    bool Stop()
    {
        if (Fixture) { Fixture->Blueprint->SetObjectBeingDebugged(nullptr); }
        if (bOwnsPIE)
        {
            if (GEditor->GetPlaySessionRequest().IsSet()) { GEditor->CancelRequestPlaySession(); }
            if (GEditor->PlayWorld) { GEditor->RequestEndPlayMap(); }
        }
        Phase = EPhase::Stop; Deadline = FPlatformTime::Seconds() + 30;
        return false;
    }
    void Restore()
    {
        if (!bRestore) { return; }
        RemoveBreakpoint();
        if (bOwnsPIE && GEditor)
        {
            GEditor->CancelRequestPlaySession();
            if (GEditor->PlayWorld) { GEditor->RequestEndPlayMap(); }
        }
        CloseGraph();
        if (Fixture && GEditor)
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Fixture->Blueprint.Get());
        }
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
        GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
        GetMutableDefault<UBlueprintEditorSettings>()->bDrawMidpointArrowsInBlueprints = bOriginalArrows;
        bRestore = false;
    }
    enum class EPhase { Start, ReadOnly, AwaitPIE, Playing, Rebuilt, Diagonal, Stop };
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TStrongObjectPtr<ULevelEditorPlaySettings> PlaySettings;
    TWeakObjectPtr<UWorld> OriginalWorld;
    TWeakObjectPtr<AActor> Actor;
    UK2Node_CustomEvent* Entry = nullptr;
    UK2Node_IfThenElse* Branch = nullptr;
    UK2Node_VariableSet* TrueSet = nullptr;
    UK2Node_VariableSet* FalseSet = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TArray<uint8> Before;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    EPhase Phase = EPhase::Start;
    double Deadline = 0;
    int32 Frames = 0, Builds = 0;
    bool bOwnsPIE = false, bRestore = false, bOriginalArrows = false, bOwnsBreakpoint = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionWireTest, "GlooPrint.Editor.ExecutionWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FExecutionWireTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld || GEditor->GetPlaySessionRequest().IsSet() || FString(FApp::GetProjectName()) != TEXT("GlooPrintHost"))
    {
        AddError(TEXT("Execution wire test requires the idle disposable GlooPrintHost editor.")); return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FExecutionWireCheck(*this)); return true;
}
}
#endif
