// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphEditorModule.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/SlateRenderer.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "GlooPrint"
DEFINE_LOG_CATEGORY_STATIC(LogGlooPrintEditor, Log, All);

namespace GlooPrint
{
namespace
{
void ReportFormatResult(bool bSuccess, int32 Changed, const FString& Reason)
{
    if (!Reason.IsEmpty())
    {
        UE_LOG(LogGlooPrintEditor, Display, TEXT("%s"), *Reason);
        FNotificationInfo Info(FText::FromString(Reason));
        Info.ExpireDuration = 5;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    if (bSuccess) { UE_LOG(LogGlooPrintEditor, Display, TEXT("Formatted graph: %d changed nodes."), Changed); }
}
}

bool ApplyLayout(UEdGraph* Graph, const FLayoutGraph& Snapshot, const FLayoutResult& Layout,
    int32& ChangedNodes, FString& OutReason)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_Apply);
    ChangedNodes = 0;
    if (!CanFormatGraph(Graph, OutReason)) { return false; }
    if (GEditor->IsTransactionActive())
    {
        OutReason = TEXT("Finish the current editor operation before formatting."); return false;
    }
    const int32 Count = Snapshot.Nodes.Num();
    if (Graph->Nodes.Num() != Count || Layout.Positions.Num() != Count || Layout.Sizes.Num() != Count)
    {
        OutReason = TEXT("The graph or layout changed before it could be applied."); return false;
    }
    TMap<FGuid, UEdGraphNode*> NodesById;
    NodesById.Reserve(Count);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node) || Node->GetGraph() != Graph || NodesById.Contains(Node->NodeGuid))
        {
            OutReason = TEXT("A node was removed or reconstructed before application."); return false;
        }
        NodesById.Add(Node->NodeGuid, Node);
    }
    TArray<UEdGraphNode*> Nodes;
    Nodes.Reserve(Count);
    TArray<UEdGraphPin*> Pins;
    Pins.Reserve(Snapshot.Pins.Num());
    TMap<const UEdGraphPin*, int32> PinIndices;
    PinIndices.Reserve(Snapshot.Pins.Num());
    TArray<int32> Changed;
    for (int32 I = 0; I < Count; ++I)
    {
        const FLayoutNode& Original = Snapshot.Nodes[I];
        UEdGraphNode* const* Found = NodesById.Find(Original.Geometry.Id);
        UEdGraphNode* Node = Found ? *Found : nullptr;
        if (!Node || FIntPoint(Node->NodePosX, Node->NodePosY) != Original.Geometry.Position ||
            FIntPoint(Node->NodeWidth, Node->NodeHeight) != Original.OriginalSize || Node->Pins.Num() != Original.PinCount)
        {
            OutReason = TEXT("Node positions, bounds or pins changed before application."); return false;
        }
        Nodes.Add(Node);
        for (int32 P = 0; P < Node->Pins.Num(); ++P)
        {
            UEdGraphPin* Pin = Node->Pins[P];
            const FLayoutPin& Expected = Snapshot.Pins[Original.FirstPin + P];
            if (!Pin || Pin->bWasTrashed || Pin->GetOwningNode() != Node || Pin->PinId != Expected.Id ||
                (Pin->Direction == EGPD_Output) != Expected.bOutput)
            {
                OutReason = TEXT("A pin was reconstructed before application."); return false;
            }
            PinIndices.Add(Pin, Pins.Add(Pin));
        }
        if (!Original.bComment && Layout.Sizes[I] != Original.OriginalSize)
        {
            OutReason = TEXT("A layout attempted to resize an ordinary node."); return false;
        }
        if (Original.bComment && (Layout.Sizes[I].X <= 0 || Layout.Sizes[I].Y <= 0))
        {
            OutReason = TEXT("A comment has invalid proposed bounds."); return false;
        }
        if (Layout.Positions[I] != Original.Geometry.Position || Layout.Sizes[I] != Original.OriginalSize)
        {
            if (!Node->HasAnyFlags(RF_Transactional))
            {
                OutReason = TEXT("A changed node does not support editor undo transactions."); return false;
            }
            Changed.Add(I);
        }
    }
    TSet<uint64> ExpectedLinks;
    ExpectedLinks.Reserve(Snapshot.Edges.Num() * 2);
    for (const FLayoutEdge& Edge : Snapshot.Edges)
    {
        ExpectedLinks.Add((uint64(uint32(Edge.From)) << 32) | uint32(Edge.To));
        ExpectedLinks.Add((uint64(uint32(Edge.To)) << 32) | uint32(Edge.From));
    }
    for (int32 I = 0; I < Pins.Num(); ++I)
    {
        for (const UEdGraphPin* Link : Pins[I]->LinkedTo)
        {
            const int32* Other = PinIndices.Find(Link);
            if (!Other || !ExpectedLinks.Remove((uint64(uint32(I)) << 32) | uint32(*Other)))
            {
                OutReason = TEXT("Connections changed before application."); return false;
            }
        }
    }
    if (!ExpectedLinks.IsEmpty()) { OutReason = TEXT("Connections changed before application."); return false; }
    if (Changed.IsEmpty()) { return true; }
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ApplyTransaction);
        const FScopedTransaction Transaction(LOCTEXT("FormatTransaction", "Format Blueprint Graph"));
        for (const int32 I : Changed)
        {
            UEdGraphNode* Node = Nodes[I];
            Node->Modify();
            Node->NodePosX = Layout.Positions[I].X;
            Node->NodePosY = Layout.Positions[I].Y;
            if (Snapshot.Nodes[I].bComment)
            {
                Node->NodeWidth = Layout.Sizes[I].X;
                Node->NodeHeight = Layout.Sizes[I].Y;
            }
        }
    }
    Graph->NotifyGraphChanged();
    ChangedNodes = Changed.Num();
    return true;
}

