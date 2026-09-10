#include "Modules/ModuleManager.h"
IMPLEMENT_MODULE(FDefaultModuleImpl, BlueprintAssistBaseline)

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/GlooPrintNativeBenchmarkFixture.h"
#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistTabHandler.h"
#include "BlueprintEditor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SGraphPanel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/PackageName.h"
#include "NodeFactory.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "UObject/LinkerInstancingContext.h"
#include "Widgets/SWindow.h"

namespace
{
TMap<FString, FString> CaptureValues(const UEdGraph& Graph)
{
    TMap<FString, FString> Values;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        for (TFieldIterator<FProperty> Property(Node->GetClass()); Property; ++Property)
        {
            FString Value;
            Property->ExportText_InContainer(0, Value, Node, nullptr, Node, PPF_DuplicateVerbatim);
            Values.Add(Node->GetName() + TEXT(".") + Property->GetName(), MoveTemp(Value));
        }
        for (int32 I = 0; I < Node->Pins.Num(); ++I)
        {
            FString Value; Node->Pins[I]->ExportTextItem(Value, PPF_DuplicateVerbatim);
            const FString Key = FString::Printf(TEXT("%s.Pin%d"), *Node->GetName(), I);
            Values.Add(Key, MoveTemp(Value)); Values.Add(Key + TEXT(".Tooltip"), Node->Pins[I]->PinToolTip);
            Values.Add(Key + TEXT(".SourceIndex"), LexToString(Node->Pins[I]->SourceIndex));
        }
    }
    return Values;
}

class FReferenceFormat final : public IAutomationLatentCommand
{
public:
    FReferenceFormat(FAutomationTestBase& InTest, int32 InCount) : Test(InTest), Count(InCount) {}
    virtual ~FReferenceFormat() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(FString::Printf(TEXT("Blueprint Assist benchmark timed out in stage %d; no accepted latency."), Phase));
            return Finish();
        }
        if (Phase == 3)
        {
            if (++Frames < 12) { return false; }
            Capture(); return Finish();
        }
        if (Phase == 0)
        {
            const auto Active = FBATabHandler::Get().GetActiveGraphHandler();
            if (!Active || Active->GetFocusedEdGraph() != Fixture->Graph) { return false; }
            Handler = Active; CacheStarted = FPlatformTime::Seconds(); Handler->RefreshAllNodeSizes();
            Deadline = CacheStarted + 240;
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            if (++Frames < 12 || Handler->IsCalculatingNodeSize() || Handler->IsLerpingViewport()) { return false; }
            if (CacheMs == 0) { CacheMs = (FPlatformTime::Seconds() - CacheStarted) * 1000; }
            Editor->ClearSelectionSet(); Editor->SetNodeSelection(Entry, true);
            Before = CaptureValues(*Fixture->Graph); Editor->GetViewLocation(View, Zoom);
            if (Original.IsEmpty()) { Original = Before; }
            Completed = 0; CompletedFrame = 0; CompletionCount = 0;
            Anchor = FIntPoint(Entry->NodePosX, Entry->NodePosY);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            OnComplete = Handler->OnPostFormatting.AddLambda([this]()
            {
                Completed = FPlatformTime::Seconds(); CompletedFrame = GFrameCounter; ++CompletionCount;
            });
            FSlateApplication::Get().SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            StartedFrame = GFrameCounter; Started = FPlatformTime::Seconds();
            bool bHandled = false;
            {
                TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ReferenceFKey);
                bHandled = FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            }
            Dispatched = FPlatformTime::Seconds();
            FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            if (!Test.TestTrue(TEXT("Blueprint Assist accepts the native F key"), bHandled)) { return Finish(); }
            Phase = 2; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
        }
        if (Phase == 2)
        {
            if (!Completed || Handler->HasActiveTransaction()) { return false; }
            const double Observed = FPlatformTime::Seconds();
            Handler->OnPostFormatting.Remove(OnComplete);
            Test.TestEqual(TEXT("One reference formatting completion"), CompletionCount, 1);
            Test.TestEqual(TEXT("No inserted or deleted nodes"), Fixture->Graph->Nodes.Num(), Count);
            const auto After = CaptureValues(*Fixture->Graph);
            int32 Changed = 0, Links = 0;
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const FString Prefix = Node->GetName();
                if (Before.FindChecked(Prefix + TEXT(".NodePosX")) != After.FindChecked(Prefix + TEXT(".NodePosX")) ||
                    Before.FindChecked(Prefix + TEXT(".NodePosY")) != After.FindChecked(Prefix + TEXT(".NodePosY"))) { ++Changed; }
                for (UEdGraphPin* Pin : Node->Pins) if (Pin->Direction == EGPD_Output) { Links += Pin->LinkedTo.Num(); }
            }
            if (!bRepeat) { Test.TestTrue(TEXT("Reference F changes the unformatted chain"), Changed > 0); }
            Test.TestEqual(TEXT("Every original connection remains"), Links, Count - 1);
            Compare(Before, After, true, TEXT("F"));
            const int32 AddedTransactions = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - Queue;
            if (!bRepeat) { Test.TestEqual(TEXT("Reference command creates one undo entry"), AddedTransactions, 1); }
            else { Test.TestTrue(TEXT("Repeat creates at most one transaction"), AddedTransactions >= 0 && AddedTransactions <= 1); }
            FVector2f EndView; float EndZoom = 0; Editor->GetViewLocation(EndView, EndZoom);
            const bool bAnchor = Anchor == FIntPoint(Entry->NodePosX, Entry->NodePosY);
            const bool bView = View == EndView && Zoom == EndZoom;
            Csv += FString::Printf(TEXT("%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%llu,%d,%d,%d,%d,%s,%d\n"), Count, Pins, Links, CacheMs,
                (Dispatched - Started) * 1000, (FMath::Max(Completed, Dispatched) - Started) * 1000,
                (Observed - Started) * 1000, CompletedFrame - StartedFrame, Changed, int32(bAnchor), int32(bView),
                Sample, bRepeat ? TEXT("Repeat") : TEXT("Format"), AddedTransactions);
            Test.TestTrue(TEXT("Reference preserves its anchor and camera"), bAnchor && bView);
            Test.AddInfo(FString::Printf(TEXT("Reference sample %d %s: dispatch %.3f ms, completion %.3f ms, changed %d, added transactions %d."),
                Sample, bRepeat ? TEXT("Repeat") : TEXT("Format"), (Dispatched - Started) * 1000,
                (FMath::Max(Completed, Dispatched) - Started) * 1000, Changed, AddedTransactions));
            if (!bRepeat)
            {
                Formatted = After;
                Test.TestTrue(TEXT("Reference format undoes"), GEditor->UndoTransaction()); Compare(Before, CaptureValues(*Fixture->Graph), false, TEXT("Undo"));
                Test.TestTrue(TEXT("Reference format redoes"), GEditor->RedoTransaction()); Compare(After, CaptureValues(*Fixture->Graph), false, TEXT("Redo"));
                bRepeat = true;
            }
            else
            {
                if (AddedTransactions == 1)
                {
                    Test.TestTrue(TEXT("Reference repeat undoes"), GEditor->UndoTransaction());
                    Compare(Formatted, CaptureValues(*Fixture->Graph), false, TEXT("Repeat undo"));
                }
                else { Compare(Formatted, After, false, TEXT("Repeat without transaction")); }
                if (++Sample >= SamplesWanted)
                {
                    Editor->ZoomToFit(false); Phase = 3; Frames = 0; return false;
                }
                Test.TestTrue(TEXT("Restore original layout for next reference sample"), GEditor->UndoTransaction());
                Compare(Original, CaptureValues(*Fixture->Graph), false, TEXT("Next sample"));
                bRepeat = false;
            }
            if (Test.HasAnyErrors()) { return Finish(); }
            Phase = 1; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
        }
        return false;
    }
