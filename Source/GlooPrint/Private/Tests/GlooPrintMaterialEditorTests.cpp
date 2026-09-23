// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintMaterialTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Editor/Transactor.h"
#include "ConnectionDrawingPolicy.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "ImageUtils.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "MaterialEditorModule.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NodeFactory.h"
#include "Rendering/DrawElementTypes.h"
#include "RenderingThread.h"
#include "SGraphNode.h"
#include "ShaderCompiler.h"
#include "SGraphPanel.h"
#include "UObject/SavePackage.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace GlooPrint::Tests
{
namespace
{
class FMaterialFormatListener final : public FOutputDevice
{
public:
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (!IsInGameThread() || !bArmed || Category != TEXT("LogGlooPrintEditor")) { return; }
        const FString Text(Message);
        if (Text.StartsWith(TEXT("Formatted graph: ")))
        {
            Changed = FCString::Atoi(*Text.Mid(17)); Completed = FPlatformTime::Seconds(); bArmed = false;
        }
        else { LastMessage = Text; }
    }
    void Arm() { Changed = -1; Completed = 0; LastMessage.Reset(); bArmed = true; }
    bool bArmed = false;
    int32 Changed = -1;
    double Completed = 0;
    FString LastMessage;
};

TSharedPtr<SGraphEditor> FindMaterialGraphWidget(UMaterialGraph* Graph)
{
    TArray<TSharedRef<SWidget>> Pending;
    TArray<TSharedRef<SWindow>> Windows;
    FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
    for (const auto& Window : Windows) { Pending.Add(Window); }
    for (int32 I = 0; I < Pending.Num() && I < 50000; ++I)
    {
        const auto Widget = Pending[I];
        if (Widget->GetType() == TEXT("SGraphEditor"))
        {
            const auto Editor = StaticCastSharedRef<SGraphEditor>(Widget);
            if (Editor->GetCurrentGraph() == Graph) { return Editor; }
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 C = 0; C < Children->Num(); ++C) { Pending.Add(Children->GetChildAt(C)); }
    }
    return nullptr;
}
}