struct FFormatJob::FState
{
    enum class EPhase : uint8 { Layout, Headers, Routes, Done };
    TWeakObjectPtr<UEdGraph> Graph;
    FFormatPlan Plan;
    FLayoutGraph Working;
    FLayoutSettings Settings;
    EGlooPrintWireStyle Style;
    float Scale;
    TMap<FGuid, TWeakObjectPtr<UEdGraphNode_Comment>> Comments;
    TMap<uint64, FMeasuredRect> Headers;
    TUniquePtr<FLayoutJob> LayoutJob;
    TUniquePtr<FRoutingJob> RoutingJob;
    FLayoutResult Layout;
    FString Reason, LastConstraint;
    EPhase Phase = EPhase::Layout;
    int32 Repair = 0, Pass = 0, HeaderIndex = 0;
    bool bChangedHeader = false, bHaveCandidate = false, bFailed = false, bNeedsRetry = false, bTaken = false;

    bool bRetainRouteSource;
    FState(UEdGraph* InGraph, FLayoutGraph Snapshot, float InScale, FLayoutSettings InSettings, EGlooPrintWireStyle InStyle, bool bInRetainRouteSource)
        : Graph(InGraph), Settings(InSettings), Style(InStyle == EGlooPrintWireStyle::Native ? EGlooPrintWireStyle::Rounded90 : InStyle), Scale(InScale),
          bRetainRouteSource(bInRetainRouteSource)
    {
        if (!IsValid(InGraph)) { Fail(TEXT("The graph closed before planning.")); return; }
        Plan.Snapshot = MoveTemp(Snapshot); Working = Plan.Snapshot;
        for (UEdGraphNode* Node : InGraph->Nodes)
        {
            if (auto* Comment = Cast<UEdGraphNode_Comment>(Node)) { Comments.Add(Comment->NodeGuid, Comment); }
        }
        for (int32 I = 0; I < Working.Nodes.Num(); ++I)
        {
            const auto& Node = Working.Nodes[I];
            if (Node.bComment && Node.Geometry.CommentHeader.IsSet())
            {
                Headers.Add((uint64(uint32(I)) << 32) | uint32(Node.OriginalSize.X), Node.Geometry.CommentHeader.GetValue());
            }
        }
    }
    void Fail(const FString& Error) { Reason = Error; bFailed = true; Phase = EPhase::Done; }
    void Finish()
    {
        if (!bHaveCandidate) { Fail(LastConstraint.IsEmpty() ? TEXT("No valid layout was found within the spacing-repair limit.") : LastConstraint); }
        else { Reason.Reset(); Phase = EPhase::Done; }
    }
    void NextRepair()
    {
        Pass = 0;
        if (++Repair > 2) { Finish(); }
        else { Phase = EPhase::Layout; }
    }
};