private:
    bool Start()
    {
        auto& Settings = UBASettings::GetMutable(); OriginalAuto = Settings.bGloballyDisableAutoFormatting;
        OriginalKnots = Settings.bCreateKnotNodes; bRestore = true;
        Settings.bGloballyDisableAutoFormatting = true; Settings.bCreateKnotNodes = false;
        OriginalCursor = FSlateApplication::Get().GetCursorPos();
        if (FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted)) { Sample = -1; }
        SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100);
        Directory = FPaths::ProjectSavedDir() / TEXT("ReferenceBenchmarks"); IFileManager::Get().MakeDirectory(*Directory, true);
        Test.AddInfo(FString::Printf(TEXT("Reference settings: node padding=%d,%d; parameter padding=%d,%d; formatting style=%d; parameter style=%d; knot creation=off; auto format=off; explicit native size refresh before timed F."),
            Settings.BlueprintFormatterSettings.Padding.X, Settings.BlueprintFormatterSettings.Padding.Y,
            Settings.BlueprintParameterPadding.X, Settings.BlueprintParameterPadding.Y, int32(Settings.FormattingStyle), int32(Settings.ParameterStyle)));
        Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintReference_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
        Fixture = MakeUnique<GlooPrint::Tests::FFixture>(UObject::StaticClass(), false, Package.Get());
        if (!GlooPrint::Tests::PopulateNativeChain(Test, *Fixture, Count, 64, Entry, Pins)) { return Finish(); }
        auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!Test.TestTrue(TEXT("Open owned Blueprint in the real editor"), Editors->OpenEditorForAsset(Fixture->Blueprint.Get()))) { return Finish(); }
        auto* Instance = Editors->FindEditorForAsset(Fixture->Blueprint.Get(), true);
        if (!Test.TestTrue(TEXT("The asset opened in BlueprintEditor"), Instance && Instance->GetEditorName() == TEXT("BlueprintEditor"))) { return Finish(); }
        Editor = static_cast<FBlueprintEditor*>(Instance)->OpenGraphAndBringToFront(Fixture->Graph);
        if (!Test.TestTrue(TEXT("Owned graph editor is available"), Editor.IsValid())) { return Finish(); }
        Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false); FSlateApplication::Get().SetCursorPos(FVector2D::ZeroVector);
        Deadline = FPlatformTime::Seconds() + 180; return false;
    }
    void Compare(const TMap<FString, FString>& A, const TMap<FString, FString>& B, bool bIgnorePositions, const TCHAR* Stage)
    {
        if (!Test.TestEqual(FString(Stage) + TEXT(" preserves value count"), B.Num(), A.Num())) { return; }
        for (const auto& Pair : A)
        {
            if (bIgnorePositions && (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY")))) { continue; }
            const auto* Value = B.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) { Test.AddError(FString(Stage) + TEXT(" changed ") + Pair.Key); return; }
        }
    }
    void Capture()
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture reference result"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        Test.TestTrue(TEXT("Save reference capture"), FFileHelper::SaveArrayToFile(Png, *(Directory / FString::Printf(TEXT("%d-BlueprintAssist-PinHeavy.png"), Count))));
    }
    bool Finish()
    {
        if (!Directory.IsEmpty()) { Test.TestTrue(TEXT("Save reference raw timings"), FFileHelper::SaveStringToFile(Csv, *(Directory / FString::Printf(TEXT("%d-BlueprintAssist-PinHeavy.csv"), Count)))); }
        Restore(); return true;
    }
    void Restore()
    {
        if (Handler) { Handler->OnPostFormatting.Remove(OnComplete); Handler->CancelActiveFormatting(); Handler.Reset(); }
        if (Package) { Package->SetDirtyFlag(false); }
        if (Fixture && GEditor) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Fixture->Blueprint.Get()); }
        Editor.Reset();
        if (bRestore)
        {
            bRestore = false; auto& Settings = UBASettings::GetMutable(); Settings.bGloballyDisableAutoFormatting = OriginalAuto; Settings.bCreateKnotNodes = OriginalKnots;
            FSlateApplication::Get().SetCursorPos(OriginalCursor);
        }
    }
    FAutomationTestBase& Test;
    int32 Count, Pins = 0, Phase = 0, Frames = 0, CompletionCount = 0, Queue = 0;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<GlooPrint::Tests::FFixture> Fixture;
    UK2Node_ExecutionSequence* Entry = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<FBAGraphHandler> Handler;
    FDelegateHandle OnComplete;
    TMap<FString, FString> Before, Original, Formatted;
    FVector2D OriginalCursor;
    FVector2f View;
    FIntPoint Anchor;
    float Zoom = 0;
    double Deadline = 0, CacheStarted = 0, CacheMs = 0, Started = 0, Dispatched = 0, Completed = 0;
    uint64 StartedFrame = 0, CompletedFrame = 0;
    bool bRestore = false, OriginalAuto = false, OriginalKnots = true, bRepeat = false;
    int32 Sample = 0, SamplesWanted = 1;
    FString Directory;
    FString Csv = TEXT("nodes,pins,links,initial_cache_ms,dispatch_ms,completion_ms,transaction_observed_ms,completion_frames,changed_nodes,anchor_retained,view_retained,sample,operation,added_transactions\n");
};
#include "ReferenceWirePaint.h"

