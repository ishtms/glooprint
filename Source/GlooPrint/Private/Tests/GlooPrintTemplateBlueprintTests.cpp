// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Algo/Reverse.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NodeFactory.h"
#include "Rendering/DrawElements.h"
#include "Serialization/ObjectWriter.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "UObject/LinkerInstancingContext.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FTemplateBlueprintCheck final : public IAutomationLatentCommand, public FOutputDevice
{
public:
    FTemplateBlueprintCheck(FAutomationTestBase& InTest, const TCHAR* InAssetName, bool bInZoomCase = false)
        : Test(InTest), AssetName(InAssetName), bZoomCase(bInZoomCase) {}
    virtual ~FTemplateBlueprintCheck() { Restore(); }
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (!IsInGameThread() || !bAwaitingFormat || Category != FName(TEXT("LogGlooPrintEditor"))) { return; }
        static constexpr TCHAR Prefix[] = TEXT("Formatted graph: ");
        LastMessage = Message;
        if (FCString::Strncmp(Message, Prefix, UE_ARRAY_COUNT(Prefix) - 1) == 0)
        {
            Changed = FCString::Atoi(Message + UE_ARRAY_COUNT(Prefix) - 1); bAwaitingFormat = false;
            Editor->GetViewLocation(CompletedView, CompletedZoom);
        }
    }
    virtual bool Update() override
    {
        if (!bInitialized) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(TEXT("Template Blueprint check exceeded its deadline. ") + LastMessage); return Finish();
        }
        if (++Frames < 12) { return false; }
        auto& Slate = FSlateApplication::Get();
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady() || bAwaitingFormat) { return false; }
        if (Phase == 7) { return CheckZoom(*Cache); }
        FString Reason;
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        if (Phase == 0)
        {
            const auto Original = SerializeNodes(*Graph);
            if (!Test.TestTrue(TEXT("Authored construction script has a complete format plan"),
                PlanFormatGraph(Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Planning preserves authored graph data"), Original == SerializeNodes(*Graph));
            Test.AddInfo(FString::Printf(TEXT("Installed %s construction script: %d nodes, %d pins, %d links, %d spacing repairs."),
                *AssetName, Plan.Snapshot.Nodes.Num(), Plan.Snapshot.Pins.Num(), Plan.Snapshot.Edges.Num(), Plan.SpacingRepairs));
            RecordMetrics(TEXT("AuthoredRounded"), Plan.Snapshot, Cache->GetRoutes());
            FBox2f Bounds(ForceInit);
            for (int32 I = 0; I < Plan.Snapshot.Nodes.Num(); ++I)
            {
                const auto& N = Plan.Snapshot.Nodes[I];
                Bounds += FVector2f(N.Geometry.Position); Bounds += FVector2f(N.Geometry.Position) + N.Geometry.BodySize;
                const FVector2f ProposedSize = N.bComment ? FVector2f(Plan.Layout.Sizes[I]) : N.Geometry.BodySize;
                Bounds += FVector2f(Plan.Layout.Positions[I]); Bounds += FVector2f(Plan.Layout.Positions[I]) + ProposedSize;
            }
            const FRouteSet* RouteSets[] = {&Plan.Routes, &Cache->GetRoutes()};
            for (const auto* Routes : RouteSets)
            {
                for (const auto& Pair : Routes->Wires) { Bounds += Pair.Value.Bounds.Min; Bounds += Pair.Value.Bounds.Max; }
            }
            const FVector2f Available = Panel->GetCachedGeometry().GetLocalSize();
            const FVector2f Span = Bounds.Max - Bounds.Min + FVector2f(160);
            const float FitZoom = FMath::Min(0.25f, FMath::Min(Available.X / Span.X, Available.Y / Span.Y));
            const auto& Levels = Panel->GetZoomLevels(); float CaptureZoom = 0;
            for (int32 I = 0; I < Levels->GetNumZoomLevels(); ++I)
            {
                const float Amount = Levels->GetZoomAmount(I);
                if (Amount <= FitZoom) { CaptureZoom = FMath::Max(CaptureZoom, Amount); }
            }
            if (!Test.TestTrue(TEXT("A supported native zoom fits both complete layouts"), CaptureZoom > 0)) { return Finish(); }
            CaptureBounds = Bounds;
            Editor->SetViewLocation(Bounds.Min - FVector2f(80), CaptureZoom);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            CheckPaintedRoutes(TEXT("AuthoredRounded"), Cache->GetRoutes(), false);
            Capture(TEXT("BeforeRounded")); Before = SerializeTransactionValues(*Graph); Properties = DescribeNodes(*Graph);
            TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
            for (auto* Other : Graphs) { if (Other != Graph) { OtherGraphs.Add(Other, SerializeTransactionValues(*Other)); } }
            FObjectWriter DefaultWriter(Blueprint->GeneratedClass->GetDefaultObject(), Defaults);
            Editor->GetViewLocation(View, Zoom); Anchor = FIntPoint(Entry->NodePosX, Entry->NodePosY);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            BeginFormat(); Phase = 2; return false;
        }
        if (Phase == 2)
        {
            Test.TestTrue(TEXT("Actual F formats the authored layout"), Changed > 0);
            Test.TestEqual(TEXT("Template F creates one undo action"), GEditor->Trans->GetQueueLength(), Queue + 1);
            CheckContext(); CheckProperties();
            for (const auto& Pair : OtherGraphs) { Test.TestTrue(TEXT("Formatting leaves other Blueprint graphs unchanged"), Pair.Value == SerializeTransactionValues(*Pair.Key)); }
            TArray<uint8> CurrentDefaults; FObjectWriter DefaultWriter(Blueprint->GeneratedClass->GetDefaultObject(), CurrentDefaults);
            Test.TestTrue(TEXT("Formatting leaves Blueprint class defaults unchanged"), Defaults == CurrentDefaults);
            After = SerializeTransactionValues(*Graph);
            Test.TestTrue(TEXT("Authored Blueprint format undoes"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores every authored graph value"), Before == SerializeTransactionValues(*Graph));
            Test.TestTrue(TEXT("Authored Blueprint format redoes"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores every formatted graph value"), After == SerializeTransactionValues(*Graph));
            if (!Compile()) { return Finish(); }
            PrimeTooltips(); After = SerializeTransactionValues(*Graph);
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3 || Phase == 5)
        {
            const bool bDiagonal = Phase == 5;
            FFormatPlan Cold;
            if (!Test.TestTrue(TEXT("Template cold native geometry computes"), PlanFormatGraph(Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            Test.TestTrue(TEXT("Template layout is idempotent with cold caches"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.Sizes == Plan.Layout.Sizes);
            CheckRoutes(Cold, Cache->GetRoutes());
            FLayoutGraph Shuffled = Cold.Snapshot; Algo::Reverse(Shuffled.Edges); FRouteSet ShuffledRoutes;
            if (Test.TestTrue(TEXT("Reordered authored links route"), ComputeLayoutRoutes(Shuffled, Cold.Layout, ShuffledRoutes, Reason,
                bDiagonal ? EGlooPrintWireStyle::Diagonal45 : EGlooPrintWireStyle::Rounded90))) { CheckRoutes(Cold, ShuffledRoutes); }
            RecordMetrics(bDiagonal ? TEXT("FormattedDiagonal") : TEXT("FormattedRounded"), Cold.Snapshot, Cache->GetRoutes());
            CheckPaintedRoutes(bDiagonal ? TEXT("FormattedDiagonal") : TEXT("FormattedRounded"), Cache->GetRoutes());
            Capture(bDiagonal ? TEXT("AfterDiagonal") : TEXT("AfterRounded"));
            Queue = GEditor->Trans->GetQueueLength(); Package->SetDirtyFlag(false); After = SerializeTransactionValues(*Graph);
            BeginFormat(); Phase = bDiagonal ? 6 : 4; return false;
        }
        Test.TestEqual(TEXT("Repeated F completes as a true no-op"), Changed, 0);
        Test.TestEqual(TEXT("No-op creates no undo action"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestTrue(TEXT("No-op preserves every graph value"), After == SerializeTransactionValues(*Graph));
        Test.TestFalse(TEXT("No-op leaves the private package clean"), Package->IsDirty()); CheckContext();
        if (Phase == 4)
        {
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Diagonal45;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); Phase = 5; Frames = 0; return false;
        }
        TArray<uint8> CurrentSource, CurrentCopy;
        Test.TestTrue(TEXT("Installed template bytes remain unchanged"), FFileHelper::LoadFileToArray(CurrentSource, *SourcePath) && CurrentSource == SourceBytes);
        Test.TestTrue(TEXT("Private asset file remains an untouched authored baseline"), FFileHelper::LoadFileToArray(CurrentCopy, *CopyPath) && CurrentCopy == SourceBytes);
        Test.TestTrue(TEXT("Save real Blueprint quality metrics"), FFileHelper::SaveStringToFile(Metrics, *(Directory / TEXT("metrics.csv"))));
        if (bZoomCase)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->Direction != EGPD_Output || Pin->PinName != TEXT("LoopBody") || Pin->LinkedTo.Num() != 1) { continue; }
                if (!Test.TestNull(TEXT("Zoom fixture identifies one LoopBody connection"), ZoomOutput)) { return Finish(); }
                ZoomOutput = Pin; ZoomInput = Pin->LinkedTo[0];
            }
            if (!Test.TestNotNull(TEXT("Zoom fixture retains the authored LoopBody output"), ZoomOutput)) { return Finish(); }
            Phase = 7; ZoomIndex = 0; SetZoomView(); return false;
        }
        return Finish();
    }
private:
    void SetZoomView()
    {
        auto* Panel = Editor->GetGraphPanel();
        const float Amount = Panel->GetZoomLevels()->GetZoomAmount(ZoomIndex);
        const auto OutputNode = Panel->GetNodeWidgetFromGuid(ZoomOutput->GetOwningNode()->NodeGuid);
        const auto InputNode = Panel->GetNodeWidgetFromGuid(ZoomInput->GetOwningNode()->NodeGuid);
        const FVector2f Center = (OutputNode->GetPosition2f() + FVector2f(OutputNode->GetDesiredSize().X, 0) + InputNode->GetPosition2f()) * 0.5f;
        Editor->SetViewLocation(Center - Panel->GetCachedGeometry().GetLocalSize() / (2 * Amount), Amount);
        Frames = 0;
    }
    bool CheckZoom(const FRouteCache& Cache)
    {
        auto* Panel = Editor->GetGraphPanel();
        const float Amount = Panel->GetZoomLevels()->GetZoomAmount(ZoomIndex);
        const float Scale = Panel->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale() * Amount;
        Test.TestEqual(TEXT("Zoom check reaches the requested supported level"), Panel->GetZoomAmount(), Amount);
        Test.TestTrue(TEXT("Zoom changes no serialized Blueprint values"), After == SerializeTransactionValues(*Graph));
        Test.TestEqual(TEXT("Zoom creates no undo action"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestFalse(TEXT("Zoom leaves the private package clean"), Package->IsDirty());
        const FRouteKey Key{ZoomOutput->GetOwningNode()->NodeGuid, ZoomOutput->PinId, ZoomInput->GetOwningNode()->NodeGuid, ZoomInput->PinId};
        const auto* Route = Cache.GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Zoom keeps the direct execution route"), Route && Route->Curves.Num() == 1)) { return Finish(); }
        if (ZoomIndex == 0) { ZoomBuilds = Cache.GetBuildCount(); ZoomPoints = Route->Points; }
        Test.TestEqual(TEXT("Zoom alone reuses the route cache"), Cache.GetBuildCount(), ZoomBuilds);
        Test.TestTrue(TEXT("Zoom alone preserves graph-coordinate routes"), Route->Points == ZoomPoints);
        Test.TestTrue(TEXT("Measured execution pins remain exactly aligned in graph space"),
            FMath::IsNearlyEqual(Route->Points[0].Y, Route->Points.Last().Y, 0.1f));
        FArrangedChildren Nodes(EVisibility::Visible);
        TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
        FVector2f Offsets[2]; int32 Endpoint = 0;
        for (UEdGraphPin* Pin : {ZoomOutput, ZoomInput})
        {
            const auto Node = Panel->GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid);
            const auto Widget = Node->FindWidgetForPin(Pin);
            if (!Test.TestTrue(TEXT("Zoom retains the original native pin widget"), Widget.IsValid())) { return Finish(); }
            const auto& Geometry = Node->GetCachedGeometry();
            const FVector2f ExpectedOrigin = Panel->GetCachedGeometry().LocalToAbsolute((Node->GetPosition2f() - FVector2f(Panel->GetViewOffset())) * Amount);
            if (!Test.TestTrue(TEXT("Zoom checks fresh visible node geometry"), Geometry.GetAbsolutePosition().Equals(ExpectedOrigin, 0.1f) &&
                FMath::IsNearlyEqual(Geometry.GetAccumulatedLayoutTransform().GetScale(), Scale, 0.001f))) { return Finish(); }
            const auto& PinGeometry = Widget->GetCachedGeometry();
            const FVector2f PinCenter = PinGeometry.LocalToAbsolute(PinGeometry.GetLocalSize() * 0.5f);
            const FVector2f PanelPoint = Panel->GetCachedGeometry().AbsoluteToLocal(PinCenter);
            const FVector2f PanelSize = Panel->GetCachedGeometry().GetLocalSize();
            if (!Test.TestTrue(TEXT("Both checked pins are visible at every zoom"), PanelPoint.X > 0 && PanelPoint.Y > 0 &&
                PanelPoint.X < PanelSize.X && PanelPoint.Y < PanelSize.Y)) { return Finish(); }
            Offsets[Endpoint++] = Geometry.AbsoluteToLocal(PinCenter);
            Nodes.AddWidget(FArrangedWidget(Node.ToSharedRef(), Geometry));
            Pins.Add(Widget.ToSharedRef(), FArrangedWidget(Widget.ToSharedRef(), PinGeometry));
        }
        FSlateWindowElementList NativeElements(Window), CustomElements(Window);
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Graph);
        TUniquePtr<FConnectionDrawingPolicy> Custom(FNodeFactory::CreateConnectionPolicy(Graph->GetSchema(), 0, 1, Scale, Clip, CustomElements, Graph));
        if (!Test.TestTrue(TEXT("Zoom uses the custom connection policy"), Custom.IsValid())) { return Finish(); }
        Native.SetAbsoluteMousePosition(FVector2f(-100000)); Native.Draw(Pins, Nodes);
        const auto& Baseline = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
        if (!Test.TestEqual(TEXT("Zoom isolates one native execution wire"), Baseline.Num(), 1)) { return Finish(); }
        Custom->SetAbsoluteMousePosition((Baseline[0].P0 + Baseline[0].P3) * 0.5f); Custom->Draw(Pins, Nodes);
        const auto& Pieces = CustomElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
        if (!Test.TestEqual(TEXT("Zoom draws the cached single piece without a detour"), Pieces.Num(), 1)) { return Finish(); }
        Test.TestTrue(TEXT("Zoom keeps exact native wire attachment"), Pieces[0].P0.Equals(Baseline[0].P0, 0.1f) && Pieces[0].P3.Equals(Baseline[0].P3, 0.1f));
        Test.TestTrue(TEXT("Zoom keeps horizontal departure and arrival tangents"),
            FMath::IsNearlyEqual(Pieces[0].P0.Y, Pieces[0].P1.Y, 0.1f) && FMath::IsNearlyEqual(Pieces[0].P2.Y, Pieces[0].P3.Y, 0.1f));
        UEdGraphPin* HitA = nullptr; UEdGraphPin* HitB = nullptr;
        Test.TestTrue(TEXT("Visible wire hover identifies its original pin pair at every zoom"), Custom->SplineOverlapResult.GetPins(*Panel, HitA, HitB) &&
            ((HitA == ZoomOutput && HitB == ZoomInput) || (HitA == ZoomInput && HitB == ZoomOutput)));
        const TCHAR* Style = bZoomRounded ? TEXT("rounded") : TEXT("diagonal");
        ZoomMetrics += FString::Printf(TEXT("%s,%.3f,%.3f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"), Style, double(Amount), double(Scale),
            int32(Panel->GetCurrentLOD()), double(Offsets[0].Y), double(Offsets[1].Y), double(Baseline[0].P3.Y - Baseline[0].P0.Y),
            double(Pieces[0].P3.Y - Pieces[0].P0.Y), double((Pieces[0].P0 - Baseline[0].P0).Size()), double((Pieces[0].P3 - Baseline[0].P3).Size()));
        if (Amount == 0.375f || Amount == 1.f)
        {
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture native zoom geometry"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const FString Name = FString::Printf(TEXT("Zoom-%s-%.3f.png"), Style, double(Amount));
                Test.TestTrue(TEXT("Save zoom capture"), FFileHelper::SaveArrayToFile(Png, *(Directory / Name)));
            }
        }
        if (++ZoomIndex < Panel->GetZoomLevels()->GetNumZoomLevels()) { SetZoomView(); return false; }
        if (!bZoomRounded)
        {
            bZoomRounded = true; ZoomIndex = 0;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); SetZoomView(); return false;
        }
        Test.TestTrue(TEXT("Save native and custom zoom evidence"), FFileHelper::SaveStringToFile(ZoomMetrics, *(Directory / TEXT("zoom.csv"))));
        Test.AddInfo(FString::Printf(TEXT("Checked %d supported zoom levels in both wire styles; native endpoint deltas and captures: %s"),
            Panel->GetZoomLevels()->GetNumZoomLevels(), *Directory));
        return Finish();
    }
    bool Start()
    {
        bInitialized = true; Deadline = FPlatformTime::Seconds() + 90;
        SourcePath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("../Templates/TP_AEC_ArchvisBP/Content/ArchvisProject/Blueprints") / (AssetName + TEXT(".uasset")));
        if (!Test.TestTrue(TEXT("Installed architectural spline template is available"), FFileHelper::LoadFileToArray(SourceBytes, *SourcePath))) { return true; }
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TemplateBlueprint") / Id) + TEXT("/");
        Mount = TEXT("/GlooPrintTemplate_") + Id + TEXT("/"); CopyPath = Directory / (AssetName + TEXT(".uasset"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        if (!Test.TestTrue(TEXT("Copy the authored asset into a private fixture directory"), FFileHelper::SaveArrayToFile(SourceBytes, *CopyPath))) { return true; }
        FPackageName::RegisterMountPoint(Mount, Directory); bMounted = true;
        const FString Name = Mount + AssetName; FLinkerInstancingContext Context(true);
        Context.AddPackageMapping(FName(*(TEXT("/Game/ArchvisProject/Blueprints/") + AssetName)), FName(*Name));
        Package.Reset(LoadPackage(nullptr, *Name, LOAD_None, nullptr, &Context));
        if (!Test.TestNotNull(TEXT("Native loader instances the copied package"), Package.Get())) { return Finish(); }
        Test.TestEqual(TEXT("Only the private package is loaded for formatting"), Package->GetName(), Name);
        Blueprint.Reset(Cast<UBlueprint>(Package->FindAssetInPackage()));
        if (!Test.TestNotNull(TEXT("Installed template loads as a real Blueprint"), Blueprint.Get()) || !Compile()) { return Finish(); }
        Graph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint.Get());
        if (!Test.TestTrue(TEXT("Authored construction graph is nontrivial"), Graph && Graph->Nodes.Num() > 4)) { return Finish(); }
        for (UEdGraphNode* Node : Graph->Nodes) { if (Node->IsA<UK2Node_FunctionEntry>()) { Entry = Node; break; } }
        if (!Test.TestNotNull(TEXT("Construction script retains its entry node"), Entry)) { return Finish(); }
        PrimeTooltips();
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
        bOriginalEnabled = Settings->bFormattingEnabled; bRestoreSettings = true;
        Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
        Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
        Editor = SNew(SGraphEditor).GraphToEdit(Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint authored ") + AssetName)).ClientSize(FVector2f(1550, 1000))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef()); Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false);
        GLog->AddOutputDevice(this); bListening = true;
        Test.AddInfo(TEXT("Private authored template artifacts: ") + Directory); return false;
    }
    bool Compile()
    {
        FCompilerResultsLog Result; FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Result);
        return Test.TestEqual(TEXT("The real spline Blueprint compiles without errors"), Result.NumErrors, 0);
    }
    void PrimeTooltips()
    {
        for (UEdGraphNode* Node : Graph->Nodes) { for (auto* Pin : Node->Pins) { FString Text; Node->GetPinHoverText(*Pin, Text); } }
    }
    void BeginFormat()
    {
        Changed = -1; bAwaitingFormat = true; LastMessage.Reset();
        auto& Slate = FSlateApplication::Get(); Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        Test.TestTrue(TEXT("Focused F is handled for the real template"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
        Frames = 0;
    }
    void CheckContext()
    {
        Test.TestEqual(TEXT("F preserves the camera offset at completion"), CompletedView, View);
        Test.TestEqual(TEXT("F preserves zoom at completion"), CompletedZoom, Zoom);
        Test.TestTrue(TEXT("F preserves the selected entry anchor"), Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Entry) &&
            FIntPoint(Entry->NodePosX, Entry->NodePosY) == Anchor);
    }
    void CheckProperties()
    {
        const auto Current = DescribeNodes(*Graph); Test.TestEqual(TEXT("All authored property identities remain"), Current.Num(), Properties.Num());
        for (const auto& Pair : Properties)
        {
            if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
            bool bCommentSize = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->IsA<UEdGraphNode_Comment>() && (Pair.Key == Node->GetName() + TEXT(".NodeWidth") || Pair.Key == Node->GetName() + TEXT(".NodeHeight"))) { bCommentSize = true; break; }
            }
            if (!bCommentSize) { const auto* Value = Current.Find(Pair.Key); Test.TestTrue(Pair.Key + TEXT(" survives formatting"), Value && *Value == Pair.Value); }
        }
    }
    void CheckRoutes(const FFormatPlan& Cold, const FRouteSet& Actual)
    {
        Test.TestEqual(TEXT("Formatted template retains all original connections"), Actual.Wires.Num(), Plan.Snapshot.Edges.Num());
        Test.TestEqual(TEXT("Formatted template has no native fallback"), Actual.FallbackCount, 0);
        for (const auto& Pair : Cold.Routes.Wires)
        {
            const auto* Route = Actual.Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Authored pin pairs keep exact cold and reordered paths"), Route && Route->Points == Pair.Value.Points);
        }
    }
    void CheckPaintedRoutes(const TCHAR* Stage, const FRouteSet& Routes, bool bFormatted = true)
    {
        FString Curves = TEXT("policy\tfrom_node\tfrom_pin\tto_node\tto_pin\tfrom_label\tto_label\tcached_fallback\tpiece\tp0x\tp0y\tp1x\tp1y\tp2x\tp2y\tp3x\tp3y\n");
        auto* Panel = Editor->GetGraphPanel(); FArrangedChildren Nodes(EVisibility::Visible);
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Authored node has native painted geometry"), Widget.IsValid())) { return; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const FVector2f Origin = Nodes[0].Geometry.GetAbsolutePosition() - FVector2f(Graph->Nodes[0]->NodePosX, Graph->Nodes[0]->NodePosY) * Scale;
        if (!Test.TestTrue(TEXT("Drawn-curve export has a finite positive graph scale"), FMath::IsFinite(Scale) && Scale > 0)) { return; }
        int32 Connections = 0;
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        for (UEdGraphNode* Node : Graph->Nodes)
        for (UEdGraphPin* Output : Node->Pins)
        {
            if (Output->Direction != EGPD_Output) { continue; }
            for (UEdGraphPin* Input : Output->LinkedTo)
            {
                TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
                for (UEdGraphPin* Pin : {Output, Input})
                {
                    const auto Widget = Panel->GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid)->FindWidgetForPin(Pin);
                    if (!Test.TestTrue(TEXT("Authored connection retains its native pin widgets"), Widget.IsValid())) { return; }
                    Pins.Add(Widget.ToSharedRef(), FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
                }
                FSlateWindowElementList NativeElements(Window), CustomElements(Window);
                FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Graph);
                TUniquePtr<FConnectionDrawingPolicy> Custom(FNodeFactory::CreateConnectionPolicy(Graph->GetSchema(), 0, 1, Scale, Clip, CustomElements, Graph));
                if (!Test.TestTrue(TEXT("Authored graph uses the custom drawing policy"), Custom.IsValid())) { return; }
                Native.SetAbsoluteMousePosition(FVector2f(-100000)); Custom->SetAbsoluteMousePosition(FVector2f(-100000));
                Native.Draw(Pins, Nodes); Custom->Draw(Pins, Nodes);
                const auto& Baseline = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                const auto& Pieces = CustomElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                const auto* Route = Routes.Wires.Find({Node->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId});
                if (!Test.TestTrue(TEXT("Painted authored connection retains its cached route"), Route != nullptr) ||
                    !Test.TestEqual(TEXT("Native geometry identifies one original connection"), Baseline.Num(), 1) ||
                    !Test.TestTrue(TEXT("Custom drawing keeps the authored connection visible"), !Pieces.IsEmpty())) { continue; }
                ++Connections;
                const auto Export = [&](const TCHAR* Policy, const auto& Drawn)
                {
                    for (int32 I = 0; I < Drawn.Num(); ++I)
                    {
                        const auto& Curve = Drawn[I];
                        Curves += FString::Printf(TEXT("%s\t%s\t%s\t%s\t%s\t%s.%s\t%s.%s\t%d\t%d"), Policy,
                            *Node->NodeGuid.ToString(), *Output->PinId.ToString(), *Input->GetOwningNode()->NodeGuid.ToString(), *Input->PinId.ToString(),
                            *Node->GetName(), *Output->PinName.ToString(), *Input->GetOwningNode()->GetName(), *Input->PinName.ToString(), int32(Route->Fallback), I);
                        for (const FVector2f Point : {Curve.P0, Curve.P1, Curve.P2, Curve.P3})
                        {
                            const FVector2f P = (Point - Origin) / Scale;
                            Test.TestTrue(TEXT("Every exported drawn control point is finite"), FMath::IsFinite(P.X) && FMath::IsFinite(P.Y));
                            Curves += FString::Printf(TEXT("\t%.9g\t%.9g"), double(P.X), double(P.Y));
                        }
                        Curves += TEXT("\n");
                    }
                };
                Export(TEXT("native"), Baseline); Export(TEXT("glooprint"), Pieces);
                Test.TestTrue(TEXT("Styled authored wires stay attached to the native pins"),
                    Pieces[0].P0.Equals(Baseline[0].P0, 0.1f) && Pieces.Last().P3.Equals(Baseline[0].P3, 0.1f));
                if (!bFormatted) { continue; }
                if (Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    Test.TestEqual(TEXT("Authored execution chain draws without a comment detour"), Pieces.Num(), 1);
                    Test.TestTrue(TEXT("Authored execution stays horizontal at the actual native pins"),
                        FMath::IsNearlyEqual(Pieces[0].P0.Y, Pieces.Last().P3.Y, 0.1f));
                }
                if (!Test.TestEqual(TEXT("Live authored geometry draws every cached custom piece"), Pieces.Num(), Route->Curves.Num()))
                {
                    if (Route->Points.IsEmpty())
                    {
                        Test.AddInfo(FString::Printf(TEXT("Cached fallback %d for %s.%s -> %s.%s"), int32(Route->Fallback),
                            *Node->GetName(), *Output->PinName.ToString(), *Input->GetOwningNode()->GetName(), *Input->PinName.ToString()));
                        continue;
                    }
                    const FVector2f Start = (Baseline[0].P0 + FVector2f(4, 0) - Origin) / Scale;
                    const FVector2f End = (Baseline[0].P3 - FVector2f(4, 0) - Origin) / Scale;
                    Test.AddInfo(FString::Printf(TEXT("Paint fallback %s.%s -> %s.%s; live %s -> %s; cached %s -> %s; start region %s..%s; end region %s..%s"),
                        *Node->GetName(), *Output->PinName.ToString(), *Input->GetOwningNode()->GetName(), *Input->PinName.ToString(),
                        *Start.ToString(), *End.ToString(), *Route->Points[0].ToString(), *Route->Points.Last().ToString(),
                        *Route->StartRegion.Min.ToString(), *Route->StartRegion.Max.ToString(), *Route->EndRegion.Min.ToString(), *Route->EndRegion.Max.ToString()));
                }
            }
        }
        Test.TestEqual(TEXT("Drawn-curve export includes every original connection"), Connections, Plan.Snapshot.Edges.Num());
        Test.TestTrue(TEXT("Save complete native and styled curve controls"),
            FFileHelper::SaveStringToFile(Curves, *(Directory / (FString(Stage) + TEXT("-drawn-curves.tsv")))));
    }
    void RecordMetrics(const TCHAR* Stage, const FLayoutGraph& Snapshot, const FRouteSet& Routes)
    {
        FString Details = TEXT("node\tobject\tlabel\tx\ty\twidth\theight\tcomment\n");
        TMap<FGuid, UEdGraphNode*> NativeNodes;
        for (UEdGraphNode* Node : Graph->Nodes) { NativeNodes.Add(Node->NodeGuid, Node); }
        for (int32 I = 0; I < Snapshot.Nodes.Num(); ++I)
        {
            const auto& Node = Snapshot.Nodes[I]; const auto* Native = NativeNodes.FindChecked(Node.Geometry.Id);
            FString Label = Native->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\t"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" | "));
            Details += FString::Printf(TEXT("%d\t%s\t%s\t%d\t%d\t%.3f\t%.3f\t%d\n"), I, *Native->GetName(), *Label,
                Node.Geometry.Position.X, Node.Geometry.Position.Y, Node.Geometry.BodySize.X, Node.Geometry.BodySize.Y, Node.bComment);
        }
        Test.TestTrue(TEXT("Save native template node diagnostics"), FFileHelper::SaveStringToFile(Details, *(Directory / (FString(Stage) + TEXT("-nodes.tsv")))));
        Details = TEXT("edge\tkind\tfrom_node\tfrom_pin\tto_node\tto_pin\tfrom_x\tfrom_y\tto_x\tto_y\n");
        for (int32 I = 0; I < Snapshot.Edges.Num(); ++I)
        {
            const auto& Edge = Snapshot.Edges[I]; const auto& A = Snapshot.Pins[Edge.From]; const auto& B = Snapshot.Pins[Edge.To];
            const auto* From = NativeNodes.FindChecked(Snapshot.Nodes[A.Node].Geometry.Id);
            const auto* To = NativeNodes.FindChecked(Snapshot.Nodes[B.Node].Geometry.Id);
            const FVector2f Start = FVector2f(Snapshot.Nodes[A.Node].Geometry.Position) + A.Offset.GetValue();
            const FVector2f End = FVector2f(Snapshot.Nodes[B.Node].Geometry.Position) + B.Offset.GetValue();
            Details += FString::Printf(TEXT("%d\t%d\t%d\t%s\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f\n"), I, int32(Edge.Kind), A.Node,
                *From->Pins[A.Ordinal]->PinName.ToString(), B.Node, *To->Pins[B.Ordinal]->PinName.ToString(), Start.X, Start.Y, End.X, End.Y);
        }
        Test.TestTrue(TEXT("Save native template pin diagnostics"), FFileHelper::SaveStringToFile(Details, *(Directory / (FString(Stage) + TEXT("-edges.tsv")))));
        FBox2f Nodes(ForceInit), Envelope(ForceInit); int32 Bends = 0, Execution = 0, Aligned = 0; double Length = 0;
        for (const auto& Node : Snapshot.Nodes) { Nodes += FVector2f(Node.Geometry.Position); Nodes += FVector2f(Node.Geometry.Position) + Node.Geometry.BodySize; }
        Envelope = Nodes;
        for (const auto& Pair : Routes.Wires)
        {
            const auto& R = Pair.Value; Bends += FMath::Max(0, R.Points.Num() - 2); Length += R.Length;
            if (!R.Points.IsEmpty()) { Envelope += R.Bounds.Min; Envelope += R.Bounds.Max; }
        }
        for (const auto& Edge : Snapshot.Edges)
        {
            if (Edge.Kind != ELinkKind::Execution) { continue; } ++Execution;
            const auto& A = Snapshot.Pins[Edge.From]; const auto& B = Snapshot.Pins[Edge.To];
            if (A.Offset.IsSet() && B.Offset.IsSet() && Snapshot.Nodes[A.Node].Geometry.Position.Y + A.Offset->Y == Snapshot.Nodes[B.Node].Geometry.Position.Y + B.Offset->Y) { ++Aligned; }
        }
        Test.TestEqual(TEXT("Authored spline execution chain stays exactly aligned"), Aligned, Execution);
        const FVector2f N = Nodes.Max - Nodes.Min, E = Envelope.Max - Envelope.Min;
        const FString Row = FString::Printf(TEXT("%s,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n"), Stage, Snapshot.Nodes.Num(), Routes.Wires.Num(), Routes.FallbackCount,
            Bends, Aligned, Execution, Length, N.X, N.Y, E.X, E.Y);
        Metrics += Row; Test.AddInfo(Row.TrimEnd());
    }
    void Capture(const TCHAR* Stage)
    {
        FVector2f CaptureView; float CaptureZoom; Editor->GetViewLocation(CaptureView, CaptureZoom);
        const FVector2f TopLeft = (CaptureBounds.Min - CaptureView) * CaptureZoom;
        const FVector2f BottomRight = (CaptureBounds.Max - CaptureView) * CaptureZoom;
        const FVector2f Available = Editor->GetGraphPanel()->GetCachedGeometry().GetLocalSize();
        Test.TestTrue(TEXT("Native capture includes the complete authored and formatted envelopes"),
            TopLeft.X >= 0 && TopLeft.Y >= 0 && BottomRight.X <= Available.X && BottomRight.Y <= Available.Y);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture the actual authored Blueprint viewport"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save authored Blueprint viewport"), FFileHelper::SaveArrayToFile(Png, *(Directory / (FString(Stage) + TEXT(".png")))));
        }
    }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        bAwaitingFormat = false;
        if (bListening) { GLog->RemoveOutputDevice(this); bListening = false; }
        if (Window) { Window->RequestDestroyWindow(); } Window.Reset(); Editor.Reset();
        if (Package.IsValid()) { Package->SetDirtyFlag(false); }
        if (bMounted) { FPackageName::UnRegisterMountPoint(Mount, Directory); bMounted = false; }
        if (bRestoreSettings)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalSettings.VerticalSpacing; Settings->CommentPadding = OriginalSettings.CommentPadding;
            Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged(); bRestoreSettings = false;
        }
    }
    FAutomationTestBase& Test;
    const FString AssetName;
    TStrongObjectPtr<UPackage> Package;
    TStrongObjectPtr<UBlueprint> Blueprint;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Entry = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FFormatPlan Plan;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TMap<FString, FString> Properties;
    TMap<UEdGraph*, TArray<uint8>> OtherGraphs;
    TArray<uint8> SourceBytes, Before, After, Defaults;
    FString SourcePath, CopyPath, Directory, Mount, LastMessage;
    FString Metrics = TEXT("stage,nodes,links,fallbacks,bends,aligned_execution,execution_links,route_length,node_width,node_height,envelope_width,envelope_height\n");
    FString ZoomMetrics = TEXT("style,zoom,draw_scale,lod,output_pin_local_y,input_pin_local_y,native_delta_y,custom_delta_y,start_attachment_error,end_attachment_error\n");
    UEdGraphPin* ZoomOutput = nullptr;
    UEdGraphPin* ZoomInput = nullptr;
    TArray<FVector2f> ZoomPoints;
    int32 ZoomIndex = 0, ZoomBuilds = 0;
    bool bZoomCase = false, bZoomRounded = false;
    FVector2f View, CompletedView;
    FBox2f CaptureBounds{ForceInit};
    FIntPoint Anchor;
    float Zoom = 0, CompletedZoom = 0;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Queue = 0, Changed = -1;
    bool bInitialized = false, bMounted = false, bRestoreSettings = false, bOriginalEnabled = true, bListening = false, bAwaitingFormat = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTemplateSplineMeshTest, "GlooPrint.Editor.TemplateSplineMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTemplateSplineMeshTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FTemplateBlueprintCheck(*this, TEXT("BP_Splinemesh"))); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTemplateSplineZoomTest, "GlooPrint.Editor.TemplateSplineZoom",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTemplateSplineZoomTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FTemplateBlueprintCheck(*this, TEXT("BP_Splinemesh"), true)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTemplateSplineSpawnTest, "GlooPrint.Editor.TemplateSplineSpawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTemplateSplineSpawnTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FTemplateBlueprintCheck(*this, TEXT("BP_SpawnMeshAlongSpline"))); return true;
}
}
#endif