FFormatJob::FFormatJob(UEdGraph* Graph, FLayoutGraph Snapshot, float Scale, FLayoutSettings Settings, EGlooPrintWireStyle Style, bool bRetainRouteSource)
    : State(MakeUnique<FState>(Graph, MoveTemp(Snapshot), Scale, Settings, Style, bRetainRouteSource)) {}
FFormatJob::~FFormatJob() = default;

bool FFormatJob::Advance(double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_PlanFormat);
    auto& S = *State;
    do
    {
        if (S.Phase == FState::EPhase::Layout)
        {
            if (!S.LayoutJob)
            {
                FLayoutSettings Spacing = S.Settings;
                Spacing.HorizontalSpacing += S.Repair * 48.f; Spacing.VerticalSpacing += S.Repair * 24.f;
                S.LayoutJob = MakeUnique<FLayoutJob>(S.Working, Spacing); ++S.Plan.LayoutAttempts;
            }
            if (!S.LayoutJob->Advance(Deadline)) { return false; }
            ELayoutFailure Failure;
            const bool bSuccess = S.LayoutJob->TakeResult(S.Layout, S.Reason, &Failure);
            S.LayoutJob.Reset();
            if (!bSuccess)
            {
                if (Failure != ELayoutFailure::Constraints) { S.Fail(S.Reason); }
                else { S.LastConstraint = S.Reason; S.NextRepair(); }
            }
            else { S.HeaderIndex = 0; S.bChangedHeader = false; S.Phase = FState::EPhase::Headers; }
        }
        else if (S.Phase == FState::EPhase::Headers)
        {
            if (S.HeaderIndex < S.Working.Nodes.Num())
            {
                const int32 I = S.HeaderIndex++;
                auto& Node = S.Working.Nodes[I];
                if (Node.bComment)
                {
                    const uint64 Key = (uint64(uint32(I)) << 32) | uint32(S.Layout.Sizes[I].X);
                    FMeasuredRect Header;
                    if (const auto* Cached = S.Headers.Find(Key)) { Header = *Cached; }
                    else
                    {
                        const auto* Weak = S.Comments.Find(Node.Geometry.Id);
                        auto* Comment = Weak ? Weak->Get() : nullptr;
                        if (!S.Graph.IsValid() || !Comment || Comment->GetGraph() != S.Graph.Get() ||
                            !MeasureCommentHeader(Comment, S.Scale, S.Layout.Sizes[I].X, Header, S.Reason, &S.bNeedsRetry))
                        {
                            S.Fail(S.Reason.IsEmpty() ? TEXT("A comment changed while its proposed header was measured.") : S.Reason);
                            return true;
                        }
                        S.Headers.Add(Key, Header); ++S.Plan.CommentMeasurements;
                    }
                    if (!Node.Geometry.CommentHeader.IsSet() || Node.Geometry.CommentHeader.GetValue() != Header)
                    {
                        Node.Geometry.CommentHeader = Header; S.bChangedHeader = true;
                    }
                }
            }
            else if (S.bChangedHeader)
            {
                if (++S.Pass == 3) { S.Fail(TEXT("Comment title geometry did not stabilize after three proposed layouts.")); }
                else { S.Phase = FState::EPhase::Layout; }
            }
            else { S.Phase = FState::EPhase::Routes; }
        }
        else if (S.Phase == FState::EPhase::Routes)
        {
            if (!S.RoutingJob)
            {
                S.RoutingJob = CreateLayoutRoutingJob(S.Working, S.Layout, S.Reason, S.Style);
                if (!S.RoutingJob) { S.Fail(S.Reason); return true; }
            }
            if (!S.RoutingJob->Advance(Deadline)) { return false; }
            FRouteSet Routes;
            FLayoutGraph RouteSource;
            const bool bSuccess = S.RoutingJob->TakeResult(Routes, S.Reason, S.bRetainRouteSource ? &RouteSource : nullptr);
            S.RoutingJob.Reset();
            if (!bSuccess) { S.Fail(S.Reason); return true; }
            if (!S.bHaveCandidate || Routes.FallbackCount < S.Plan.Routes.FallbackCount)
            {
                S.bHaveCandidate = true; S.Plan.Layout = MoveTemp(S.Layout);
                S.Plan.Routes = MoveTemp(Routes); S.Plan.SpacingRepairs = S.Repair;
                S.Plan.RouteSource = MoveTemp(RouteSource);
            }
            if (S.Plan.Routes.FallbackCount == 0) { S.Finish(); }
            else { S.NextRepair(); }
        }
    }
    while (S.Phase != FState::EPhase::Done && FPlatformTime::Seconds() < Deadline);
    return S.Phase == FState::EPhase::Done;
}

