// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintNativeBenchmarkFixture.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintEditor.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "SGraphPanel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
namespace
{
class FFormatCompletion final : public FOutputDevice
{
public:
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (!IsInGameThread() || !bArmed || Category != FName(TEXT("LogGlooPrintEditor"))) { return; }
        static constexpr TCHAR Prefix[] = TEXT("Formatted graph: ");
        if (FCString::Strncmp(Message, Prefix, UE_ARRAY_COUNT(Prefix) - 1) != 0) { LastMessage = Message; return; }
        CompletedAt = FPlatformTime::Seconds(); CompletedFrame = GFrameCounter;
        if (const auto Cache = Measurements.Pin()) { Hits = Cache->GetHits(); Misses = Cache->GetMisses(); }
        if (const auto Owner = GraphEditor.Pin())
        {
            Owner->GetViewLocation(View, Zoom);
            Selection.Reset(); for (UObject* Node : Owner->GetSelectedNodes()) { Selection.Add(Node); }
        }
        Changed = FCString::Atoi(Message + UE_ARRAY_COUNT(Prefix) - 1); bArmed = false;
        TRACE_BOOKMARK(TEXT("GlooPrintNativeF complete %s changed=%d"), *Label, Changed);
    }
    void Arm(FString InLabel, TWeakPtr<FMeasurementCache> InMeasurements)
    {
        Label = MoveTemp(InLabel); Measurements = InMeasurements; LastMessage.Reset();
        Changed = -1; Hits = -1; Misses = -1; CompletedAt = 0; Selection.Reset(); bArmed = true;
    }
    FString Label, LastMessage;
    TWeakPtr<FMeasurementCache> Measurements;
    TWeakPtr<SGraphEditor> GraphEditor;
    TArray<TWeakObjectPtr<UObject>> Selection;
    FVector2f View;
    float Zoom = 0;
    double CompletedAt = 0;
    uint64 CompletedFrame = 0;
    int32 Changed = -1, Hits = -1, Misses = -1;
    bool bArmed = false;
};
}