class FMaterialEditorCheck final : public IAutomationLatentCommand
{
public:
    FMaterialEditorCheck(FAutomationTestBase& InTest, int32 InCount, bool bInBenchmark, int32 InFamily = 0)
        : Test(InTest), Count(InCount), Family(InFamily), bBenchmark(bInBenchmark) {}
    virtual ~FMaterialEditorCheck() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        if (bFinishing)
        {
            if (++Frames < 30) { return false; }
            return Finish();
        }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(FString::Printf(TEXT("Material editor test timed out: phase=%d, %s"), Phase, *Listener.LastMessage)); return Finish();
        }
        if (Phase == 0)
        {
            if (++Frames < 20 || (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())) { return false; }
            Editor = FindMaterialGraphWidget(Graph);
            if (!Editor)
            {
                if (Frames == 20) { AssetEditor->JumpToHyperlink(Graph); AssetEditor->FocusWindow(); }
                return false;
            }
            Window = FSlateApplication::Get().FindWidgetWindow(Editor.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-1800, -700), 1.f);
            Anchor = Graph->RootNode ? static_cast<UEdGraphNode*>(Graph->RootNode) : Graph->Nodes[0].Get();
            Editor->ClearSelectionSet();
            Editor->SetNodeSelection(Anchor, true);
            if (!bBenchmark) for (auto* Client : GEditor->GetAllViewportClients())
            {
                PreviewFlags.Add(Client, Client->EngineShowFlags);
                // Fix exposure and temporal accumulation for image comparison.
                // Otherwise a paused native preview keeps its opening frame,
                // while Apply restarts exposure and temporal rendering.
                Client->EngineShowFlags.SetEyeAdaptation(false);
                Client->EngineShowFlags.SetTemporalAA(false);
                Client->EngineShowFlags.SetMotionBlur(false);
                Client->Invalidate(false, false);
            }
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            if (!bBenchmark) for (auto* Client : GEditor->GetAllViewportClients()) { Client->Invalidate(false, false); }
            if (++Frames < 20) { return false; }
            Before = SerializeTransactionValues(*Graph);
            if (!bBenchmark && PreviewBefore.IsEmpty()) { CapturePreview(false); }
            if (Count > 300 && !bCancellationChecked)
            {
                Begin(false);
                FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
                FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
                Phase = 9; Frames = 0; return false;
            }
            Begin(false); Phase = 2; return false;
        }
        if (Phase == 9)
        {
            if (++Frames < 20) { return false; }
            Test.TestTrue(TEXT("Escape leaves a large native material graph unchanged"), Before == SerializeTransactionValues(*Graph));
            Test.TestEqual(TEXT("Escape adds no layout transaction"), AppliedTransactions(), QueueBefore);
            bCancellationChecked = true; Phase = 1; Frames = 0; return false;
        }
        if (Phase == 2 || Phase == 4)
        {
            const auto Routes = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
            if (++Frames % 600 == 0)
            {
                UE_LOG(LogTemp, Display, TEXT("Material test waiting: phase %d, completed %d, cache %d, ready %d, pending %d, builds %d"),
                    Phase, Listener.Completed > 0, Routes.IsValid(), Routes && Routes->IsReady(), Routes && Routes->HasPendingRouting(), Routes ? Routes->GetBuildCount() : -1);
            }
            if (!Listener.Completed || !Routes || !Routes->IsReady()) { return false; }
            const double Milliseconds = (FMath::Max3(Listener.Completed, DispatchEnded, Routes->GetCompletedAt()) - Started) * 1000;
            const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
            const bool bRepeat = Phase == 4;
            Csv += FString::Printf(TEXT("%d,%d,%s,%.6f,%d,%d,%d,%d\n"), Count, Sample, bRepeat ? TEXT("Repeat") : TEXT("Cold"),
                Milliseconds, Listener.Changed, Cache ? Cache->GetHits() : -1, Cache ? Cache->GetMisses() : -1, Routes->GetRoutes().FallbackCount);
            (bRepeat ? RepeatTimes : ColdTimes).Add(Milliseconds);
            for (double Slice : GetFormatWorkStats().SliceMilliseconds)
            {
                SliceTimes.Add(Slice); SliceCsv += FString::Printf(TEXT("%d,%s,format,%.6f\n"), Sample, bRepeat ? TEXT("Repeat") : TEXT("Cold"), Slice);
            }
            if (!bRepeat) for (double Slice : Routes->GetWorkSlices())
            {
                SliceTimes.Add(Slice); SliceCsv += FString::Printf(TEXT("%d,%s,routes,%.6f\n"), Sample, bRepeat ? TEXT("Repeat") : TEXT("Cold"), Slice);
            }
            FVector2f CurrentView; float CurrentZoom; Editor->GetViewLocation(CurrentView, CurrentZoom);
            Test.TestTrue(TEXT("Material F keeps camera and zoom"), View == CurrentView && Zoom == CurrentZoom);
            Test.TestTrue(TEXT("Material F keeps selection"), Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Anchor));
            Test.TestTrue(TEXT("Material F keeps selected anchor"), AnchorPosition == FIntPoint(Anchor->NodePosX, Anchor->NodePosY));
            if (!bRepeat)
            {
                Test.TestEqual(TEXT("Material format reuses the completed routing plan"), Routes->GetReusedPlanCount(), ReusedPlansBefore + 1);
                Test.TestTrue(TEXT("Actual material-editor shortcut changes layout"), Listener.Changed > 0);
                Test.TestEqual(TEXT("Actual F uses one transaction"), AppliedTransactions(), QueueBefore + 1);
                After = SerializeTransactionValues(*Graph);
                if (!bBenchmark)
                {
                    Test.TestTrue(TEXT("Editor undo succeeds"), GEditor->UndoTransaction());
                    Test.TestTrue(TEXT("Editor undo restores nodes"), Before == SerializeTransactionValues(*Graph));
                    Test.TestTrue(TEXT("Editor redo succeeds"), GEditor->RedoTransaction());
                    Test.TestTrue(TEXT("Editor redo restores nodes"), After == SerializeTransactionValues(*Graph));
                    Editor->ClearSelectionSet(); Editor->SetNodeSelection(Anchor, true); // Native Material Editor clears selection during undo.
                }
                Phase = 3; Frames = 0; return false;
            }
            Test.TestEqual(TEXT("Repeated actual F changes no nodes"), Listener.Changed, 0);
            Test.TestEqual(TEXT("Repeated F adds no transaction"), AppliedTransactions(), QueueBefore);
            Test.TestTrue(TEXT("Repeated F preserves all graph values"), After == SerializeTransactionValues(*Graph));
            if (Test.HasAnyErrors()) { return Finish(); }
            if (++Sample < SamplesWanted)
            {
                Test.TestTrue(TEXT("Return to original layout for next cold sample"), GEditor->UndoTransaction());
                Editor->ClearSelectionSet(); Editor->SetNodeSelection(Anchor, true);
                Phase = 5; Frames = 0; return false;
            }
            if (bBenchmark) { Summarize(); if (Count == 300) { BenchmarkWirePaint(); } return Finish(); }
            auto* Panel = Editor->GetGraphPanel();
            FSlateApplication::Get().SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Panel->SummonContextMenu(Panel->GetCachedGeometry().LocalToAbsolute(FVector2f(300, 200)),
                FVector2f::ZeroVector, Anchor, nullptr, {});
            Phase = 7; Frames = 0; return false;
        }
        if (Phase == 7)
        {
            if (++Frames < 10) { return false; }
            TArray<TSharedRef<SWidget>> Widgets;
            auto& Slate = FSlateApplication::Get();
            const auto MenuWindow = Slate.GetVisibleMenuWindow();
            if (!Test.TestTrue(TEXT("Native material context menu opens"), MenuWindow.IsValid())) { return Finish(); }
            // Slate may host a popup in the editor window's overlay instead of
            // the native menu window, depending on the platform popup method.
            TArray<TSharedRef<SWindow>> MenuWindows;
            Slate.GetAllVisibleWindowsOrdered(MenuWindows);
            for (const auto& Candidate : MenuWindows) { Widgets.Add(Candidate); }
            FString MenuLabels;
            for (int32 I = 0; I < Widgets.Num() && I < 8192; ++I)
            {
                const auto Widget = Widgets[I];
                if (Widget->GetType() == TEXT("STextBlock")) { MenuLabels += StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() + TEXT(" | "); }
                if (Widget->GetType() == TEXT("STextBlock") && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == TEXT("Format Graph"))
                {
                    const FVector2f Point = Widget->GetCachedGeometry().LocalToAbsolute(Widget->GetCachedGeometry().GetLocalSize() * .5f);
                    Listener.Arm(); QueueBefore = AppliedTransactions();
                    const FVector2f Previous = Slate.GetCursorPos();
                    Slate.SetCursorPos(FVector2D(Point));
                    Slate.ProcessMouseMoveEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Point, Previous, {}, EKeys::Invalid, 0, FModifierKeysState()), false);
                    const auto PopupHost = Slate.FindWidgetWindow(Widget);
                    Slate.ProcessMouseButtonDownEvent(PopupHost->GetNativeWindow(), FPointerEvent(FSlateApplication::CursorPointerIndex, Point, Point,
                        TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState()));
                    Slate.ProcessMouseButtonUpEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Point, Point, {}, EKeys::LeftMouseButton, 0, FModifierKeysState()));
                    Phase = 8; Frames = 0; return false;
                }
                FChildren* Children = Widget->GetChildren();
                for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
            }
            Test.AddError(TEXT("Native material node menu is missing Format Graph. Labels: ") + MenuLabels); return Finish();
        }
        if (Phase == 8)
        {
            if (!Listener.Completed) { return false; }
            Test.TestEqual(TEXT("Actual material menu formats a settled graph without changes"), Listener.Changed, 0);
            Test.TestEqual(TEXT("Material menu no-op adds no undo"), AppliedTransactions(), QueueBefore);
            // Native Apply copies the editor's preview material/function back to the original asset.
            Test.TestTrue(TEXT("Native Apply command is available after formatting"),
                AssetEditor->GetToolkitCommands()->CanExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
            AssetEditor->GetToolkitCommands()->ExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef());
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 3 || Phase == 5)
        {
            if (++Frames < 8 || (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())) { return false; }
            const bool bRepeat = Phase == 3; Begin(bRepeat); Phase = bRepeat ? 4 : 2; return false;
        }
        if (Phase == 6)
        {
            const auto* Material = CastChecked<UMaterial>(AssetEditor->GetMaterialInterface());
            const auto* Resource = Material->GetMaterialResource(GEditor->PreviewPlatform.GetShaderPlatform());
            if ((GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) || !Resource || !Resource->IsCompilationFinished())
            {
                Frames = 0; return false;
            }
            // Non-realtime native viewports may retain the temporary fallback
            // frame after Apply completes. Redraw after shader finalization.
            for (auto* Client : GEditor->GetAllViewportClients()) { Client->Invalidate(false, false); }
            if (++Frames < 20) { return false; }
            Test.TestTrue(TEXT("Applied fixture compiles without errors"), Resource->GetCompileErrors().IsEmpty());
            CapturePreview(true); SaveAndCompare(); return Finish();
        }
        return false;
    }