bool FFormatJob::TakePlan(FFormatPlan& OutPlan, FString& OutReason, bool* OutNeedsLayoutRetry)
{
    OutPlan = {}; OutReason = State->Reason;
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = State->bNeedsRetry; }
    if (State->Phase != FState::EPhase::Done || State->bTaken)
    {
        OutReason = TEXT("Format plan is not available."); return false;
    }
    if (State->bFailed) { return false; }
    State->bTaken = true; OutPlan = MoveTemp(State->Plan); return true;
}

bool PlanFormatGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection, FFormatPlan& OutPlan,
    FString& OutReason, const FMeasurementOptions& MeasurementOptions, bool* OutNeedsLayoutRetry)
{
    OutPlan = {}; OutReason.Reset();
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = false; }
    const auto* Settings = GetDefault<UGlooPrintSettings>();
    if (!Settings->bFormattingEnabled)
    {
        OutReason = TEXT("GlooPrint formatting is disabled in Editor Preferences."); return false;
    }
    FLayoutGraph Snapshot;
    if (!CaptureGraph(Graph, LayoutScale, Selection, Snapshot, OutReason, MeasurementOptions, OutNeedsLayoutRetry)) { return false; }
    FFormatJob Job(Graph, MoveTemp(Snapshot), LayoutScale, Settings->GetLayoutSettings(), Settings->GetWireStyle());
    Job.Advance(TNumericLimits<double>::Max());
    return Job.TakePlan(OutPlan, OutReason, OutNeedsLayoutRetry);
}

static bool ApplyFormatPlan(UEdGraph* Graph, const FFormatPlan& Plan, int32& ChangedNodes, FString& OutReason,
    FMeasurementCache* MeasurementCache = nullptr)
{
    const TSharedPtr<FMeasurementCache> Cache = MeasurementCache ? MeasurementCache->AsShared() : TSharedPtr<FMeasurementCache>();
    auto Reuse = Cache ? Cache->PrepareLayoutReuse(Plan.Snapshot, Plan.Layout) : FMeasurementCache::FLayoutReuse();
    if (!ApplyLayout(Graph, Plan.Snapshot, Plan.Layout, ChangedNodes, OutReason)) { return false; }
    if (Cache && ChangedNodes > 0) { Cache->RestoreLayoutReuse(MoveTemp(Reuse)); }
    if (Plan.Layout.bLimitedComments) { OutReason = TEXT("Overlapping comment regions kept their internal arrangement."); }
    if (Plan.Routes.FallbackCount > 0 && GetDefault<UGlooPrintSettings>()->GetWireStyle() != EGlooPrintWireStyle::Native)
    {
        if (!OutReason.IsEmpty()) { OutReason += TEXT(" "); }
        OutReason += FString::Printf(TEXT("%d connections use native wires where clear custom routes were unavailable."), Plan.Routes.FallbackCount);
    }
    return true;
}

bool FormatGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection,
    int32& ChangedNodes, FString& OutReason, const FMeasurementOptions& MeasurementOptions, bool* OutNeedsLayoutRetry)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_FormatGraph);
    ChangedNodes = 0;
    FFormatPlan Plan;
    if (!PlanFormatGraph(Graph, LayoutScale, Selection, Plan, OutReason, MeasurementOptions, OutNeedsLayoutRetry)) { return false; }
    return ApplyFormatPlan(Graph, Plan, ChangedNodes, OutReason, MeasurementOptions.Cache);
}