class FNativeFormatBenchmark final : public IAutomationLatentCommand
{
public:
    FNativeFormatBenchmark(FAutomationTestBase& InTest, int32 InCount, FString InFamily)
        : Test(InTest), Count(InCount), Family(MoveTemp(InFamily)) {}
    virtual ~FNativeFormatBenchmark() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            const auto* Panel = Editor->GetGraphPanel();
            const auto Routes = Panel->GetMetaData<FRouteCache>();
            const auto Cache = Panel->GetMetaData<FMeasurementCache>();
            Test.AddError(FString::Printf(TEXT("Native F benchmark exceeded its 180-second stage deadline: phase=%d, frames=%d, measurement entries=%d, route builds=%d, pending=%d. Last command message: %s"),
                Phase, Frames, Cache ? Cache->GetEntryCount() : -1, Routes ? Routes->GetBuildCount() : -1,
                Routes && Routes->HasPendingRouting(), *Completion.LastMessage));
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        const auto Routes = Panel->GetMetaData<FRouteCache>();
        if (Phase == 1 || Phase == 3)
        {
            if (!Completion.CompletedAt || !Routes || !Routes->IsReady()) { return false; }
            const double RoutesObservedMs = (FPlatformTime::Seconds() - StartedAt) * 1000;
            const double CommandMs = (FMath::Max(Completion.CompletedAt, DispatchEndedAt) - StartedAt) * 1000;
            Csv += FString::Printf(TEXT("%s,%d,%d,%d,%d,%s,%d,%.6f,%.6f,%.6f,%llu,%d,%d,%d,%d,%d\n"), *Family, Count, Pins, Count - 1, Sample,
                Phase == 1 ? TEXT("Format") : TEXT("Repeat"), CacheEntriesBefore, DispatchMs, CommandMs, RoutesObservedMs,
                Completion.CompletedFrame - StartedFrame, Completion.Changed, Routes->GetRoutes().FallbackCount, Completion.Hits, Completion.Misses, bRoutesReadyBefore);
            if (Sample >= 0) { (Phase == 1 ? FormatTimes : RepeatTimes).Add(CommandMs); }
            Test.TestEqual(TEXT("Completed F retains every original connection"), Routes->GetRoutes().Wires.Num(), Count - 1);
            Test.TestEqual(TEXT("Completed fixture has no native routing fallback"), Routes->GetRoutes().FallbackCount, 0);
            if (bKeepMeasurementCache && CacheEntriesBefore == Count)
            {
                Test.TestEqual(TEXT("F reuses every node warmed by automatic routing"), Completion.Hits, Count);
                Test.TestEqual(TEXT("Warmed F constructs no duplicate native node widgets"), Completion.Misses, 0);
            }
            CheckContext();
            if (Phase == 1)
            {
                Test.TestEqual(TEXT("Post-format capture reuses the validated route plan"), Routes->GetReusedPlanCount(), ReusedPlansBefore + 1);
                Test.TestTrue(TEXT("Actual F changes this unformatted graph"), Completion.Changed > 0);
                Formatted = SerializeTransactionValues(*Fixture->Graph);
                Test.TestTrue(TEXT("Successful format changes graph values"), Formatted != Before);
                CheckProperties();
                Test.TestEqual(TEXT("Whole F creates exactly one undo transaction"), GEditor->Trans->GetQueueLength(), AppliedQueue + 1);
                Test.TestTrue(TEXT("A real format dirties its own asset"), Package->IsDirty());
                Test.TestTrue(TEXT("Whole format undoes"), GEditor->UndoTransaction());
                Test.TestTrue(TEXT("Undo restores the exact original graph"), Before == SerializeTransactionValues(*Fixture->Graph));
                Test.TestTrue(TEXT("Whole format redoes"), GEditor->RedoTransaction());
                Test.TestTrue(TEXT("Redo restores the exact formatted graph"), Formatted == SerializeTransactionValues(*Fixture->Graph));
                if (Test.HasAnyErrors()) { return Finish(); }
                Phase = 2; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
            }
            Test.TestEqual(TEXT("Completed repeated F reports no changed nodes"), Completion.Changed, 0);
            Test.TestTrue(TEXT("Completed repeated F preserves exact graph values"), Formatted == SerializeTransactionValues(*Fixture->Graph));
            Test.TestEqual(TEXT("Completed repeated F adds no undo entry"), GEditor->Trans->GetQueueLength(), NoOpQueue);
            Test.TestEqual(TEXT("Completed repeated F preserves redo history"), GEditor->Trans->GetUndoCount(), NoOpUndo);
            Test.TestFalse(TEXT("Completed repeated F leaves the clean asset clean"), Package->IsDirty());
            if (Test.HasAnyErrors()) { return Finish(); }
            if (++Sample >= SamplesWanted) { Summarize(); Capture(); return Finish(); }
            Test.TestTrue(TEXT("Restore the same starting layout for the next cold sample"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Every cold sample starts from the exact same graph"), Before == SerializeTransactionValues(*Fixture->Graph));
            Phase = 0; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
        }
        const auto Measurements = Panel->GetMetaData<FMeasurementCache>();
        const bool bWarmGeometry = bStartWithWarmGeometry && Phase == 0 && Measurements && Measurements->GetEntryCount() == Count;
        if (++Frames < 12 || !Routes || (!Routes->IsReady() && !bWarmGeometry)) { return false; }
        if (Phase == 0)
        {
            if (Before.IsEmpty())
            {
                Before = SerializeTransactionValues(*Fixture->Graph); Properties = DescribeNodes(*Fixture->Graph);
                Anchor = FIntPoint(Entry->NodePosX, Entry->NodePosY); Editor->GetViewLocation(View, Zoom);
            }
            if (!bKeepMeasurementCache)
            {
                if (const auto Cache = Panel->GetMetaData<FMeasurementCache>()) { Cache->Invalidate(); }
            }
            AppliedQueue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            if (bUseAssetEditor && Sample >= 0) { Editor->SetNodeSelection(Entry, true); }
            BeginRequest(false); Phase = 1; return false;
        }
        if (bUseAssetEditor)
        {
            Test.TestEqual(TEXT("Native Blueprint undo/redo cleared selection before repeated F"), Editor->GetSelectedNodes().Num(), 0);
        }
        NoOpQueue = GEditor->Trans->GetQueueLength(); NoOpUndo = GEditor->Trans->GetUndoCount();
        BeginRequest(true); Phase = 3; return false;
    }
private:
    bool Start()
    {
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); OriginalSettings = Settings->GetLayoutSettings();
        OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
        auto& Slate = FSlateApplication::Get(); OriginalCursor = Slate.GetCursorPos();
        Settings->bFormattingEnabled = true; Settings->WireStyle = EGlooPrintWireStyle::Rounded90;
        Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32; Settings->NotifyChanged();
        FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted); SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100);
        if (FParse::Param(FCommandLine::Get(), TEXT("GlooPrintBenchmarkWarmupOnly"))) { SamplesWanted = 0; }
        bKeepMeasurementCache = FParse::Param(FCommandLine::Get(), TEXT("GlooPrintKeepMeasurementCache"));
        bUseAssetEditor = FParse::Param(FCommandLine::Get(), TEXT("GlooPrintBenchmarkAssetEditor"));
        bStartWithWarmGeometry = FParse::Param(FCommandLine::Get(), TEXT("GlooPrintBenchmarkStartWithWarmGeometry"));
        if (bStartWithWarmGeometry && !bKeepMeasurementCache)
        {
            Test.AddError(TEXT("Warm-geometry start requires GlooPrintKeepMeasurementCache.")); return Finish();
        }
        Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks"); IFileManager::Get().MakeDirectory(*Directory, true);
        Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintNativeF_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
        Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false, Package.Get());
        if (Family == TEXT("FanOut"))
        {
            if (!PopulateNativeFan(Test, *Fixture, Count, Entry, Pins)) { return Finish(); }
        }
        else
        {
            UK2Node_ExecutionSequence* ChainEntry = nullptr;
            if (!PopulateNativeChain(Test, *Fixture, Count, Family == TEXT("PinHeavy") ? 64 : 2, ChainEntry, Pins)) { return Finish(); }
            Entry = ChainEntry;
        }
        if (bUseAssetEditor)
        {
            auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            if (!Test.TestTrue(TEXT("Open benchmark Blueprint editor"), Editors->OpenEditorForAsset(Fixture->Blueprint.Get()))) { return Finish(); }
            auto* Instance = Editors->FindEditorForAsset(Fixture->Blueprint.Get(), true);
            if (!Test.TestTrue(TEXT("Benchmark uses BlueprintEditor"), Instance && Instance->GetEditorName() == TEXT("BlueprintEditor"))) { return Finish(); }
            Editor = static_cast<FBlueprintEditor*>(Instance)->OpenGraphAndBringToFront(Fixture->Graph);
            if (!Test.TestTrue(TEXT("Blueprint benchmark graph is available"), Editor.IsValid())) { return Finish(); }
            Window = Slate.FindWidgetWindow(Editor.ToSharedRef());
            if (!Test.TestTrue(TEXT("Blueprint benchmark has a native window"), Window.IsValid())) { return Finish(); }
        }
        else
        {
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(FString::Printf(TEXT("GlooPrint native F: %d %s"), Count, *Family)))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
        }
        Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false); Slate.SetCursorPos(FVector2D::ZeroVector);
        Completion.GraphEditor = Editor;
        GLog->AddOutputDevice(&Completion); bListening = true; Deadline = FPlatformTime::Seconds() + 180; return false;
    }
    void BeginRequest(bool bRepeat)
    {
        auto* Panel = Editor->GetGraphPanel(); const auto Cache = Panel->GetMetaData<FMeasurementCache>();
        const auto Routes = Panel->GetMetaData<FRouteCache>();
        ReusedPlansBefore = Routes ? Routes->GetReusedPlanCount() : 0;
        bRoutesReadyBefore = Routes && Routes->IsReady();
        CacheEntriesBefore = Cache ? Cache->GetEntryCount() : 0;
        if (!bRepeat && !bKeepMeasurementCache) { Test.TestEqual(TEXT("Measured format begins with cold native measurement cache"), CacheEntriesBefore, 0); }
        Package->SetDirtyFlag(false); FSlateApplication::Get().SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Completion.Arm(FString::Printf(TEXT("%d.%s.%d.%s"), Count, *Family, Sample, bRepeat ? TEXT("Repeat") : TEXT("Format")), Cache);
        RequestSelection.Reset(); for (UObject* Node : Editor->GetSelectedNodes()) { RequestSelection.Add(Node); }
        if (!bRepeat)
        {
            Test.TestTrue(TEXT("Independent format starts with its selected anchor"), RequestSelection.Num() == 1 && RequestSelection[0].Get() == Entry);
        }
        Editor->GetViewLocation(RequestView, RequestZoom);
        if (bUseAssetEditor)
        {
            const FVector2f Size = Panel->GetCachedGeometry().GetLocalSize();
            Test.AddInfo(FString::Printf(TEXT("Native F uses full Blueprint editor; graph panel %.1fx%.1f, zoom %.6f; rounded wires, spacing96/48, comment padding32."), Size.X, Size.Y, RequestZoom));
        }
        Test.TestTrue(*FString::Printf(TEXT("%s begins at the original camera: initial=(%.9g,%.9g) zoom=%.9g, before F=(%.9g,%.9g) zoom=%.9g"),
            *Completion.Label, View.X, View.Y, Zoom, RequestView.X, RequestView.Y, RequestZoom), RequestView == View && RequestZoom == Zoom);
        TRACE_BOOKMARK(TEXT("GlooPrintNativeF start %s cache=%d"), *Completion.Label, CacheEntriesBefore);
        StartedFrame = GFrameCounter; StartedAt = FPlatformTime::Seconds();
        bool bHandled = false;
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BenchmarkNativeFKey);
            bHandled = FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        }
        DispatchEndedAt = FPlatformTime::Seconds(); DispatchMs = (DispatchEndedAt - StartedAt) * 1000;
        Test.TestTrue(TEXT("Real native F key is handled"), bHandled);
        FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        Deadline = FPlatformTime::Seconds() + 180;
    }
    void CheckContext()
    {
        Test.TestEqual(TEXT("Whole F keeps its selected anchor fixed"), FIntPoint(Entry->NodePosX, Entry->NodePosY), Anchor);
        bool bSameSelection = Editor->GetSelectedNodes().Num() == RequestSelection.Num() && Completion.Selection.Num() == RequestSelection.Num();
        for (const auto& Node : RequestSelection)
        {
            bSameSelection &= Node.IsValid() && Editor->GetSelectedNodes().Contains(Node.Get()) && Completion.Selection.Contains(Node);
        }
        Test.TestTrue(TEXT("Whole F retains its exact input selection at completion and route readiness"), bSameSelection);
        FVector2f CurrentView; float CurrentZoom = 0; Editor->GetViewLocation(CurrentView, CurrentZoom);
        Test.TestTrue(*FString::Printf(TEXT("Whole F retains camera position and zoom (%s): initial=(%.9g,%.9g) zoom=%.9g, before F=(%.9g,%.9g) zoom=%.9g, command result=(%.9g,%.9g) zoom=%.9g, routes ready=(%.9g,%.9g) zoom=%.9g"),
            *Completion.Label, View.X, View.Y, Zoom, RequestView.X, RequestView.Y, RequestZoom,
            Completion.View.X, Completion.View.Y, Completion.Zoom, CurrentView.X, CurrentView.Y, CurrentZoom),
            CurrentView == View && CurrentZoom == Zoom && Completion.View == RequestView && Completion.Zoom == RequestZoom);
    }
    void CheckProperties()
    {
        const auto After = DescribeNodes(*Fixture->Graph);
        Test.TestEqual(TEXT("Formatting retains every native property and pin field"), After.Num(), Properties.Num());
        for (const auto& Pair : Properties)
        {
            if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
            const FString* Value = After.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) { Test.AddError(TEXT("Native F changed non-position property: ") + Pair.Key); break; }
        }
    }
    void Summarize()
    {
        if (SamplesWanted == 0)
        {
            Test.AddInfo(TEXT("Native F warmup-only diagnostic completed format, exact undo/redo and repeated no-op. Raw warmup timings are in the CSV; no measured sample or p95."));
            return;
        }
        for (auto* Times : {&FormatTimes, &RepeatTimes})
        {
            Times->Sort(); const bool bFormat = Times == &FormatTimes;
            if (SamplesWanted >= 20)
            {
                Test.AddInfo(FString::Printf(TEXT("Native %d %s %s: %d samples after one warmup; successful-result p95 %.3fms, range %.3f–%.3fms. Includes native capture/planning/apply and intervening editor frames; excludes later automatic route readiness."),
                    Count, *Family, bFormat ? (bKeepMeasurementCache ? TEXT("F with existing cache") : TEXT("cold F")) : TEXT("repeated F"), SamplesWanted, (*Times)[FMath::CeilToInt(Times->Num() * 0.95) - 1], (*Times)[0], Times->Last()));
            }
            else
            {
                Test.AddInfo(FString::Printf(TEXT("Native %d %s %s diagnostic: %d samples after one warmup; successful-result range %.3f–%.3fms. No p95 or responsiveness claim."),
                    Count, *Family, bFormat ? (bKeepMeasurementCache ? TEXT("F with existing cache") : TEXT("cold F")) : TEXT("repeated F"), SamplesWanted, (*Times)[0], Times->Last()));
            }
        }
    }
    void Capture()
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture actual completed native F graph"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        Test.TestTrue(TEXT("Save native F benchmark capture"), FFileHelper::SaveArrayToFile(Png, *(Directory / (Stem() + TEXT(".png")))));
    }
    FString Stem() const { return FString::Printf(TEXT("%d-NativeFormat-%s%s%s%s"), Count, *Family,
        bKeepMeasurementCache ? TEXT("-KeepCache") : TEXT(""), bUseAssetEditor ? TEXT("-AssetEditor") : TEXT(""),
        bStartWithWarmGeometry ? TEXT("-WarmGeometry") : TEXT("")); }
    bool Finish()
    {
        if (!Directory.IsEmpty()) { Test.TestTrue(TEXT("Save all native F samples"), FFileHelper::SaveStringToFile(Csv, *(Directory / (Stem() + TEXT(".csv"))))); }
        Restore(); return true;
    }
    void Restore()
    {
        if (bListening) { Completion.bArmed = false; GLog->RemoveOutputDevice(&Completion); bListening = false; }
        if (Package) { Package->SetDirtyFlag(false); }
        if (bUseAssetEditor && Fixture)
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Fixture->Blueprint.Get());
        }
        else if (Window) { Window->RequestDestroyWindow(); }
        Window.Reset(); Editor.Reset();
        if (bRestore)
        {
            bRestore = false; auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = OriginalStyle;
            Settings->bFormattingEnabled = bOriginalEnabled; Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalSettings.VerticalSpacing; Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->NotifyChanged();
            FSlateApplication::Get().SetCursorPos(OriginalCursor);
        }
    }
    FAutomationTestBase& Test;
    int32 Count;
    FString Family, Directory;
    FFormatCompletion Completion;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<FFixture> Fixture;
    UEdGraphNode* Entry = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TArray<uint8> Before, Formatted;
    TMap<FString, FString> Properties;
    TArray<TWeakObjectPtr<UObject>> RequestSelection;
    TArray<double> FormatTimes, RepeatTimes;
    FVector2D OriginalCursor;
    FVector2f View, RequestView;
    FIntPoint Anchor;
    float Zoom = 0, RequestZoom = 0;
    double Deadline = 0, StartedAt = 0, DispatchEndedAt = 0, DispatchMs = 0;
    uint64 StartedFrame = 0;
    int32 Phase = 0, Frames = 0, Sample = -1, SamplesWanted = 3, Pins = 0, AppliedQueue = 0, NoOpQueue = 0, NoOpUndo = 0, CacheEntriesBefore = 0;
    int32 ReusedPlansBefore = 0;
    bool bRestore = false, bOriginalEnabled = true, bListening = false, bKeepMeasurementCache = false, bUseAssetEditor = false;
    bool bStartWithWarmGeometry = false, bRoutesReadyBefore = false;
    FString Csv = TEXT("family,nodes,pins,links,sample,operation,measurement_entries_before,initial_dispatch_ms,command_result_ms,routes_observed_ready_ms,command_frames,changed_nodes,fallbacks,measurement_hits,measurement_misses,routes_ready_before\n");
};
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FNativeFormatPerformanceTest, "GlooPrint.Performance.NativeFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FNativeFormatPerformanceTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int32 Count : {100, 1000}) for (const TCHAR* Family : {TEXT("Ordinary"), TEXT("PinHeavy")})
    {
        const FString Name = FString::Printf(TEXT("%d.%s"), Count, Family); Names.Add(Name); Commands.Add(Name);
    }
    for (const TCHAR* Name : {TEXT("1000.FanOut"), TEXT("5000.FanOut")}) { Names.Add(Name); Commands.Add(Name); }
}
bool FNativeFormatPerformanceTest::RunTest(const FString& Parameters)
{
    FString Size, Family;
    if (!Parameters.Split(TEXT("."), &Size, &Family)) { AddError(TEXT("Expected native fixture size and family.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FNativeFormatBenchmark(*this, FCString::Atoi(*Size), Family)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeValidationPerformanceTest, "GlooPrint.Performance.NativeValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
bool FNativeValidationPerformanceTest::RunTest(const FString& Parameters)
{
    FFixture Fixture(UObject::StaticClass(), false);
    UK2Node_ExecutionSequence* Entry = nullptr;
    int32 Pins = 0;
    if (!PopulateNativeChain(*this, Fixture, 1000, 64, Entry, Pins)) { return false; }
    const auto Before = SerializeNodes(*Fixture.Graph);
    TArray<double> Times; Times.Reserve(30);
    FString Csv = TEXT("nodes,pins,sample,validation_ms\n"), Reason;
    for (int32 Sample = -3; Sample < 30; ++Sample)
    {
        const double Start = FPlatformTime::Seconds();
        const bool bValid = ValidateMeasurementGraph(Fixture.Graph, Reason);
        const double Ms = (FPlatformTime::Seconds() - Start) * 1000;
        if (!TestTrue(*Reason, bValid)) { return false; }
        Csv += FString::Printf(TEXT("1000,%d,%d,%.6f\n"), Pins, Sample, Ms);
        if (Sample >= 0) { Times.Add(Ms); }
    }
    TestTrue(TEXT("Repeated validation preserves every native node and pin value"), Before == SerializeNodes(*Fixture.Graph));
    Times.Sort();
    AddInfo(FString::Printf(TEXT("Native validation only, 1000 nodes/%d pins: 30 samples after 3 warmups, median %.3fms, p95 %.3fms, range %.3f–%.3fms. No F or responsiveness claim."),
        Pins, (Times[14] + Times[15]) * 0.5, Times[28], Times[0], Times.Last()));
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks");
    IFileManager::Get().MakeDirectory(*Directory, true);
    TestTrue(TEXT("Save native validation samples"), FFileHelper::SaveStringToFile(Csv, *(Directory / TEXT("1000-NativeValidation.csv"))));
    return true;
}

}
#endif