private:
    int32 AppliedTransactions() const { return GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); }
    void CapturePreview(bool bAfter)
    {
        TArray<TSharedRef<SWidget>> Widgets{Window.ToSharedRef()};
        for (int32 I = 0; I < Widgets.Num() && I < 12000; ++I)
        {
            const auto Widget = Widgets[I]; const FVector2f Size = Widget->GetCachedGeometry().GetLocalSize();
            if (Widget->GetType() == TEXT("SViewport") && Size.X > 200 && Size.Y > 200)
            {
                TArray<FColor> Pixels; FIntVector Dimensions;
                if (!Test.TestTrue(TEXT("Capture native material rendered preview"), FSlateApplication::Get().TakeScreenshot(Widget, Pixels, Dimensions))) { return; }
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Dimensions.X, Dimensions.Y, Pixels, Png);
                FFileHelper::SaveArrayToFile(Png, *(Directory / (Stem() + (bAfter ? TEXT("-PreviewAfter.png") : TEXT("-PreviewBefore.png")))));
                if (!bAfter) { PreviewBefore = MoveTemp(Pixels); PreviewSize = Dimensions; return; }
                if (Test.TestTrue(TEXT("Preview dimensions remain unchanged"), PreviewSize.X == Dimensions.X && PreviewSize.Y == Dimensions.Y && Pixels.Num() == PreviewBefore.Num()))
                {
                    // The native sphere viewport uses temporal rendering. Compare
                    // average RGB error, allowing subpixel antialiasing noise.
                    double Difference = 0;
                    for (int32 P = 0; P < Pixels.Num(); ++P)
                    {
                        Difference += FMath::Abs(int32(Pixels[P].R) - PreviewBefore[P].R) +
                            FMath::Abs(int32(Pixels[P].G) - PreviewBefore[P].G) + FMath::Abs(int32(Pixels[P].B) - PreviewBefore[P].B);
                    }
                    Difference /= FMath::Max(1, Pixels.Num() * 3);
                    Test.AddInfo(FString::Printf(TEXT("Native preview mean RGB difference after format/Apply: %.5f/255"), Difference));
                    Test.TestTrue(TEXT("Formatting preserves the rendered material preview"), Difference <= 1.0);
                }
                return;
            }
            auto* Children = Widget->GetChildren();
            for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
        }
        Test.AddError(TEXT("Native material preview viewport was not found."));
    }
    bool Start()
    {
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); OriginalStyle = Settings->WireStyle;
        bOriginalEnabled = Settings->bFormattingEnabled; OriginalLayout = Settings->GetLayoutSettings(); bRestore = true; OriginalCursor = FSlateApplication::Get().GetCursorPos();
        Settings->WireStyle = EGlooPrintWireStyle::Native; Settings->bFormattingEnabled = true;
        Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32; Settings->NotifyChanged();
        SamplesWanted = bBenchmark && Count <= 300 ? 20 : 1;
        if (bBenchmark) { FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted); SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100); }
        Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintMaterials"); IFileManager::Get().MakeDirectory(*Directory, true);
        Package.Reset(CreatePackage(*FString::Printf(TEXT("/Game/GlooPrintMaterialTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
        Fixture = MakeUnique<FMaterialFixture>(Family, Count, Package.Get(), bBenchmark);
        UObject* Asset = Family ? static_cast<UObject*>(Fixture->Function.Get()) : Fixture->Material.Get();
        Asset->SetFlags(RF_Public | RF_Standalone);
        AssetEditor = Family ? IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, {}, Fixture->Function.Get()) :
            IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, {}, Fixture->Material.Get());
        // The native editor owns these now and replaces the original UObject
        // during Apply/Undo. A TStrongObjectPtr would prohibit that replacement.
        Fixture->Material.Reset(); Fixture->Function.Reset();
        auto* Preview = Cast<UMaterial>(AssetEditor->GetMaterialInterface());
        if (!Test.TestNotNull(TEXT("Real material editor exposes its preview"), Preview)) { return Finish(); }
        Graph = Preview->MaterialGraph;
        if (!Test.TestNotNull(TEXT("Real material editor exposes its native graph"), Graph)) { return Finish(); }
        GLog->AddOutputDevice(&Listener); bListening = true;
        Deadline = FPlatformTime::Seconds() + (Count > 300 ? 900 : 300); return false;
    }
    void Begin(bool bRepeat)
    {
        auto* Panel = Editor->GetGraphPanel(); auto& Slate = FSlateApplication::Get();
        const auto ExistingRoutes = Panel->GetMetaData<FRouteCache>();
        ReusedPlansBefore = ExistingRoutes ? ExistingRoutes->GetReusedPlanCount() : 0;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        if (Settings->WireStyle != EGlooPrintWireStyle::Rounded90) { Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->NotifyChanged(); }
        if (!bRepeat)
        {
            if (const auto Routes = Panel->GetMetaData<FRouteCache>()) { Routes->Invalidate(); }
            if (const auto Cache = Panel->GetMetaData<FMeasurementCache>()) { Cache->Invalidate(); }
        }
        Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Editor->GetViewLocation(View, Zoom); AnchorPosition = {Anchor->NodePosX, Anchor->NodePosY};
        QueueBefore = AppliedTransactions(); Listener.Arm(); Started = FPlatformTime::Seconds();
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        DispatchEnded = FPlatformTime::Seconds(); Deadline = Started + (Count > 300 ? 900 : 180);
    }
    void SaveAndCompare()
    {
        const auto* Assets = AssetEditor->GetObjectsCurrentlyBeingEdited();
        if (!Assets || Assets->IsEmpty()) { Test.AddError(TEXT("The native material editor lost its asset.")); return; }
        UObject* Asset = (*Assets)[0];
        const auto OriginalExpressions = Family ? CastChecked<UMaterialFunction>(Asset)->GetExpressions() : CastChecked<UMaterial>(Asset)->GetExpressions();
        TMap<FGuid, FIntPoint> Saved;
        FString Metadata = TEXT("name,class,x,y,inputs\n");
        for (UMaterialExpression* Expression : OriginalExpressions)
        {
            Saved.Add(Expression->MaterialExpressionGuid, {Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY});
            TArray<FString> Inputs;
            for (const FExpressionInput* Input : Expression->GetInputsView()) { Inputs.Add(Input && Input->Expression ? Input->Expression->GetName() : TEXT("")); }
            Metadata += FString::Printf(TEXT("%s,%s,%d,%d,%s\n"), *Expression->GetName(), *Expression->GetClass()->GetPathName(),
                Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY, *FString::Join(Inputs, TEXT("|")));
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(*Node)))
            {
                const FIntPoint* Position = Saved.Find(Expression->MaterialExpressionGuid);
                Test.TestTrue(TEXT("Apply copies expression coordinates to original asset"), Position && *Position == FIntPoint(Node->NodePosX, Node->NodePosY));
            }
        }
        const FString Filename = Directory / (Stem() + TEXT(".uasset"));
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        Test.TestTrue(TEXT("Native material asset saves"), UPackage::SavePackage(Package.Get(), Asset, *Filename, Args));
        FFileHelper::SaveStringToFile(Metadata, *(Directory / (Stem() + TEXT(".expressions.csv"))));
        FFileHelper::SaveStringToFile(Asset->GetName(), *(Directory / (Stem() + TEXT(".asset.txt"))));
    }
    void Summarize()
    {
        if (SamplesWanted < 20 || Count > 300) { Test.AddInfo(TEXT("Diagnostic/stress run only; no performance acceptance claim.")); return; }
        const auto P95 = [](TArray<double> Times) { Times.Sort(); return Times[FMath::CeilToInt(Times.Num() * .95) - 1]; };
        const double Cold = P95(ColdTimes), Repeat = P95(RepeatTimes), Slices = P95(SliceTimes);
        double Maximum = 0; for (double Slice : SliceTimes) { Maximum = FMath::Max(Maximum, Slice); }
        Test.AddInfo(FString::Printf(TEXT("%d material nodes, %d samples: p95 cold %.3fms, repeat %.3fms, slice %.3fms, max slice %.3fms; CPU %s"),
            Count, SamplesWanted, Cold, Repeat, Slices, Maximum, *FPlatformMisc::GetCPUBrand()));
        Test.TestTrue(TEXT("Material cold format p95 <= 1000ms"), Cold <= 1000);
        Test.TestTrue(TEXT("Material repeat format p95 <= 250ms"), Repeat <= 250);
        Test.TestTrue(TEXT("Material work slices p95 <= 8ms"), Slices <= 8);
        Test.TestTrue(TEXT("No material work slice > 50ms"), Maximum <= 50);
    }
    void BenchmarkWirePaint()
    {
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FMeasurementCache>();
        const auto Routes = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Routes || !Routes->IsReady()) { Test.AddError(TEXT("Material paint benchmark needs cached routes.")); return; }
        FMeasurementOptions Options; Options.Cache = Cache.Get(); FGraphMeasurement Measurement; FString Reason;
        if (!Test.TestTrue(TEXT("Paint benchmark uses cached native geometry"), MeasureGraph(Graph, Window->GetDPIScaleFactor() * FSlateApplication::Get().GetApplicationScale(), Measurement, Reason, Options)))
        {
            Test.AddError(Reason); return;
        }
        TMap<FGuid, const FMeasuredNode*> Geometry;
        for (const auto& Node : Measurement.Nodes) { Geometry.Add(Node.Id, &Node); }
        FArrangedChildren Nodes(EVisibility::Visible);
        TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
        // Isolate policy CPU with every wire in the clip. Native widgets provide
        // the measured attachments, including nodes outside the editor viewport.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Widget) { Test.AddError(TEXT("Native material node widget is unavailable.")); return; }
            const auto& Measured = *Geometry.FindChecked(Node->NodeGuid);
            const FVector2f Position(Node->NodePosX, Node->NodePosY);
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), FGeometry::MakeRoot(Measured.BodySize, FSlateLayoutTransform(Position))));
            for (int32 I = 0; I < Node->Pins.Num(); ++I)
            {
                if (!Measured.Pins[I].AttachmentOffset.IsSet()) { continue; }
                const auto PinWidget = Widget->FindWidgetForPin(Node->Pins[I]); if (!PinWidget) { continue; }
                const FVector2f Attachment = Position + Measured.Pins[I].AttachmentOffset.GetValue();
                const FVector2f Min = Attachment - FVector2f(Node->Pins[I]->Direction == EGPD_Output ? 16.f : 0.f, 8.f);
                Pins.Add(PinWidget.ToSharedRef(), FArrangedWidget(PinWidget.ToSharedRef(), FGeometry::MakeRoot(FVector2f(16), FSlateLayoutTransform(Min))));
            }
        }
        auto Factory = MakeShared<FWireDrawing>();
        const FSlateRect Clip(-10000000, -10000000, 10000000, 10000000);
        TArray<double> Times;
        FString PaintCsv = TEXT("sample,connections,draw_pieces,policy_ms\n");
        const int32 Builds = Routes->GetBuildCount();
        for (int32 I = -3; I < 20; ++I)
        {
            FSlateWindowElementList Elements(Window);
            const double Start = FPlatformTime::Seconds();
            TUniquePtr<FConnectionDrawingPolicy> Policy(Factory->CreateConnectionPolicy(Graph->GetSchema(), 0, 1, 1, Clip, Elements, Graph));
            Policy->SetAbsoluteMousePosition(FVector2f(-10000000)); Policy->Draw(Pins, Nodes);
            const double Ms = (FPlatformTime::Seconds() - Start) * 1000;
            const int32 Pieces = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>().Num();
            PaintCsv += FString::Printf(TEXT("%d,%d,%d,%.6f\n"), I, Routes->GetRoutes().Wires.Num(), Pieces, Ms);
            if (I >= 0) { Times.Add(Ms); }
            Test.TestTrue(TEXT("All cached material connections painted"), Pieces >= Routes->GetRoutes().Wires.Num());
        }
        Test.TestEqual(TEXT("Idle material paint does not restart routing"), Routes->GetBuildCount(), Builds);
        Times.Sort(); const double P95 = Times[18];
        Test.AddInfo(FString::Printf(TEXT("Material wire paint: %d connections, 20 samples, p95 %.3fms"), Routes->GetRoutes().Wires.Num(), P95));
        Test.TestTrue(TEXT("Material cached wire policy p95 <= 2ms"), P95 <= 2);
        FFileHelper::SaveStringToFile(PaintCsv, *(Directory / TEXT("300-WirePaint.csv")));
        Factory->Shutdown();
    }
    FString Stem() const { return FString::Printf(TEXT("%d-Family%d-%s"), Count, Family, bBenchmark ? TEXT("Benchmark") : TEXT("NativeEditor")); }
    bool Finish()
    {
        if (Editor && !bFinishing)
        {
            bFinishing = true; Frames = 0;
            Editor->GetGraphPanel()->DismissContextMenu();
            Editor->ZoomToFit(false);
            return false;
        }
        if (!Directory.IsEmpty())
        {
            Test.TestTrue(TEXT("Save material timings"), FFileHelper::SaveStringToFile(Csv, *(Directory / (Stem() + TEXT(".csv")))));
            FFileHelper::SaveStringToFile(SliceCsv, *(Directory / (Stem() + TEXT("-Slices.csv"))));
            if (Editor)
            {
                TArray<FColor> Pixels; FIntVector Size;
                if (FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))
                {
                    TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                    FFileHelper::SaveArrayToFile(Png, *(Directory / (Stem() + TEXT(".png"))));
                }
            }
        }
        Restore(); return true;
    }
    void Restore()
    {
        for (auto* Client : GEditor->GetAllViewportClients())
        {
            if (const auto* Flags = PreviewFlags.Find(Client)) { Client->EngineShowFlags = *Flags; Client->Invalidate(false, false); }
        }
        PreviewFlags.Reset();
        if (bListening) { Listener.bArmed = false; GLog->RemoveOutputDevice(&Listener); bListening = false; }
        if (Editor) { Editor->GetGraphPanel()->DismissContextMenu(); }
        if (AssetEditor) { AssetEditor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); AssetEditor.Reset(); }
        Editor.Reset(); Window.Reset();
        if (bRestore)
        {
            bRestore = false; FSlateApplication::Get().SetCursorPos(OriginalCursor); auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = OriginalStyle;
            Settings->bFormattingEnabled = bOriginalEnabled; Settings->HorizontalSpacing = OriginalLayout.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalLayout.VerticalSpacing; Settings->CommentPadding = OriginalLayout.CommentPadding; Settings->NotifyChanged();
        }
    }
    FAutomationTestBase& Test;
    int32 Count, Family = 0, Phase = 0, Frames = 0, Sample = 0, SamplesWanted = 1, QueueBefore = 0, ReusedPlansBefore = 0;
    bool bBenchmark, bRestore = false, bListening = false, bOriginalEnabled = false, bCancellationChecked = false, bFinishing = false;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Native;
    FLayoutSettings OriginalLayout;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<FMaterialFixture> Fixture;
    TSharedPtr<IMaterialEditor> AssetEditor;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    UMaterialGraph* Graph = nullptr;
    UEdGraphNode* Anchor = nullptr;
    FMaterialFormatListener Listener;
    FVector2f View;
    FVector2D OriginalCursor;
    FIntPoint AnchorPosition;
    float Zoom = 0;
    double Deadline = 0, Started = 0, DispatchEnded = 0;
    FString Directory, Csv = TEXT("nodes,sample,stage,command_ms,changed,final_capture_hits,final_capture_misses,fallbacks\n");
    FString SliceCsv = TEXT("sample,stage,work,slice_ms\n");
    TArray<uint8> Before, After;
    TArray<FColor> PreviewBefore;
    TMap<FEditorViewportClient*, FEngineShowFlags> PreviewFlags;
    FIntVector PreviewSize = FIntVector::ZeroValue;
    TArray<double> ColdTimes, RepeatTimes, SliceTimes;
};