class FCommands final : public TCommands<FCommands>
{
public:
    FCommands() : TCommands(TEXT("GlooPrint"), LOCTEXT("CommandContext", "GlooPrint"), NAME_None, FAppStyle::GetAppStyleSetName()) {}
    virtual void RegisterCommands() override
    {
        UI_COMMAND(Format, "Format Graph", "Format the entire active Blueprint graph; selection chooses the stationary anchor.",
            EUserInterfaceActionType::Button, FInputChord(EKeys::F));
    }
    TSharedPtr<FUICommandInfo> Format;
};

void FEditor::Initialize()
{
    FCommands::Register();
    CommandList = MakeShared<FUICommandList>();
    CommandList->MapAction(FCommands::Get().Format, FExecuteAction::CreateSP(this, &FEditor::ExecuteFocused),
        FCanExecuteAction::CreateLambda([] { return GetDefault<UGlooPrintSettings>()->bFormattingEnabled; }));
    auto& GraphEditor = FModuleManager::LoadModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
    auto Extender = FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode::CreateSP(this, &FEditor::ExtendMenu);
    MenuHandle = Extender.GetHandle();
    GraphEditor.GetAllGraphEditorContextMenuExtender().Add(Extender);
    FSlateApplication::Get().RegisterInputPreProcessor(AsShared());
    InvalidateWidgetsHandle = FSlateApplication::Get().OnInvalidateAllWidgets().AddSP(this, &FEditor::OnInvalidateWidgets);
    PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &FEditor::OnPropertyChanged);
    const auto CurrentFontCache = FSlateApplication::Get().GetRenderer()->GetFontCache();
    FontCache = CurrentFontCache;
    FontResourcesHandle = CurrentFontCache->OnReleaseResources().AddSP(this, &FEditor::OnFontResourcesReleased);
}

void FEditor::Shutdown()
{
    CancelPending();
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().UnregisterInputPreProcessor(AsShared());
        FSlateApplication::Get().OnInvalidateAllWidgets().Remove(InvalidateWidgetsHandle);
    }
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
    if (const auto Fonts = FontCache.Pin()) { Fonts->OnReleaseResources().Remove(FontResourcesHandle); }
    for (const auto& WeakPanel : CachedPanels)
    {
        if (const auto Panel = WeakPanel.Pin())
        {
            if (const auto Cache = Panel->GetMetaData<FMeasurementCache>()) { Panel->RemoveMetaData(Cache.ToSharedRef()); }
        }
    }
    CachedPanels.Reset(); FontCache.Reset();
    if (auto* GraphEditor = FModuleManager::GetModulePtr<FGraphEditorModule>(TEXT("GraphEditor")))
    {
        GraphEditor->GetAllGraphEditorContextMenuExtender().RemoveAll([this](const auto& Delegate) { return Delegate.GetHandle() == MenuHandle; });
    }
    CurrentPanel.Reset(); CommandList.Reset();
    FCommands::Unregister();
}

void FEditor::InvalidateMeasurements()
{
    for (const auto& WeakPanel : CachedPanels)
    {
        if (const auto Panel = WeakPanel.Pin())
        {
            if (const auto Cache = Panel->GetMetaData<FMeasurementCache>()) { Cache->Invalidate(); }
        }
    }
}

void FEditor::OnInvalidateWidgets(bool bClearResources) { InvalidateMeasurements(); }
void FEditor::OnPropertyChanged(UObject* Object, FPropertyChangedEvent& Event) { InvalidateMeasurements(); }
void FEditor::OnFontResourcesReleased(const FSlateFontCache& InFontCache) { InvalidateMeasurements(); }