class FReferenceVisual final : public IAutomationLatentCommand
{
    struct FPlacement { FIntPoint Position; FVector2f Size; };
public:
    FReferenceVisual(FAutomationTestBase& InTest, const FString& InAsset, bool bInElectronicNodes = false)
        : Test(InTest), Asset(InAsset), bElectronicNodes(bInElectronicNodes) {}
    virtual ~FReferenceVisual() { Restore(); }
    virtual bool Update() override
    {
        if (!bStarted) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Reference visual fixture exceeded its 90-second deadline.")); return Finish(); }
        if (Phase == 0)
        {
            if (bElectronicNodes) { Phase = 1; Frames = 0; return false; }
            const auto Active = FBATabHandler::Get().GetActiveGraphHandler();
            if (!Active || Active->GetFocusedEdGraph() != Graph) { return false; }
            Handler = Active; Handler->RefreshAllNodeSizes(); Phase = 1; Frames = 0; return false;
        }
        if (++Frames < 12) { return false; }
        if (Phase == 1)
        {
            if (!bElectronicNodes && (Handler->IsCalculatingNodeSize() || Handler->IsLerpingViewport())) { return false; }
            PrimeTooltips(); Before = CaptureValues(*Graph); ReadPlacement(0);
            if (bElectronicNodes)
            {
                After = Before; Placements[1] = Placements[0];
                Queue = GEditor->Trans->GetQueueLength();
                return BeginCaptures() ? false : Finish();
            }
            if (!CaptureTerminals(BeforeTerminals, BeforeLinks)) { return Finish(); }
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                OriginalClasses.Add(Node->GetName(), Node->GetClass()->GetFName());
                if (Node->IsA<UK2Node_Knot>()) { OriginalReroutes.Add(Node->GetName()); }
            }
            Editor->ClearSelectionSet(); Editor->SetNodeSelection(Entry, true);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            CompleteHandle = Handler->OnPostFormatting.AddLambda([this]() { bCompleted = true; });
            auto& Slate = FSlateApplication::Get(); Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            if (!Test.TestTrue(TEXT("Reference accepts F for the authored construction graph"),
                Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)))) { return Finish(); }
            Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 2; return false;
        }
        if (Phase == 2)
        {
            if (!bCompleted || Handler->HasActiveTransaction() || Handler->IsLerpingViewport()) { return false; }
            Handler->OnPostFormatting.Remove(CompleteHandle); Frames = 0; Phase = 3; return false;
        }
        if (Phase == 3)
        {
            After = CaptureValues(*Graph);
            TMap<FString,FString> CurrentTerminals; TArray<FString> CurrentLinks;
            if (!CaptureTerminals(CurrentTerminals, CurrentLinks) ||
                !Compare(BeforeTerminals, CurrentTerminals, true, TEXT("Reference F terminal properties")) ||
                !Test.TestTrue(TEXT("Reference F preserves every effective terminal connection through authored reroutes"), BeforeLinks == CurrentLinks)) { return Finish(); }
            TSet<FString> Remaining;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const FName* OriginalClass = OriginalClasses.Find(Node->GetName()); Remaining.Add(Node->GetName());
                if (!Test.TestTrue(TEXT("Reference adds no replacement nodes with knot creation disabled"), OriginalClass && *OriginalClass == Node->GetClass()->GetFName())) { return Finish(); }
            }
            for (const auto& Pair : OriginalClasses)
            {
                if (!Test.TestTrue(TEXT("Reference removes only known authored reroutes"), Remaining.Contains(Pair.Key) || OriginalReroutes.Contains(Pair.Key))) { return Finish(); }
            }
            Notes += FString::Printf(TEXT("Reference topology: original nodes=%d, formatted nodes=%d, removed authored reroutes=%d; effective terminal links=%d\n"),
                OriginalClasses.Num(),Graph->Nodes.Num(),OriginalClasses.Num()-Graph->Nodes.Num(),BeforeLinks.Num());
            bool bMoved = false;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                bMoved |= Placements[0].FindChecked(Node->GetName()).Position != FIntPoint(Node->NodePosX,Node->NodePosY);
            }
            Test.TestTrue(TEXT("Reference F moves surviving authored nodes"), bMoved);
            Test.TestEqual(TEXT("Reference F creates one undo action"), GEditor->Trans->GetQueueLength(), Queue + 1);
            if (!Test.TestTrue(TEXT("Reference F undoes"), GEditor->UndoTransaction()) ||
                !Compare(Before, CaptureValues(*Graph), false, TEXT("Reference undo"))) { return Finish(); }
            if (!Test.TestTrue(TEXT("Reference F redoes"), GEditor->RedoTransaction()) ||
                !Compare(After, CaptureValues(*Graph), false, TEXT("Reference redo"))) { return Finish(); }
            bReferenceApplied = true;
            if (!Compile()) { return Finish(); }
            PrimeTooltips(); After = CaptureValues(*Graph); Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            ReadPlacement(1);
            if (!Test.TestTrue(TEXT("Return to original topology for replay inputs"), GEditor->UndoTransaction())) { return Finish(); }
            bReferenceApplied = false;
            if (!Compare(Before, CaptureValues(*Graph), false, TEXT("Original topology before replay"))) { return Finish(); }
            return BeginCaptures() ? false : Finish();
        }
        if (!CheckAndExport()) { return Finish(); }
        if (bCapturePass) { Capture(); }
        CloseCapture();
        if (++Stage < 3) { return OpenCapture() ? false : Finish(); }
        if (!bCapturePass) { bCapturePass = true; Stage = 0; return OpenCapture() ? false : Finish(); }
        TArray<uint8> Current;
        Test.TestTrue(TEXT("Installed authored asset remains byte-identical"), FFileHelper::LoadFileToArray(Current, *SourcePath) && Current == SourceBytes);
        Test.TestTrue(TEXT("Private baseline asset file remains byte-identical"), FFileHelper::LoadFileToArray(Current, *CopyPath) && Current == SourceBytes);
        Test.TestTrue(TEXT("Save comparison settings and provenance"), FFileHelper::SaveStringToFile(Notes, *(Directory / TEXT("comparison.txt"))));
        if (bElectronicNodes) { Test.TestEqual(TEXT("Drawing replay creates no transaction"),GEditor->Trans->GetQueueLength(),Queue); }
        Test.AddInfo(TEXT("Matched reference visual comparison: ") + Directory);
        return Finish();
    }
