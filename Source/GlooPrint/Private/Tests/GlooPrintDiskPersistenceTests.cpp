// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/CompilerResultsLog.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NodeFactory.h"
#include "Rendering/DrawElements.h"
#include "ScopedTransaction.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
namespace
{
FString PersistentGraphState(const UEdGraph& Graph, bool bIncludeTextIdentity = true)
{
    TArray<FString> Lines{TEXT("Graph=") + Graph.GraphGuid.ToString()};
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        const FString Key = Node->NodeGuid.ToString() + TEXT(".");
        Lines.Add(Key + Node->GetName() + TEXT("=") + Node->GetClass()->GetPathName());
        Lines.Add(Key + FString::Printf(TEXT("Bounds=%d,%d,%d,%d"), Node->NodePosX, Node->NodePosY, Node->NodeWidth, Node->NodeHeight));
        Lines.Add(Key + FString::Printf(TEXT("Enabled=%d"), int32(Node->GetDesiredEnabledState())));
        for (const TCHAR* Name : {TEXT("NodeComment"), TEXT("bCommentBubbleVisible"), TEXT("bCommentBubblePinned"),
            TEXT("AdvancedPinDisplay"), TEXT("FunctionReference"), TEXT("CustomFunctionName"), TEXT("CommentColor"),
            TEXT("FontSize"), TEXT("MoveMode"), TEXT("NodeDetails"), TEXT("bCommentBubbleVisible_InDetailsPanel"), TEXT("bColorCommentBubble")})
        {
            if (const FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), Name))
            {
                FString Value;
                if (const auto* Text = CastField<FTextProperty>(Property); Text && !bIncludeTextIdentity)
                {
                    Value = Text->GetPropertyValue_InContainer(Node).ToString();
                }
                else { Property->ExportText_InContainer(0, Value, Node, nullptr, Node, PPF_None); }
                Lines.Add(Key + Name + TEXT("=") + Value);
            }
        }
        for (int32 I = 0; I < Node->Pins.Num(); ++I)
        {
            FString Value; Node->Pins[I]->ExportTextItem(Value, PPF_None);
            Lines.Add(Key + FString::Printf(TEXT("Pin%d="), I) + Value);
        }
    }
    Lines.Sort(); return FString::Join(Lines, TEXT("\n"));
}

FString PersistentRouteState(const FRouteSet& Routes)
{
    TArray<FString> Lines;
    for (const auto& Pair : Routes.Wires)
    {
        const auto& K = Pair.Key;
        FString Line = K.FromNode.ToString() + TEXT("/") + K.FromPin.ToString() + TEXT("->") + K.ToNode.ToString() + TEXT("/") + K.ToPin.ToString();
        Line += FString::Printf(TEXT(" fallback%d"), int32(Pair.Value.Fallback));
        for (const auto& P : Pair.Value.Points) { Line += FString::Printf(TEXT(" %.9g,%.9g"), P.X, P.Y); }
        Lines.Add(MoveTemp(Line));
    }
    Lines.Sort(); return FString::Join(Lines, TEXT("\n"));
}
}