bool FEditor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event)
{
    if (Pending && Event.GetKey() == EKeys::Escape)
    {
        CancelPending();
        return false;
    }
    if (!GetDefault<UGlooPrintSettings>()->bFormattingEnabled) { return false; }
    const TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
    if (!Focused || Focused->GetType() != TEXT("SGraphPanel") || !SlateApp.IsNormalExecution()) { return false; }
    const auto Panel = StaticCastSharedPtr<SGraphPanel>(Focused);
    if (!Panel->GetGraphObj() || !Panel->GetGraphObj()->GetSchema() ||
        Panel->GetGraphObj()->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass()) { return false; }
    const FInputChord Chord(Event.GetKey(), Event.IsShiftDown(), Event.IsControlDown(), Event.IsAltDown(), Event.IsCommandDown());
    if (!FCommands::Get().Format->HasActiveChord(Chord)) { return false; }
    if (Event.IsRepeat()) { return true; }
    CurrentPanel = Panel;
    const bool Handled = CommandList->ProcessCommandBindings(Event);
    CurrentPanel.Reset();
    return Handled;
}

void FEditor::ExecuteFocused() { ExecutePanel(CurrentPanel); }

void FEditor::OnSettingsChanged() { CancelPending(); }

void FEditor::CloseProgress()
{
    if (FSlateApplication::IsInitialized())
    {
        if (const auto Item = Progress.Pin()) { Item->ExpireAndFadeout(); }
    }
    Progress.Reset();
}

void FEditor::CancelPending()
{
    ++RequestGeneration; Pending.Reset(); CloseProgress();
}

void FEditor::ShowProgress()
{
    if (!Pending || Progress.IsValid() || FPlatformTime::Seconds() - Pending->StartedAt < 0.25) { return; }
    FNotificationInfo Info(LOCTEXT("FormattingProgress", "Formatting Blueprint graph…"));
    if (const auto Panel = Pending->Panel.Pin()) { Info.ForWindow = FSlateApplication::Get().FindWidgetWindow(Panel.ToSharedRef()); }
    Info.bFireAndForget = false; Info.bUseThrobber = true;
    Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("CancelFormat", "Cancel"),
        LOCTEXT("CancelFormatTip", "Leave the graph unchanged."), FSimpleDelegate::CreateSP(this, &FEditor::CancelPending)));
    const auto Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (Item) { Item->SetCompletionState(SNotificationItem::CS_Pending); Progress = Item; }
}

void FEditor::ExecutePanel(TWeakPtr<SGraphPanel> WeakPanel)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_FormatDispatch);
    auto& Slate = FSlateApplication::Get();
    if (Pending && Pending->Panel == WeakPanel && IsPendingCurrent(*Pending, Slate)) { return; }
    CancelPending();
    const double StartedAt = FPlatformTime::Seconds();
    const auto Panel = WeakPanel.Pin();
    FString Reason;
    if (!GetDefault<UGlooPrintSettings>()->bFormattingEnabled) { Reason = TEXT("GlooPrint formatting is disabled in Editor Preferences."); }
    else if (!Panel || !Panel->IsGraphEditable()) { Reason = TEXT("The graph is closed or read-only."); }
    else
    {
        const auto Window = Slate.FindWidgetWindow(Panel.ToSharedRef());
        if (!Window) { Reason = TEXT("The graph window is unavailable."); }
        else
        {
            Slate.SetKeyboardFocus(Panel, EFocusCause::SetDirectly);
            if (!CanFormatGraph(Panel->GetGraphObj(), Reason) || !ValidateMeasurementGraph(Panel->GetGraphObj(), Reason))
            {
                ReportFormatResult(false, 0, Reason); return;
            }
            auto Cache = Panel->GetMetaData<FMeasurementCache>();
            if (!Cache)
            {
                Cache = MakeShared<FMeasurementCache>(); Panel->AddMetadata(Cache.ToSharedRef());
            }
            CachedPanels.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); }); CachedPanels.AddUnique(Panel);
            FPendingFormat Request;
            Request.Panel = Panel; Request.Graph = Panel->GetGraphObj(); Request.Cache = Cache;
            Request.Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            Request.Visibility = Panel->GetPinVisibility(); Request.StartedAt = StartedAt;
            Request.Generation = RequestGeneration;
            Request.Settings = GetDefault<UGlooPrintSettings>()->GetLayoutSettings();
            Request.Style = GetDefault<UGlooPrintSettings>()->GetWireStyle();
            for (const UEdGraphNode* Node : Panel->GetSelectedGraphNodes()) { Request.Selection.Add(Node->NodeGuid); }
            Cache->Begin(Request.Graph.Get(), Request.Scale, Request.Visibility);
            Request.Revision = Cache->GetRevision();
            Request.Nodes.Reserve(Request.Graph->Nodes.Num());
            for (UEdGraphNode* Node : Request.Graph->Nodes) { Request.Nodes.Add({Node, CaptureMeasurementState(*Node)}); }
            ContinueRequest(MoveTemp(Request), StartedAt + 0.004);
            return;
        }
    }
    ReportFormatResult(false, 0, Reason);
}