class FMaterialEditorCleanup final : public IAutomationLatentCommand
{
public:
    virtual bool Update() override
    {
        // Native close defers Slate/preview-scene destruction until subsequent
        // ticks. Release fixture transactions before renderer shutdown as well.
        if (++Frames < 30 || (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())) { return false; }
        GEditor->Trans->Reset(NSLOCTEXT("GlooPrintTests", "MaterialFixtureCleanup", "Release disposable material fixtures"));
        // Let the editor own global GC/world teardown. Forcing full GC here can
        // collect unrelated worlds retained by earlier automation fixtures before
        // their editor subsystems have received the normal world-cleanup event.
        FlushRenderingCommands();
        return true;
    }
private:
    int32 Frames = 0;
};

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMaterialNativeEditorTest, "GlooPrint.Materials.NativeEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FMaterialNativeEditorTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    const TCHAR* Families[] = {TEXT("Material"), TEXT("Function"), TEXT("Layer"), TEXT("Blend")};
    for (int32 I = 0; I < 4; ++I) { Names.Add(Families[I]); Commands.Add(FString::FromInt(I)); }
}
bool FMaterialNativeEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FMaterialEditorCheck(*this, 12, false, FCString::Atoi(*Parameters)));
    ADD_LATENT_AUTOMATION_COMMAND(FMaterialEditorCleanup()); return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMaterialNativePerformanceTest, "GlooPrint.Performance.Materials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FMaterialNativePerformanceTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int32 Count : {50, 150, 300, 1000}) { Names.Add(FString::FromInt(Count)); Commands.Add(FString::FromInt(Count)); }
}
bool FMaterialNativePerformanceTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FMaterialEditorCheck(*this, FCString::Atoi(*Parameters), true));
    ADD_LATENT_AUTOMATION_COMMAND(FMaterialEditorCleanup()); return true;
}
}
#endif