private:
    bool Start()
    {
        bStarted = true; Deadline = FPlatformTime::Seconds() + 90;
        if (!Test.TestFalse(TEXT("Reference visual host does not load GlooPrint"), FModuleManager::Get().IsModuleLoaded(TEXT("GlooPrint")))) { return Finish(); }
        auto& Settings = UBASettings::GetMutable(); OriginalAuto = Settings.bGloballyDisableAutoFormatting; OriginalKnots = Settings.bCreateKnotNodes;
        bRestoreSettings = true; Settings.bGloballyDisableAutoFormatting = true; Settings.bCreateKnotNodes = false;
        OriginalCursor = FSlateApplication::Get().GetCursorPos(); FSlateApplication::Get().SetCursorPos(FVector2D::ZeroVector);
        Notes = FString::Printf(TEXT("Asset=%s\nBlueprintAssist=4.4.7, isolated UE5.8 compatibility build\nNative wires in all views; GlooPrint module absent\nReference node padding=%d,%d; parameter padding=%d,%d; comment padding=%d,%d; apply comment padding=%d; formatting style=%d; parameter style=%d\nAutomatic formatting=off; knot creation=off\nGlooPrint positions replayed from accepted source074845732881f466da8223dd76fc9d003d0388359b1a2eb9101513882eb18bce; H96,V48,comment32\nNo timing comparison\n"),
            *Asset, Settings.BlueprintFormatterSettings.Padding.X, Settings.BlueprintFormatterSettings.Padding.Y,
            Settings.BlueprintParameterPadding.X, Settings.BlueprintParameterPadding.Y, Settings.CommentNodePadding.X, Settings.CommentNodePadding.Y,
            int32(Settings.bApplyCommentPadding), int32(Settings.FormattingStyle), int32(Settings.ParameterStyle));
        if (bElectronicNodes)
        {
            if (!Test.TestTrue(TEXT("Electronic Nodes is loaded in its isolated host"),FModuleManager::Get().IsModuleLoaded(TEXT("ElectronicNodes")))) { return Finish(); }
            UClass* Class=FindObject<UClass>(nullptr,TEXT("/Script/ElectronicNodes.ElectronicNodesSettings"));
            if (!Test.TestNotNull(TEXT("Reference exposes its native settings class"),Class)) { return Finish(); }
            ElectronicSettings=Class->GetDefaultObject();
            MasterProperty=FindFProperty<FBoolProperty>(Class,TEXT("MasterActivate"));
            RibbonProperty=FindFProperty<FBoolProperty>(Class,TEXT("ActivateRibbon"));
            if (!Test.TestTrue(TEXT("Reference exposes activation and ribbon settings"),MasterProperty && RibbonProperty)) { return Finish(); }
            OriginalMaster=MasterProperty->GetPropertyValue_InContainer(ElectronicSettings.Get());
            OriginalRibbon=RibbonProperty->GetPropertyValue_InContainer(ElectronicSettings.Get()); bRestoreElectronic=true;
            MasterProperty->SetPropertyValue_InContainer(ElectronicSettings.Get(),false);
            Notes=TEXT("ElectronicNodes=3.16, isolated descriptor5.8; original source unchanged\nNo formatter command or timing sample; GlooPrint module absent\nAuthored ribbon off/on, then accepted GlooPrint positions with ribbon on\nGlooPrint placement source074845732881f466da8223dd76fc9d003d0388359b1a2eb9101513882eb18bce; H96,V48,comment32\n");
            for (TFieldIterator<FProperty> Property(Class);Property;++Property) if (Property->HasAnyPropertyFlags(CPF_Config))
            {
                FString Value; Property->ExportText_InContainer(0,Value,ElectronicSettings.Get(),nullptr,ElectronicSettings.Get(),PPF_None);
                Notes+=Property->GetName()+TEXT("=")+Value+TEXT("\n");
            }
            Names[0]=TEXT("AuthoredElectronicNodesDefault"); Names[1]=TEXT("AuthoredElectronicNodesRibbon"); Names[2]=TEXT("GlooPrintLayoutElectronicNodesRibbon");
        }
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ReferenceVisual") / (Asset + TEXT("-") + Id));
        IFileManager::Get().MakeDirectory(*Directory, true); Mount = TEXT("/GlooPrintReferenceVisual_") + Id + TEXT("/");
        SourcePath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("../Templates/TP_AEC_ArchvisBP/Content/ArchvisProject/Blueprints") / (Asset + TEXT(".uasset")));
        CopyPath = Directory / (Asset + TEXT(".uasset"));
        if (!Test.TestTrue(TEXT("Read installed authored Blueprint"), FFileHelper::LoadFileToArray(SourceBytes, *SourcePath)) ||
            !Test.TestTrue(TEXT("Copy authored Blueprint into private fixture"), FFileHelper::SaveArrayToFile(SourceBytes, *CopyPath))) { return Finish(); }
        FPackageName::RegisterMountPoint(Mount, Directory + TEXT("/")); bMounted = true;
        const FString Name = Mount + Asset; FLinkerInstancingContext Context(true);
        Context.AddPackageMapping(FName(*(TEXT("/Game/ArchvisProject/Blueprints/") + Asset)), FName(*Name));
        Package.Reset(LoadPackage(nullptr, *Name, LOAD_None, nullptr, &Context));
        if (!Test.TestNotNull(TEXT("Load private authored package"), Package.Get())) { return Finish(); }
        Blueprint.Reset(Cast<UBlueprint>(Package->FindAssetInPackage()));
        if (!Test.TestNotNull(TEXT("Load real authored Blueprint"), Blueprint.Get()) || !Compile()) { return Finish(); }
        Graph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint.Get());
        if (!Test.TestNotNull(TEXT("Find authored construction graph"), Graph)) { return Finish(); }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->IsA<UK2Node_FunctionEntry>()) { Entry = Node; }
            for (const TCHAR* Field : {TEXT("NodePosX"), TEXT("NodePosY")}) { LayoutFields.Add(Node->GetName() + TEXT(".") + Field); }
            if (Node->IsA<UEdGraphNode_Comment>()) for (const TCHAR* Field : {TEXT("NodeWidth"), TEXT("NodeHeight")}) { LayoutFields.Add(Node->GetName() + TEXT(".") + Field); }
        }
        if (!Test.TestNotNull(TEXT("Authored graph has its entry anchor"), Entry)) { return Finish(); }
        auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!Test.TestTrue(TEXT("Open authored Blueprint editor"), Editors->OpenEditorForAsset(Blueprint.Get()))) { return Finish(); }
        const auto Instance = Editors->FindEditorForAsset(Blueprint.Get(), true);
        if (!Test.TestTrue(TEXT("Authored asset opens in BlueprintEditor"), Instance && Instance->GetEditorName() == TEXT("BlueprintEditor"))) { return Finish(); }
        Editor = static_cast<FBlueprintEditor*>(Instance)->OpenGraphAndBringToFront(Graph);
        if (!Test.TestTrue(TEXT("Authored graph editor is available"), Editor.IsValid())) { return Finish(); }
        Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false); return false;
    }
    bool CaptureTerminals(TMap<FString,FString>& Values, TArray<FString>& Links)
    {
        Values = CaptureValues(*Graph); TSet<FString> Effective;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->IsA<UK2Node_Knot>())
            {
                const FString Prefix = Node->GetName()+TEXT(".");
                for (auto It=Values.CreateIterator();It;++It) { if (It.Key().StartsWith(Prefix)) { It.RemoveCurrent(); } }
                continue;
            }
            for (int32 Index=0;Index<Node->Pins.Num();++Index)
            {
                UEdGraphPin* Pin=Node->Pins[Index];
                FString& Export=Values.FindChecked(FString::Printf(TEXT("%s.Pin%d"),*Node->GetName(),Index));
                int32 Depth=0,Start=1; bool bQuoted=false,bEscaped=false,bRemoved=false;
                for (int32 I=0;I<Export.Len();++I)
                {
                    const TCHAR C=Export[I];
                    if (bQuoted)
                    {
                        if (bEscaped) { bEscaped=false; }
                        else if (C==TCHAR('\\')) { bEscaped=true; }
                        else if (C==TCHAR('"')) { bQuoted=false; }
                        continue;
                    }
                    if (C==TCHAR('"')) { bQuoted=true; }
                    else if (C==TCHAR('(')) { ++Depth; }
                    else if (C==TCHAR(')')) { --Depth; }
                    else if (C==TCHAR(',') && Depth==1)
                    {
                        if (Export.Mid(Start,I-Start).StartsWith(TEXT("LinkedTo="))) { Export.RemoveAt(Start,I-Start+1); bRemoved=true; break; }
                        Start=I+1;
                    }
                }
                if (!Test.TestEqual(TEXT("Native link field is removed exactly when present"),bRemoved,!Pin->LinkedTo.IsEmpty())) { return false; }
                if (Pin->Direction!=EGPD_Output) { continue; }
                TArray<UEdGraphPin*> Pending=Pin->LinkedTo; TSet<UEdGraphNode*> Seen;
                while (!Pending.IsEmpty())
                {
                    UEdGraphPin* Target=Pending.Pop(EAllowShrinking::No);
                    if (auto* Knot=Cast<UK2Node_Knot>(Target->GetOwningNode()))
                    {
                        if (!Seen.Contains(Knot)) { Seen.Add(Knot);Pending.Append(Knot->GetOutputPin()->LinkedTo); }
                    }
                    else
                    {
                        Effective.Add(Node->NodeGuid.ToString()+TEXT("/")+Pin->PinId.ToString()+TEXT("->")+
                            Target->GetOwningNode()->NodeGuid.ToString()+TEXT("/")+Target->PinId.ToString());
                    }
                }
            }
        }
        Links=Effective.Array();Links.Sort();return true;
    }
    bool Compile()
    {
        FCompilerResultsLog Result; FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Result);
        return Test.TestEqual(TEXT("Reference authored Blueprint compiles"), Result.NumErrors, 0);
    }
    void PrimeTooltips() { for (UEdGraphNode* Node : Graph->Nodes) for (UEdGraphPin* Pin : Node->Pins) { FString Value; Node->GetPinHoverText(*Pin, Value); } }
    bool Compare(const TMap<FString,FString>& A, const TMap<FString,FString>& B, bool bLayout, const TCHAR* StageName)
    {
        if (!Test.TestEqual(FString(StageName) + TEXT(" preserves property count"), A.Num(), B.Num())) { return false; }
        for (const auto& Pair : A)
        {
            if (bLayout && LayoutFields.Contains(Pair.Key)) { continue; }
            const FString* Value = B.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) { Test.AddError(FString(StageName) + TEXT(" changed ") + Pair.Key); return false; }
        }
        return true;
    }
    void ReadPlacement(int32 Index)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto Widget = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Node->NodeGuid);
            Placements[Index].Add(Node->GetName(), {FIntPoint(Node->NodePosX, Node->NodePosY), FVector2f(Widget->GetDesiredSize())});
        }
    }
    bool LoadGlooPrintPlacement()
    {
        const TCHAR* Id = Asset == TEXT("BP_Splinemesh") ? TEXT("E1CAE691C044FF9EE8C55C88E1E91398") : TEXT("67B42EE5B640D0A10C087BBBF645365A");
        const FString Accepted = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../Automation-CommentConsumerCapture/TemplateBlueprint") / Id);
        TArray<uint8> AcceptedAsset; FString Rows;
        if (!Test.TestTrue(TEXT("Replay uses the identical accepted authored asset bytes"), FFileHelper::LoadFileToArray(AcceptedAsset, *(Accepted / (Asset + TEXT(".uasset")))) && AcceptedAsset == SourceBytes) ||
            !Test.TestTrue(TEXT("Read accepted GlooPrint positions"), FFileHelper::LoadFileToString(Rows, *(Accepted / TEXT("FormattedRounded-nodes.tsv"))))) { return false; }
        Test.TestTrue(TEXT("Retain accepted GlooPrint position input"), FFileHelper::SaveStringToFile(Rows, *(Directory / TEXT("GlooPrint-input-nodes.tsv"))));
        Notes += TEXT("GlooPrintInput=") + Accepted + TEXT("/FormattedRounded-nodes.tsv\n");
        TArray<FString> Lines; Rows.ParseIntoArrayLines(Lines);
        for (int32 I = 1; I < Lines.Num(); ++I)
        {
            TArray<FString> Fields; Lines[I].ParseIntoArray(Fields, TEXT("\t"), false);
            if (!Test.TestEqual(TEXT("Position row has the accepted schema"), Fields.Num(), 8)) { return false; }
            Placements[2].Add(Fields[1], {FIntPoint(FCString::Atoi(*Fields[3]), FCString::Atoi(*Fields[4])),
                FVector2f(FCString::Atof(*Fields[5]), FCString::Atof(*Fields[6]))});
        }
        if (!Test.TestEqual(TEXT("Replay retains every original node"), Placements[2].Num(), Graph->Nodes.Num())) { return false; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto* P = Placements[2].Find(Node->GetName());
            if (!Test.TestTrue(TEXT("Replay identifies each original node and finite positive size"), P &&
                FMath::IsFinite(P->Size.X) && FMath::IsFinite(P->Size.Y) && P->Size.X > 0 && P->Size.Y > 0)) { return false; }
            for (int32 I : {0,1}) if (!Node->IsA<UEdGraphNode_Comment>()) { if (auto* Existing=Placements[I].Find(Node->GetName())) { Existing->Size = P->Size; } }
        }
        return true;
    }
    bool BeginCaptures()
    {
        if (!LoadGlooPrintPlacement()) { return false; }
        Handler.Reset(); GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Blueprint.Get()); Editor.Reset();
        for (const auto& State : Placements) for (const auto& Pair : State)
        {
            Bounds += FVector2f(Pair.Value.Position); Bounds += FVector2f(Pair.Value.Position) + Pair.Value.Size;
        }
        Phase=5;Stage=0;return OpenCapture();
    }
    void RestoreLayout(const TMap<FString,FString>& Values)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const FString Prefix=Node->GetName()+TEXT(".");
            Node->SetPosition(FVector2f(FCString::Atoi(*Values.FindChecked(Prefix+TEXT("NodePosX"))),
                FCString::Atoi(*Values.FindChecked(Prefix+TEXT("NodePosY")))));
            if (auto* Comment=Cast<UEdGraphNode_Comment>(Node))
            {
                Comment->NodeWidth=FCString::Atoi(*Values.FindChecked(Prefix+TEXT("NodeWidth")));
                Comment->NodeHeight=FCString::Atoi(*Values.FindChecked(Prefix+TEXT("NodeHeight")));
            }
        }
    }
    bool OpenCapture()
    {
        if (bElectronicNodes)
        {
            MasterProperty->SetPropertyValue_InContainer(ElectronicSettings.Get(),true);
            RibbonProperty->SetPropertyValue_InContainer(ElectronicSettings.Get(),Stage!=0);
        }
        else if (Stage==1 && !bReferenceApplied)
        {
            RestoreLayout(Before);
            if (!Compare(Before,CaptureValues(*Graph),false,TEXT("Original state before replay redo")) ||
                !Test.TestTrue(TEXT("Restore actual reference topology for its view"),GEditor->RedoTransaction())) { return false; }
            bReferenceApplied=true;
            if (!Compare(After,CaptureValues(*Graph),false,TEXT("Reference replay redo"))) { return false; }
        }
        else if (Stage!=1 && bReferenceApplied)
        {
            RestoreLayout(After);
            if (!Compare(After,CaptureValues(*Graph),false,TEXT("Reference state before replay undo")) ||
                !Test.TestTrue(TEXT("Restore authored topology for its view"),GEditor->UndoTransaction())) { return false; }
            bReferenceApplied=false;
            if (!Compare(Before,CaptureValues(*Graph),false,TEXT("Reference replay undo"))) { return false; }
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto& P = Placements[Stage].FindChecked(Node->GetName()); Node->SetPosition(FVector2f(P.Position));
            if (auto* Comment = Cast<UEdGraphNode_Comment>(Node)) { Comment->NodeWidth = FMath::RoundToInt(P.Size.X); Comment->NodeHeight = FMath::RoundToInt(P.Size.Y); }
        }
        Graph->NotifyGraphChanged();
        Editor = SNew(SGraphEditor).GraphToEdit(Graph).IsEditable(false);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("Native layout comparison: ") + Asset)).ClientSize(FVector2f(1550,1000)).FocusWhenFirstShown(false)[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        const FVector2f Span = Bounds.GetSize() + FVector2f(160); const auto& Levels = Editor->GetGraphPanel()->GetZoomLevels();
        const float Fit = FMath::Min(0.25f, FMath::Min(1500.f / Span.X, 940.f / Span.Y)); CaptureZoom = Levels->GetZoomAmount(0);
        for (int32 I = 0; I < Levels->GetNumZoomLevels(); ++I) { const float Amount = Levels->GetZoomAmount(I); if (Amount <= Fit) { CaptureZoom = FMath::Max(CaptureZoom, Amount); } }
        CaptureView = Bounds.Min - FVector2f(80); Editor->SetViewLocation(CaptureView, CaptureZoom); Frames = 0; return true;
    }
    bool CheckAndExport()
    {
        if (!Compare(Stage==1 ? After : Before, CaptureValues(*Graph), true, TEXT("Visual replay"))) { return false; }
        auto* Panel = Editor->GetGraphPanel(); FArrangedChildren Nodes(EVisibility::Visible); TMap<TSharedRef<SWidget>,FArrangedWidget> Pins;
        FString Rows = TEXT("object\tx\ty\twidth\theight\n");
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid); const auto& G = Widget->GetCachedGeometry();
            const FVector2f Expected = Panel->GetCachedGeometry().LocalToAbsolute((FVector2f(Node->GetPosition()) - CaptureView) * CaptureZoom);
            if (!Test.TestTrue(TEXT("Comparison uses freshly painted node positions"), G.GetAbsolutePosition().Equals(Expected, 0.1f))) { return false; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(),G));
            Rows += FString::Printf(TEXT("%s\t%d\t%d\t%.3f\t%.3f\n"),*Node->GetName(),Node->NodePosX,Node->NodePosY,double(G.GetLocalSize().X),double(G.GetLocalSize().Y));
            TArray<TSharedRef<SWidget>> NativePins; Widget->GetPins(NativePins);
            for (const auto& Pin : NativePins) { Pins.Add(Pin,FArrangedWidget(Pin,Pin->GetCachedGeometry())); }
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const FVector2f Origin = Nodes[0].Geometry.GetAbsolutePosition() - FVector2f(Graph->Nodes[0]->GetPosition()) * Scale;
        FSlateWindowElementList Elements(Window);
        TUniquePtr<FConnectionDrawingPolicy> Policy(bElectronicNodes
            ? FNodeFactory::CreateConnectionPolicy(Graph->GetSchema(),0,1,Scale,FSlateRect(-100000,-100000,100000,100000),Elements,Graph)
            : new FKismetConnectionDrawingPolicy(0,1,Scale,FSlateRect(-100000,-100000,100000,100000),Elements,Graph));
        if (!Test.TestNotNull(TEXT("Reference drawing policy is available"),Policy.Get())) { return false; }
        Policy->SetAbsoluteMousePosition(FVector2f(-100000)); Policy->Draw(Pins,Nodes);
        const auto& Curves = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
        int32 Links = 0; for (const UEdGraphNode* Node : Graph->Nodes) for (const auto* Pin : Node->Pins) if (Pin->Direction == EGPD_Output) { Links += Pin->LinkedTo.Num(); }
        if (bElectronicNodes)
        {
            if (!Test.TestTrue(TEXT("Reference emits visible wire geometry"),!Curves.IsEmpty())) { return false; }
        }
        else if (!Test.TestEqual(TEXT("Native comparison draws every current connection"),Curves.Num(),Links)) { return false; }
        for (const auto& C : Curves) for (const FVector2f P : {C.P0,C.P1,C.P2,C.P3})
        {
            if (!bCapturePass) { Bounds += (P-Origin)/Scale; }
            else
            {
                const FVector2f Local = Panel->GetCachedGeometry().AbsoluteToLocal(P); const FVector2f Size = Panel->GetCachedGeometry().GetLocalSize();
                if (!Test.TestTrue(TEXT("Complete drawn curve hull fits the comparison viewport"),Local.X>=0 && Local.Y>=0 && Local.X<=Size.X && Local.Y<=Size.Y)) { return false; }
            }
        }
        if (bCapturePass)
        {
            const TCHAR* Name = Names[Stage];
            Test.TestTrue(TEXT("Save actual comparison node geometry"),FFileHelper::SaveStringToFile(Rows,*(Directory/(FString(Name)+TEXT("-nodes.tsv")))));
            Notes += FString::Printf(TEXT("%s: zoom=%.3f, draw scale=%.3f, view=%.3f,%.3f, nodes=%d, links=%d, curve pieces=%d\n"),Name,double(CaptureZoom),double(Scale),double(CaptureView.X),double(CaptureView.Y),Nodes.Num(),Links,Curves.Num());
        }
        return true;
    }
    void Capture()
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture fresh complete reference view"),FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(),Pixels,Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,Png);
        Test.TestTrue(TEXT("Save matched reference view"),FFileHelper::SaveArrayToFile(Png,*(Directory/(FString(Names[Stage])+TEXT(".png")))));
    }
    void CloseCapture() { if (Window) { Window->RequestDestroyWindow(); } Window.Reset(); Editor.Reset(); }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        if (Handler) { Handler->OnPostFormatting.Remove(CompleteHandle); Handler->CancelActiveFormatting(); Handler.Reset(); }
        CloseCapture(); if (Package) { Package->SetDirtyFlag(false); }
        if (Blueprint && GEditor) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Blueprint.Get()); }
        if (bMounted) { FPackageName::UnRegisterMountPoint(Mount,Directory+TEXT("/")); bMounted=false; }
        if (bRestoreElectronic && ElectronicSettings.IsValid())
        {
            MasterProperty->SetPropertyValue_InContainer(ElectronicSettings.Get(),OriginalMaster);
            RibbonProperty->SetPropertyValue_InContainer(ElectronicSettings.Get(),OriginalRibbon); bRestoreElectronic=false;
        }
        if (bRestoreSettings)
        {
            auto& Settings=UBASettings::GetMutable();Settings.bGloballyDisableAutoFormatting=OriginalAuto;Settings.bCreateKnotNodes=OriginalKnots;
            FSlateApplication::Get().SetCursorPos(OriginalCursor);bRestoreSettings=false;
        }
    }
    FAutomationTestBase& Test;
    FString Asset,Directory,Mount,SourcePath,CopyPath,Notes;
    TWeakObjectPtr<UObject> ElectronicSettings;
    FBoolProperty* MasterProperty=nullptr; FBoolProperty* RibbonProperty=nullptr;
    bool bElectronicNodes=false,bRestoreElectronic=false,OriginalMaster=false,OriginalRibbon=false;
    TArray<uint8> SourceBytes;
    TStrongObjectPtr<UPackage> Package;
    TStrongObjectPtr<UBlueprint> Blueprint;
    UEdGraph* Graph=nullptr; UEdGraphNode* Entry=nullptr;
    TSharedPtr<SGraphEditor> Editor; TSharedPtr<SWindow> Window; TSharedPtr<FBAGraphHandler> Handler; FDelegateHandle CompleteHandle;
    TMap<FString,FString> Before,After,BeforeTerminals; TArray<FString> BeforeLinks;
    TMap<FString,FName> OriginalClasses; TSet<FString> OriginalReroutes; TSet<FString> LayoutFields; TMap<FString,FPlacement> Placements[3];
    FBox2f Bounds{ForceInit}; FVector2f CaptureView; FVector2D OriginalCursor;
    float CaptureZoom=0; double Deadline=0; int32 Phase=0,Frames=0,Stage=0,Queue=0;
    bool bReferenceApplied=false,bStarted=false,bCompleted=false,bMounted=false,bRestoreSettings=false,bCapturePass=false,OriginalAuto=false,OriginalKnots=false;
    const TCHAR* Names[3]={TEXT("AuthoredNative"),TEXT("BlueprintAssistNative"),TEXT("GlooPrintLayoutNative")};
};
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBlueprintAssistVisualTest, "GlooPrint.Reference.BlueprintAssist.AuthoredVisual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FBlueprintAssistVisualTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    for (const TCHAR* Asset : {TEXT("BP_Splinemesh"),TEXT("BP_SpawnMeshAlongSpline")}) { Names.Add(Asset);Commands.Add(Asset); }
}
bool FBlueprintAssistVisualTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FReferenceVisual(*this,Parameters));return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FElectronicNodesVisualTest, "GlooPrint.Reference.ElectronicNodes.AuthoredVisual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FElectronicNodesVisualTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    for (const TCHAR* Asset : {TEXT("BP_Splinemesh"),TEXT("BP_SpawnMeshAlongSpline")}) { Names.Add(Asset);Commands.Add(Asset); }
}
bool FElectronicNodesVisualTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FReferenceVisual(*this,Parameters,true));return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBlueprintAssistBaselineTest, "GlooPrint.Reference.BlueprintAssist.PinHeavy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FBlueprintAssistBaselineTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int32 Count : {100, 1000}) { const FString Size = LexToString(Count); Names.Add(Size); Commands.Add(Size); }
}
bool FBlueprintAssistBaselineTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FReferenceFormat(*this, FCString::Atoi(*Parameters))); return true;
}
}
#endif