void FEditor::ContinueRequest(FPendingFormat Request, double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_FormatContinuation);
    auto& Slate = FSlateApplication::Get();
    FString Reason;
    bool bSuccess = false, bFinished = true, bNeedsRetry = false;
    FFormatPlan Plan;
    if (!Request.Job)
    {
        if (!Request.Capture && Request.AttemptsMade > 0 && !IsPendingCurrent(Request, Slate))
        {
            CloseProgress(); ReportFormatResult(false, 0, TEXT("Formatting canceled because the graph, selection or display changed.")); return;
        }
        if (!Request.Capture)
        {
            const auto Cache = Request.Cache.Pin();
            FMeasurementOptions Options; Options.Cache = Cache.Get(); Options.PinVisibility = Request.Visibility;
            ++Request.AttemptsMade;
            Request.Capture = MakeUnique<FGraphCaptureJob>(Request.Graph.Get(), Request.Scale, Request.Selection, Options);
        }
        bFinished = Request.Capture->Advance(Deadline);
        if (bFinished)
        {
            FLayoutGraph Snapshot;
            if (Request.Capture->TakeResult(Snapshot, Reason, &bNeedsRetry))
            {
                Request.Job = MakeUnique<FFormatJob>(Request.Graph.Get(), MoveTemp(Snapshot), Request.Scale, Request.Settings, Request.Style,
                    Request.Style != EGlooPrintWireStyle::Native);
            }
            Request.Capture.Reset();
        }
    }
    if (Request.Job)
    {
        bFinished = Request.Job->Advance(Deadline);
        if (bFinished) { bSuccess = Request.Job->TakePlan(Plan, Reason, &bNeedsRetry); }
    }
    if (!IsPendingCurrent(Request, Slate, false))
    {
        CloseProgress(); ReportFormatResult(false, 0, TEXT("Formatting canceled because the graph, selection or display changed.")); return;
    }
    if (!bFinished || (!bSuccess && bNeedsRetry && Request.AttemptsMade < 4))
    {
        if (bFinished) { Request.Job.Reset(); }
        Request.LastAttemptFrame = GFrameCounter;
        Pending = MoveTemp(Request); ShowProgress(); return;
    }
    int32 Changed = 0;
    if (bSuccess)
    {
        if (!IsPendingCurrent(Request, Slate))
        {
            bSuccess = false; Reason = TEXT("Formatting canceled because the graph, selection or display changed.");
        }
        else
        {
            const auto Cache = Request.Cache.Pin();
            bSuccess = ApplyFormatPlan(Request.Graph.Get(), Plan, Changed, Reason, Cache.Get());
            if (bSuccess && Changed > 0)
            {
                const auto Panel = Request.Panel.Pin();
                const auto Routes = Panel ? Panel->GetMetaData<FRouteCache>() : nullptr;
                if (Routes && Routes->GetGraph() == Request.Graph.Get())
                {
                    Routes->StagePlannedRoutes(MoveTemp(Plan.RouteSource), MoveTemp(Plan.Routes), Request.Style);
                }
            }
        }
    }
    else if (bNeedsRetry) { Reason = TEXT("Geometry is still unavailable after three deferred attempts. ") + Reason; }
    CloseProgress(); ReportFormatResult(bSuccess, Changed, Reason);
}