class FDiskPersistenceCheck final : public IAutomationLatentCommand
{
public:
    explicit FDiskPersistenceCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FDiskPersistenceCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!bInitialized)
        {
            bInitialized = true;
            bReader = FParse::Value(FCommandLine::Get(), TEXT("GlooPrintDiskRead="), RunId);
            if (!bReader) { RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits); }
            FGuid Parsed;
            if (!Test.TestTrue(TEXT("Persistence run has an owned GUID directory"), FGuid::ParseExact(RunId, EGuidFormats::Digits, Parsed))) { return true; }
            Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DiskPersistence") / RunId) + TEXT("/");
            Mount = TEXT("/GlooPrintDisk_") + RunId + TEXT("/"); PackageName = Mount + TEXT("BP_RoundTrip");
            IFileManager::Get().MakeDirectory(*Directory, true);
            FPackageName::RegisterMountPoint(Mount, Directory); bMounted = true;
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled;
            OriginalCursor = Slate.GetCursorPos(); bRestoreSettings = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Deadline = FPlatformTime::Seconds() + 90;
            if (bReader)
            {
                if (!Test.TestNull(TEXT("Fresh reader has never loaded the saved package"), FindPackage(nullptr, *PackageName)) || !StopModule()) { return Finish(); }
                if (!Test.TestTrue(TEXT("Read saved semantic baseline"), FFileHelper::LoadFileToString(ExpectedState, *(Directory / TEXT("graph.txt")))) ||
                    !Test.TestTrue(TEXT("Read saved route baseline"), FFileHelper::LoadFileToString(ExpectedRoutes, *(Directory / TEXT("routes.txt"))))) { return Finish(); }
                Package.Reset(LoadPackage(nullptr, *PackageName, LOAD_None));
                if (!Test.TestNotNull(TEXT("Native loader reads the package from disk"), Package.Get())) { return Finish(); }
                Blueprint.Reset(Cast<UBlueprint>(Package->FindAssetInPackage()));
                if (!Test.TestNotNull(TEXT("Saved asset is an ordinary Blueprint"), Blueprint.Get())) { return Finish(); }
                Graph = FindObject<UEdGraph>(Blueprint.Get(), TEXT("GeometryFixture"));
                if (!Test.TestNotNull(TEXT("Saved native K2 graph loads"), Graph)) { return Finish(); }
                PrimeTooltips();
                if (!CheckPersistent()) { return Finish(); }
                if (!Compile()) { return Finish(); }
                Test.TestTrue(TEXT("Compiling without GlooPrint retains persisted graph values"), ExpectedState == PersistentGraphState(*Graph));
            }
            else if (!CreateFixture()) { return Finish(); }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (auto* Event = Cast<UK2Node_CustomEvent>(Node)) { Entry = Event; }
                if (auto* FunctionNode = Cast<UK2Node_CallFunction>(Node); FunctionNode && FunctionNode->GetFunctionName() == TEXT("MakeLiteralString")) { Literal = FunctionNode; }
            }
            if (!Test.TestTrue(TEXT("Reload retains entry and editable literal node types"), Entry && Literal)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(bReader ? TEXT("GlooPrint disk reload") : TEXT("GlooPrint disk save")))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false);
            Slate.SetCursorPos(FVector2D::ZeroVector); SettleUntil = FPlatformTime::Seconds() + 1;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Disk persistence fixture exceeded its bounded deadline.")); return Finish(); }
        if (++Frames < 12 || FPlatformTime::Seconds() < SettleUntil) { return false; }
        if (!bReader) { return AdvanceWriter(); }
        auto* Panel = Editor->GetGraphPanel();
        if (Phase == 0)
        {
            if (!CheckPersistent() || !CheckNativeWires()) { return Finish(); }
            Test.TestFalse(TEXT("Plugin remains stopped after native load/compile/paint"), FModuleManager::Get().IsModuleLoaded(TEXT("GlooPrint")));
            Test.TestFalse(TEXT("Stopped plugin has no route cache"), Panel->GetMetaData<FRouteCache>().IsValid());
            Test.TestFalse(TEXT("Stopped plugin has no measurement cache"), Panel->GetMetaData<FMeasurementCache>().IsValid());
            Capture(TEXT("ReloadedNative"));
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, HoverPoint);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            UEdGraphPin* From = nullptr; UEdGraphPin* To = nullptr;
            Test.TestTrue(TEXT("Native reloaded wire hover identifies the original saved pins"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, From, To) &&
                ((From == HoverFrom && To == HoverTo) || (From == HoverTo && To == HoverFrom)));
            auto* Value = Literal->FindPinChecked(TEXT("Value"));
            const auto BeforeEdit = SerializeTransactionValues(*Graph);
            const int32 AppliedQueue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("Edit saved native Blueprint default")));
                Value->Modify();
                GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Value, TEXT("Edited with GlooPrint stopped"));
            }
            Test.TestEqual(TEXT("Native pin editing changes the reloaded default"), Value->DefaultValue, FString(TEXT("Edited with GlooPrint stopped")));
            Test.TestEqual(TEXT("Native editing remains one undo action"), GEditor->Trans->GetQueueLength(), AppliedQueue + 1);
            const auto Edited = SerializeTransactionValues(*Graph);
            Test.TestTrue(TEXT("Native saved-asset edit undoes"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Native undo restores every original graph value"), BeforeEdit == SerializeTransactionValues(*Graph));
            Test.TestTrue(TEXT("Native saved-asset edit redoes"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Native redo restores every edited graph value"), Edited == SerializeTransactionValues(*Graph));
            Test.TestTrue(TEXT("Restore the saved default through native undo"), GEditor->UndoTransaction());
            if (!CheckPersistent() || Test.HasAnyErrors()) { return Finish(); }
            Package->SetDirtyFlag(false);
            if (!StartModule()) { return Finish(); }
            Phase = 2; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 2)
        {
            CheckPersistent();
            Test.TestEqual(TEXT("Reopened rounded routing retains every saved corridor and pin pair"), PersistentRouteState(Cache->GetRoutes()), ExpectedRoutes);
            Test.TestEqual(TEXT("Reopened saved routes require no native fallback"), Cache->GetRoutes().FallbackCount, 0);
            Test.TestFalse(TEXT("Automatic routing after module restart leaves the saved asset clean"), Package->IsDirty());
            RequestNoOp(); Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            CheckNoOp(); Capture(TEXT("ReloadedRounded"));
            auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = EGlooPrintWireStyle::Diagonal45; Settings->NotifyChanged();
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            FFormatPlan Cold; FString Reason;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Reloaded diagonal graph replans with cold geometry"), PlanFormatGraph(Graph, Scale, {Entry->NodeGuid}, Cold, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestEqual(TEXT("Reopened diagonal live routing matches cold routes"), PersistentRouteState(Cache->GetRoutes()), PersistentRouteState(Cold.Routes));
            Test.TestEqual(TEXT("Reopened diagonal routes require no fallback"), Cache->GetRoutes().FallbackCount, 0);
            CheckPersistent(); RequestNoOp(); Phase = 5; Frames = 0; return false;
        }
        CheckNoOp(); Capture(TEXT("ReloadedDiagonal"));
        return Finish();
    }
private:
    bool CreateFixture()
    {
        Package.Reset(CreatePackage(*PackageName)); Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false, Package.Get());
        Blueprint.Reset(Fixture->Blueprint.Get()); Graph = Fixture->Graph;
        Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("DiskRoundTrip");
        auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>({700, 330});
        auto* First = Call(TEXT("PrintString"), {450, 200}); auto* Second = Call(TEXT("PrintString"), {1050, 650});
        Literal = Call(TEXT("MakeLiteralString"), {100, 600}); Literal->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("Saved shared message");
        auto* Exec = Fixture->Add<UK2Node_Knot>({450, 50}); auto* Data = Fixture->Add<UK2Node_Knot>({900, 500});
        Link(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Exec->GetInputPin());
        Link(Exec->GetOutputPin(), Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        Link(Sequence->GetThenPinGivenIndex(0), First->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        Link(Sequence->GetThenPinGivenIndex(1), Second->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        Link(Literal->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Data->GetInputPin());
        Link(Data->GetOutputPin(), First->FindPinChecked(TEXT("InString"))); Link(Data->GetOutputPin(), Second->FindPinChecked(TEXT("InString")));
        auto* Comment = Fixture->Add<UEdGraphNode_Comment>({-80, -140}); Comment->NodeWidth = 1650; Comment->NodeHeight = 1050;
        Comment->NodeComment = TEXT("Saved graph with existing reroutes"); Comment->NodeDetails = FText::FromString(TEXT("Persistent comment details"));
        Comment->CommentColor = FLinearColor(0.12f, 0.3f, 0.5f); Comment->bCommentBubbleVisible_InDetailsPanel = false;
        auto* Empty = Fixture->Add<UEdGraphNode_Comment>({-700, -400}); Empty->NodeWidth = 280; Empty->NodeHeight = 140;
        Empty->NodeComment = TEXT("Keep this empty comment"); Empty->MoveMode = ECommentBoxMode::NoGroupMovement; Empty->bCommentBubbleVisible_InDetailsPanel = false;
        for (int32 I = 0; I < Graph->Nodes.Num(); ++I) { Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
        return bLinksValid && Compile();
    }
    UK2Node_CallFunction* Call(FName Function, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Function)); Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B) { bLinksValid &= Test.TestTrue(TEXT("Native schema accepts saved fixture link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B)); }
    void PrimeTooltips()
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
        }
    }
    bool Compile()
    {
        FCompilerResultsLog Results; FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Results);
        PrimeTooltips();
        return Test.TestEqual(TEXT("Persistence Blueprint compiles with native nodes"), Results.NumErrors, 0);
    }
    bool AdvanceWriter()
    {
        if (Phase == 4)
        {
            if (FPlatformProcess::IsProcRunning(Reader)) { return false; }
            int32 ExitCode = -1;
            Test.TestTrue(TEXT("Fresh reader process returns an exit status"), FPlatformProcess::GetProcReturnCode(Reader, &ExitCode));
            Test.TestEqual(TEXT("Fresh reader exits normally"), ExitCode, 0);
            FPlatformProcess::CloseProc(Reader); Reader.Reset();
            FString Completed;
            Test.TestTrue(TEXT("Fresh reader completes all native disk assertions"), FFileHelper::LoadFileToString(Completed, *(Directory / TEXT("verified.txt"))) && Completed == RunId);
            Test.AddInfo(TEXT("Disk persistence evidence and fresh-editor report: ") + Directory);
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        if (Phase == 0)
        {
            Before = SerializeTransactionValues(*Graph); Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            FSlateApplication::Get().SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats the asset before saving"), PressF()); Phase = 1; return false;
        }
        if (Phase == 1)
        {
            if (Before == SerializeTransactionValues(*Graph)) { return false; }
            const auto Formatted = SerializeTransactionValues(*Graph);
            Test.TestEqual(TEXT("Saved layout is one normal format transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Pre-save format undoes"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Pre-save undo restores exact original graph"), Before == SerializeTransactionValues(*Graph));
            Test.TestTrue(TEXT("Pre-save format redoes"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Pre-save redo restores exact formatted graph"), Formatted == SerializeTransactionValues(*Graph));
            Phase = 2; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>(); if (!Cache || !Cache->IsReady()) { return false; }
        Test.TestEqual(TEXT("Saved fixture contains all seven original wire pairs"), Cache->GetRoutes().Wires.Num(), 7);
        Test.TestEqual(TEXT("Saved fixture needs no routing fallback"), Cache->GetRoutes().FallbackCount, 0);
        ExpectedState = PersistentGraphState(*Graph); ExpectedRoutes = PersistentRouteState(Cache->GetRoutes());
        if (Test.HasAnyErrors()) { return Finish(); }
        const FString BeforeSave = PersistentGraphState(*Graph, false);
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.Error = GLog; Args.bSlowTask = false;
        if (!Test.TestTrue(TEXT("Native package writer saves the formatted Blueprint"), UPackage::SavePackage(Package.Get(), Blueprint.Get(), *(Directory / TEXT("BP_RoundTrip.uasset")), Args))) { return Finish(); }
        if (!Test.TestTrue(TEXT("Saving preserves formatted graph semantics"), BeforeSave == PersistentGraphState(*Graph, false)))
        {
            FFileHelper::SaveStringToFile(PersistentGraphState(*Graph), *(Directory / TEXT("after-save-graph.txt")));
        }
        ExpectedState = PersistentGraphState(*Graph);
        Test.TestTrue(TEXT("Native package bytes exist on disk"), IFileManager::Get().FileSize(*(Directory / TEXT("BP_RoundTrip.uasset"))) > 0);
        Test.TestTrue(TEXT("Record persisted graph values"), FFileHelper::SaveStringToFile(ExpectedState, *(Directory / TEXT("graph.txt"))));
        Test.TestTrue(TEXT("Record persisted route values"), FFileHelper::SaveStringToFile(ExpectedRoutes, *(Directory / TEXT("routes.txt"))));
        Capture(TEXT("SavedRounded"));
        Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
        if (Test.HasAnyErrors()) { return Finish(); }
        const FString Arguments = FString::Printf(TEXT("\"%s\" -unattended -nosplash -GlooPrintDiskRead=%s -ExecCmds=\"Automation SetFilter Engine,Automation RunTests GlooPrint.Editor.DiskPersistence\" -TestExit=\"Automation Test Queue Empty\" -ReportExportPath=\"%s\" -abslog=\"%s\""),
            *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *RunId, *(Directory / TEXT("ReaderReport")), *(Directory / TEXT("reader.log")));
        uint32 ProcessId = 0;
        Reader = FPlatformProcess::CreateProc(FPlatformProcess::ExecutablePath(), *Arguments, false, false, false, &ProcessId, 0, nullptr, nullptr);
        if (!Test.TestTrue(TEXT("Launch a fresh native editor for disk read-back"), Reader.IsValid())) { return Finish(); }
        Test.AddInfo(FString::Printf(TEXT("Fresh disk reader PID%u; evidence %s"), ProcessId, *Directory));
        Deadline = FPlatformTime::Seconds() + 180; Phase = 4; return false;
    }
    bool CheckPersistent()
    {
        Test.TestEqual(TEXT("Disk round-trip retains every original node and comment"), Graph->Nodes.Num(), 9);
        int32 Knots = 0; for (UEdGraphNode* Node : Graph->Nodes) { Knots += Node->IsA<UK2Node_Knot>() ? 1 : 0; }
        Test.TestEqual(TEXT("Disk round-trip retains both real reroute nodes"), Knots, 2);
        const FString Actual = PersistentGraphState(*Graph);
        if (!Test.TestTrue(TEXT("Disk round-trip preserves GUIDs, topology, defaults, comment values and layout"), Actual == ExpectedState))
        {
            FFileHelper::SaveStringToFile(Actual, *(Directory / TEXT("actual-graph.txt"))); return false;
        }
        return true;
    }
    bool CheckNativeWires()
    {
        auto* Panel = Editor->GetGraphPanel(); FArrangedChildren Nodes(EVisibility::Visible);
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Reloaded node has a native widget"), Widget.IsValid())) { return false; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        int32 Links = 0;
        for (UEdGraphNode* Node : Graph->Nodes) for (UEdGraphPin* From : Node->Pins)
        {
            if (From->Direction != EGPD_Output) { continue; }
            for (UEdGraphPin* To : From->LinkedTo)
            {
                ++Links; TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
                for (UEdGraphPin* Pin : {From, To})
                {
                    const auto Widget = Panel->GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid)->FindWidgetForPin(Pin);
                    if (!Test.TestTrue(TEXT("Reloaded wire has both native pin widgets"), Widget.IsValid())) { return false; }
                    Pins.Add(Widget.ToSharedRef(), FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
                }
                const FSlateRect Clip(-100000, -100000, 100000, 100000); FSlateWindowElementList ActualElements(Window), NativeElements(Window);
                TUniquePtr<FConnectionDrawingPolicy> Actual(FNodeFactory::CreateConnectionPolicy(Graph->GetSchema(), 0, 1, Scale, Clip, ActualElements, Graph));
                FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Graph);
                if (!Test.TestTrue(TEXT("Unloaded plugin leaves a native drawing policy"), Actual.IsValid())) { return false; }
                Actual->SetAbsoluteMousePosition(FVector2f(-100000)); Native.SetAbsoluteMousePosition(FVector2f(-100000));
                Actual->Draw(Pins, Nodes); Native.Draw(Pins, Nodes);
                const auto& A = ActualElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                const auto& B = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                if (!Test.TestTrue(TEXT("Every saved connection draws exactly one ordinary native spline"), A.Num() == 1 && B.Num() == 1)) { return false; }
                Test.TestTrue(TEXT("Stopped plugin retains native endpoints, control points, color and thickness"),
                    A[0].P0.Equals(B[0].P0, 0.1f) && A[0].P1.Equals(B[0].P1, 0.1f) && A[0].P2.Equals(B[0].P2, 0.1f) && A[0].P3.Equals(B[0].P3, 0.1f) &&
                    A[0].GetTint().Equals(B[0].GetTint(), 0.001f) && FMath::IsNearlyEqual(A[0].GetThickness(), B[0].GetThickness(), 0.001f));
                if (Node != Literal) { continue; }
                HoverFrom = From; HoverTo = To;
                HoverPoint = (B[0].P0 + 3 * B[0].P1 + 3 * B[0].P2 + B[0].P3) * 0.125f;
            }
        }
        Test.TestEqual(TEXT("Stopped plugin draws all seven saved connections"), Links, 7);
        return Test.TestTrue(TEXT("Saved shared-data wire remains available for native hover"), HoverFrom && HoverTo);
    }
    void RequestNoOp()
    {
        NoOpBefore = SerializeTransactionValues(*Graph); Queue = GEditor->Trans->GetQueueLength(); UndoCount = GEditor->Trans->GetUndoCount();
        FSlateApplication::Get().SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        Test.TestTrue(TEXT("Reloaded graph accepts actual F"), PressF());
    }
    void CheckNoOp()
    {
        CheckPersistent(); Test.TestTrue(TEXT("Reloaded F is an exact no-op"), NoOpBefore == SerializeTransactionValues(*Graph));
        Test.TestEqual(TEXT("Reloaded no-op adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestEqual(TEXT("Reloaded no-op preserves redo history"), GEditor->Trans->GetUndoCount(), UndoCount);
        Test.TestFalse(TEXT("Reloaded no-op leaves the saved asset clean"), Package->IsDirty());
    }
    bool PressF() { return FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)); }
    bool StopModule()
    {
        auto& Modules = FModuleManager::Get(); auto* Module = Modules.GetModule(TEXT("GlooPrint"));
        if (!Test.TestNotNull(TEXT("Reader starts with the registered plugin module"), Module)) { return false; }
        Module->PreUnloadCallback(); bNeedsModule = Modules.UnloadModule(TEXT("GlooPrint"), false, false);
        return Test.TestTrue(TEXT("Reader stops GlooPrint before loading the saved asset"), bNeedsModule);
    }
    bool StartModule()
    {
        if (!bNeedsModule) { return true; }
        const bool Loaded = FModuleManager::Get().LoadModuleWithCallback(TEXT("GlooPrint"), *GLog);
        Test.TestTrue(TEXT("Restart GlooPrint after native disk checks"), Loaded); bNeedsModule = !Loaded; return Loaded;
    }
    void Capture(const TCHAR* Stage)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture actual persisted Blueprint"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save persistence screenshot"), FFileHelper::SaveArrayToFile(Png, *(Directory / (FString(Stage) + TEXT(".png")))));
        }
    }
    bool Finish()
    {
        Restore();
        if (bReader && !Test.HasAnyErrors()) { Test.TestTrue(TEXT("Publish successful fresh-reader verification"), FFileHelper::SaveStringToFile(RunId, *(Directory / TEXT("verified.txt")))); }
        return true;
    }
    void Restore()
    {
        if (Reader.IsValid())
        {
            if (FPlatformProcess::IsProcRunning(Reader)) { FPlatformProcess::TerminateProc(Reader); }
            FPlatformProcess::CloseProc(Reader); Reader.Reset();
        }
        if (Window) { Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset(); }
        StartModule();
        if (bRestoreSettings)
        {
            bRestoreSettings = false; auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
            Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
            FSlateApplication::Get().SetCursorPos(OriginalCursor);
        }
        if (Package) { Package->SetDirtyFlag(false); }
        if (bMounted) { FPackageName::UnRegisterMountPoint(Mount, Directory); bMounted = false; }
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TStrongObjectPtr<UPackage> Package;
    TStrongObjectPtr<UBlueprint> Blueprint;
    UEdGraph* Graph = nullptr;
    UK2Node_CustomEvent* Entry = nullptr;
    UK2Node_CallFunction* Literal = nullptr;
    UEdGraphPin* HoverFrom = nullptr;
    UEdGraphPin* HoverTo = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FProcHandle Reader;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FVector2D OriginalCursor;
    FVector2f HoverPoint;
    FString RunId, Directory, Mount, PackageName, ExpectedState, ExpectedRoutes;
    TArray<uint8> Before, NoOpBefore;
    int32 Phase = 0, Frames = 0, Queue = 0, UndoCount = 0;
    double Deadline = 0, SettleUntil = 0;
    bool bInitialized = false, bReader = false, bMounted = false, bRestoreSettings = false, bOriginalEnabled = true, bNeedsModule = false, bLinksValid = true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiskPersistenceTest, "GlooPrint.Editor.DiskPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDiskPersistenceTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDiskPersistenceCheck(*this)); return true;
}
}
#endif