bool FEditor::IsPendingCurrent(const FPendingFormat& Request, FSlateApplication& SlateApp, bool bValidateNodes) const
{
    const auto Panel = Request.Panel.Pin();
    const auto Cache = Request.Cache.Pin();
    UEdGraph* Graph = Request.Graph.Get();
    if (Request.Generation != RequestGeneration || !GetDefault<UGlooPrintSettings>()->bFormattingEnabled ||
        Request.Settings != GetDefault<UGlooPrintSettings>()->GetLayoutSettings() ||
        Request.Style != GetDefault<UGlooPrintSettings>()->GetWireStyle() ||
        !Panel || !Graph || !Cache || Panel->GetGraphObj() != Graph ||
        SlateApp.GetKeyboardFocusedWidget().Get() != Panel.Get()) { return false; }
    FString Reason;
    const auto Window = SlateApp.FindWidgetWindow(Panel.ToSharedRef());
    if (!Window || !Panel->IsGraphEditable() || !CanFormatGraph(Graph, Reason))
    {
        return false;
    }
    bool bSame = Cache->GetRevision() == Request.Revision && Panel->GetMetaData<FMeasurementCache>() == Cache &&
        Panel->GetPinVisibility() == Request.Visibility &&
        Window->GetDPIScaleFactor() * SlateApp.GetApplicationScale() == Request.Scale &&
        Graph->Nodes.Num() == Request.Nodes.Num() && Panel->GetSelectedGraphNodes().Num() == Request.Selection.Num();
    for (const UEdGraphNode* Node : Panel->GetSelectedGraphNodes()) { bSame &= IsValid(Node) && Request.Selection.Contains(Node->NodeGuid); }
    if (bSame && bValidateNodes) { bSame = ValidateMeasurementGraph(Graph, Reason); }
    if (bSame && bValidateNodes)
    {
        for (int32 I = 0; I < Request.Nodes.Num(); ++I)
        {
            UEdGraphNode* Node = Graph->Nodes[I];
            if (Request.Nodes[I].Node.Get() != Node || Request.Nodes[I].State != CaptureMeasurementState(*Node))
            {
                bSame = false; break;
            }
        }
    }
    return bSame;
}

void FEditor::Tick(float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor)
{
    if (!Pending || Pending->LastAttemptFrame == GFrameCounter) { return; }
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_FormatPendingTick);
    FPendingFormat Request = MoveTemp(Pending.GetValue()); Pending.Reset();
    if (!IsPendingCurrent(Request, SlateApp, false)) { CloseProgress(); return; }
    ContinueRequest(MoveTemp(Request), FPlatformTime::Seconds() + 0.004);
}


TSharedRef<FExtender> FEditor::ExtendMenu(const TSharedRef<FUICommandList> Commands, const UEdGraph* Graph,
    const UEdGraphNode* Node, const UEdGraphPin* Pin, bool bReadOnly)
{
    const TSharedRef<FExtender> Extender = MakeShared<FExtender>();
    if (bReadOnly || !FSlateApplication::IsInitialized() || !Graph || !Graph->GetSchema() ||
        Graph->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass()) { return Extender; }
    TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
    for (int32 Depth = 0; Focused && Depth < 128; ++Depth, Focused = Focused->GetParentWidget())
    {
        if (Focused->GetType() != TEXT("SGraphPanel")) { continue; }
        const auto Panel = StaticCastSharedPtr<SGraphPanel>(Focused);
        if (Panel->GetGraphObj() != Graph) { break; }
        const TWeakPtr<SGraphPanel> WeakPanel(Panel);
        const auto MenuCommands = MakeShared<FUICommandList>();
        MenuCommands->MapAction(FCommands::Get().Format, FExecuteAction::CreateSP(this, &FEditor::ExecutePanel, WeakPanel),
            FCanExecuteAction::CreateLambda([] { return GetDefault<UGlooPrintSettings>()->bFormattingEnabled; }));
        Extender->AddMenuExtension(Pin ? TEXT("EdGraphSchemaPinActions") : TEXT("EdGraphSchemaNodeActions"),
            EExtensionHook::After, MenuCommands, FMenuExtensionDelegate::CreateSP(this, &FEditor::BuildMenu));
        break;
    }
    return Extender;
}

void FEditor::BuildMenu(FMenuBuilder& Menu) { Menu.AddMenuEntry(FCommands::Get().Format); }
}

#undef LOCTEXT_NAMESPACE
