// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintMeasurement.h"
#include "GlooPrintMeasurementCache.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "NodeFactory.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Types/SlateAttributeMetaData.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"

namespace GlooPrint
{
namespace
{
constexpr int32 MaxWidgetsPerNode = 16384;

bool IsFinite(const FVector2f& Value)
{
    return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
}

void Include(FMeasuredRect& Bounds, const FVector2f& Min, const FVector2f& Max)
{
    Bounds.Min.X = FMath::Min(Bounds.Min.X, Min.X);
    Bounds.Min.Y = FMath::Min(Bounds.Min.Y, Min.Y);
    Bounds.Max.X = FMath::Max(Bounds.Max.X, Max.X);
    Bounds.Max.Y = FMath::Max(Bounds.Max.Y, Max.Y);
}

bool RefreshTextControls(const TSharedRef<SGraphNode>& Node, float LayoutScale)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNodeTextRefresh);
    TArray<TSharedRef<SWidget>> Pending { Node };
    int32 Visited = 0;
    while (!Pending.IsEmpty())
    {
        const TSharedRef<SWidget> Widget = Pending.Pop(EAllowShrinking::No);
        if (++Visited > MaxWidgetsPerNode)
        {
            return false;
        }
        if (Widget->GetType() == TEXT("SEditableText"))
        {
            Widget->Tick(
                FGeometry::MakeRoot(Widget->GetDesiredSize(), FSlateLayoutTransform(LayoutScale)),
                FSlateApplication::Get().GetCurrentTime(), 0.0f);
        }
        else if (Widget->GetType() == TEXT("SMultiLineEditableText"))
        {
            StaticCastSharedRef<SMultiLineEditableText>(Widget)->Refresh();
        }
        FChildren* Children = Widget->GetChildren();
        if (Visited + Pending.Num() + Children->Num() > MaxWidgetsPerNode)
        {
            return false;
        }
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            Pending.Add(Children->GetChildAt(Index));
        }
    }
    return true;
}

bool IsHiddenPin(const UEdGraphPin& Pin, SGraphEditor::EPinVisibility Visibility)
{
    if (!Pin.LinkedTo.IsEmpty()) { return false; }
    if (Pin.bHidden || (Pin.bAdvancedView && Pin.GetOwningNode()->AdvancedPinDisplay == ENodeAdvancedPins::Hidden)) { return true; }
    if (Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { return false; }
    if (Visibility == SGraphEditor::Pin_HideNoConnection) { return true; }
    if (Visibility == SGraphEditor::Pin_HideNoConnectionNoDefault)
    {
        const bool bSelf = Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Object && Pin.PinName == UEdGraphSchema_K2::PN_Self;
        return Pin.Direction != EGPD_Input || (Pin.DefaultValue.IsEmpty() && !Pin.DefaultObject && !bSelf);
    }
    return false;
}

struct FRestoreNodeWidgetReference
{
    TWeakObjectPtr<UEdGraphNode> Node;
    TWeakPtr<SGraphNode> Previous;
    explicit FRestoreNodeWidgetReference(UEdGraphNode& InNode) : Node(&InNode), Previous(InNode.DEPRECATED_NodeWidget) {}
    ~FRestoreNodeWidgetReference()
    {
        if (UEdGraphNode* Live = Node.Get()) { Live->DEPRECATED_NodeWidget = Previous; }
    }
};

bool MeasureNode(UEdGraphNode& Node, float LayoutScale, FMeasuredNode& Out, FString& Reason,
    const TSharedPtr<SGraphPanel>& MeasurementPanel, SGraphEditor::EPinVisibility Visibility, bool& bNeedsRetry,
    TOptional<int32> ProposedCommentWidth = {})
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNode);
    Out.Pins.Reserve(Node.Pins.Num());
    TMap<const UEdGraphPin*, int32> PinIndices;
    PinIndices.Reserve(Node.Pins.Num());
    TSet<FGuid> PinIds;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        if (!Pin || Pin->bWasTrashed || Pin->GetOwningNodeUnchecked() != &Node
            || !Pin->PinId.IsValid() || PinIds.Contains(Pin->PinId)
            || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
        {
            Reason = TEXT("The node has missing, reconstructed, or ambiguous pin identities.");
            return false;
        }
        PinIds.Add(Pin->PinId);
        PinIndices.Add(Pin, Out.Pins.Num());
        Out.Pins.Add({ Pin->PinId, {} });
    }


    FRestoreNodeWidgetReference RestoreWidgetReference(Node);
    TSharedPtr<SGraphNode> Widget;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNodeFactory);
        Widget = FNodeFactory::CreateNodeWidget(&Node);
    }
    if (!Widget || Widget->RequiresSecondPassLayout())
    {
        Reason = TEXT("The node requires an unavailable widget or dependent layout.");
        return false;
    }
    if (MeasurementPanel) { Widget->SetOwner(MeasurementPanel.ToSharedRef()); }
    if (ProposedCommentWidth.IsSet())
    {
        if (Widget->GetType() != TEXT("SGraphNodeComment"))
        {
            Reason = TEXT("The custom comment widget cannot measure a proposed title width."); return false;
        }
        TArray<TSharedRef<SWidget>> Pending{Widget.ToSharedRef()};
        TSharedPtr<SInlineEditableTextBlock> Title;
        for (int32 I = 0; I < Pending.Num(); ++I)
        {
            const auto Current = Pending[I];
            if (Current->GetType() == TEXT("SInlineEditableTextBlock"))
            {
                if (Title) { Reason = TEXT("The comment widget has an ambiguous title."); return false; }
                Title = StaticCastSharedRef<SInlineEditableTextBlock>(Current);
            }
            FChildren* Children = Current->GetChildren();
            if (Pending.Num() + Children->Num() > MaxWidgetsPerNode)
            {
                Reason = TEXT("The comment widget exceeds the measurement limit."); return false;
            }
            for (int32 C = 0; C < Children->Num(); ++C) { Pending.Add(Children->GetChildAt(C)); }
        }
        if (!Title) { Reason = TEXT("The native comment title widget is unavailable."); return false; }
        Title->SetWrapTextAt(float(ProposedCommentWidth.GetValue()) - 32.f);
    }
    FVector2f Size = FVector2f::ZeroVector;
    bool bSizeSettled = false;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNodePrepass);
        for (int32 Pass = 0; Pass < 4; ++Pass)
        {
            Widget->MarkPrepassAsDirty();
            Widget->SlatePrepass(LayoutScale);
            FSlateAttributeMetaData::UpdateAllAttributes(*Widget,
                FSlateAttributeMetaData::EInvalidationPermission::AllowInvalidationIfConstructed);
            const FVector2f NextSize(Widget->GetDesiredSize());
            if (Pass == 0 && !RefreshTextControls(Widget.ToSharedRef(), LayoutScale))
            {
                Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
                return false;
            }
            bSizeSettled = Pass > 0 && NextSize == Size;
            Size = NextSize;
            if (bSizeSettled)
            {
                break;
            }
        }
    }
    if (!bSizeSettled || !IsFinite(Size) || Size.X <= 0.0f || Size.Y <= 0.0f)
    {
        bNeedsRetry = IsFinite(Size);
        Reason = TEXT("The native node widget has no reliable desired size.");
        return false;
    }

    Out.Id = Node.NodeGuid;
    Out.Position = FIntPoint(Node.NodePosX, Node.NodePosY);
    Out.BodySize = Size;
    Out.VisualBounds = { FVector2f::ZeroVector, Size };

    TArray<TSharedRef<SWidget>> PinWidgets;
    TMap<const SWidget*, int32> WidgetPins;
    TArray<FArrangedWidget> Pending;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNodeArrange);
        Widget->GetPins(PinWidgets);
        WidgetPins.Reserve(PinWidgets.Num());
        for (const TSharedRef<SWidget>& PinWidget : PinWidgets)
        {
            const UEdGraphPin* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj();
            const int32* Index = PinIndices.Find(Pin);
            if (!Index)
            {
                Reason = TEXT("The native widget contains a pin outside this node snapshot.");
                return false;
            }
            WidgetPins.Add(&PinWidget.Get(), *Index);
        }

        Pending.Emplace(Widget.ToSharedRef(), FGeometry::MakeRoot(Size, FSlateLayoutTransform()));
        int32 Visited = 0;
        while (!Pending.IsEmpty())
        {
            const FArrangedWidget Current = Pending.Pop(EAllowShrinking::No);
            if (++Visited > MaxWidgetsPerNode)
            {
                Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
                return false;
            }
            const FVector2f LocalSize(Current.Geometry.GetLocalSize());
            const FVector2f Min(Current.Geometry.LocalToAbsolute(FVector2f::ZeroVector));
            const FVector2f Max(Current.Geometry.LocalToAbsolute(LocalSize));
            if (!IsFinite(Min) || !IsFinite(Max) || Max.X < Min.X || Max.Y < Min.Y)
            {
                Reason = TEXT("The native widget supplied invalid arranged geometry.");
                return false;
            }
            Include(Out.VisualBounds, Min, Max);
            if (const int32* Index = WidgetPins.Find(&Current.Widget.Get()))
            {
                if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
                {
                    bNeedsRetry = true;
                    Reason = TEXT("A visible pin has no reliable arranged geometry.");
                    return false;
                }
                const UEdGraphPin* Pin = Node.Pins[*Index];
                const FVector2f Point(Pin->Direction == EGPD_Output ? LocalSize.X : 0.0f,
                    LocalSize.Y * 0.5f);
                Out.Pins[*Index].AttachmentOffset = FVector2f(Current.Geometry.LocalToAbsolute(Point));
            }
            FArrangedChildren Children(EVisibility::Visible);
            Current.Widget->ArrangeChildren(Current.Geometry, Children, true);
            if (Visited + Pending.Num() + Children.Num() > MaxWidgetsPerNode)
            {
                Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
                return false;
            }
            for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
            {
                Pending.Add(Children[Index]);
            }
        }

        for (int32 Index = 0; Index < Node.Pins.Num(); ++Index)
        {
            const UEdGraphPin& Pin = *Node.Pins[Index];
            if (!IsHiddenPin(Pin, Visibility) && !Out.Pins[Index].AttachmentOffset.IsSet())
            {
                bNeedsRetry = true;
                Reason = FString::Printf(TEXT("Required pin '%s' has no arranged attachment."), *Pin.PinName.ToString());
                return false;
            }
        }

    }

    TArray<FOverlayBrushInfo> Brushes;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNodeOverlays);
        Widget->GetOverlayBrushes(false, Size, Brushes);
        for (const FOverlayBrushInfo& Overlay : Brushes)
        {
            if (Overlay.Brush)
            {
                const FVector2f Offset(Overlay.OverlayOffset);
                const FVector2f Envelope(Overlay.AnimationEnvelope);
                const FVector2f BrushSize(Overlay.Brush->ImageSize);
                if (!IsFinite(Offset) || !IsFinite(Envelope) || !IsFinite(BrushSize))
                {
                    Reason = TEXT("The node supplied invalid overlay bounds.");
                    return false;
                }
                const FVector2f Extent(FMath::Abs(Envelope.X), FMath::Abs(Envelope.Y));
                Include(Out.VisualBounds, Offset - Extent, Offset + BrushSize + Extent);
            }
        }
        for (const FOverlayWidgetInfo& Overlay : Widget->GetOverlayWidgets(false, Size))
        {
            if (Overlay.Widget)
            {
                Overlay.Widget->SlatePrepass(LayoutScale);
                if (Overlay.Widget->GetVisibility().IsVisible())
                {
                    const FVector2f Offset(Overlay.OverlayOffset);
                    const FVector2f OverlaySize(Overlay.Widget->GetDesiredSize());
                    if (!IsFinite(Offset) || !IsFinite(OverlaySize) || OverlaySize.X <= 0 || OverlaySize.Y <= 0)
                    {
                        bNeedsRetry = IsFinite(Offset) && IsFinite(OverlaySize);
                        Reason = TEXT("The node supplied unavailable overlay geometry.");
                        return false;
                    }
                    Include(Out.VisualBounds, Offset, Offset + OverlaySize);
                }
            }
        }
    }
    if (Node.IsA<UEdGraphNode_Comment>())
    {
        const FSlateRect Header = Widget->GetTitleRect();
        const FVector2f Origin(Out.Position);
        Out.CommentHeader = FMeasuredRect {
            FVector2f(Header.Left, Header.Top) - Origin,
            FVector2f(Header.Right, Header.Bottom) - Origin
        };
        const FMeasuredRect& Bounds = Out.CommentHeader.GetValue();
        if (!IsFinite(Bounds.Min) || !IsFinite(Bounds.Max)
            || Bounds.Max.X <= Bounds.Min.X || Bounds.Max.Y <= Bounds.Min.Y)
        {
            bNeedsRetry = IsFinite(Bounds.Min) && IsFinite(Bounds.Max);
            Reason = TEXT("The comment has no reliable header geometry.");
            return false;
        }
    }
    return true;
}
}

bool ValidateMeasurementGraph(UEdGraph* Graph, FString& OutReason)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ValidateMeasurementGraph);
    OutReason.Reset();
    if (!IsInGameThread() || !FSlateApplication::IsInitialized())
    {
        OutReason = TEXT("Measurement requires the editor thread and initialized Slate.");
        return false;
    }
    if (!IsValid(Graph) || !Graph->GetSchema() || Graph->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass())
    {
        OutReason = TEXT("Measurement supports standard K2 Blueprint graphs only.");
        return false;
    }
    const UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
    if (!Blueprint || Blueprint->bBeingCompiled || Blueprint->bIsRegeneratingOnLoad)
    {
        OutReason = TEXT("A stable Blueprint owner is required; compilation or reconstruction is active.");
        return false;
    }

    TSet<FGuid> NodeIds, PinIds;
    NodeIds.Reserve(Graph->Nodes.Num());
    TSet<const UEdGraphPin*> ReferencedPins;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node) || Node->GetGraph() != Graph || !Node->NodeGuid.IsValid())
        {
            OutReason = TEXT("The graph has missing, reconstructed, or ambiguous node identities.");
            return false;
        }
        bool bDuplicate = false;
        NodeIds.Add(Node->NodeGuid, &bDuplicate);
        if (bDuplicate) { OutReason = TEXT("The graph has missing, reconstructed, or ambiguous node identities."); return false; }
        PinIds.Reset(); PinIds.Reserve(Node->Pins.Num());
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bWasTrashed || Pin->GetOwningNodeUnchecked() != Node || !Pin->PinId.IsValid() ||
                (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
            {
                OutReason = TEXT("A node contains invalid pin identities."); return false;
            }
            PinIds.Add(Pin->PinId, &bDuplicate);
            if (bDuplicate) { OutReason = TEXT("A node contains invalid pin identities."); return false; }
            for (const UEdGraphPin* Link : Pin->LinkedTo) { ReferencedPins.Add(Link); }
        }
    }
    if (ReferencedPins.IsEmpty()) { return true; }
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            ReferencedPins.Remove(Pin);
            if (ReferencedPins.IsEmpty()) { return true; }
        }
    }
    OutReason = TEXT("A connection has a missing or external endpoint."); return false;
}

struct FMeasurementJob::FState
{
    struct FNode
    {
        TWeakObjectPtr<UEdGraphNode> Node;
        TArray<uint8> Signature;
    };
    TWeakObjectPtr<UEdGraph> Graph;
    TWeakPtr<FMeasurementCache> Cache;
    TArray<FNode> Nodes;
    TArray<int32> PendingNodes;
    FGraphMeasurement Result;
    FString Reason;
    SGraphEditor::EPinVisibility Visibility;
    float Scale;
    uint64 Revision = 0;
    int32 Next = 0;
    bool bHasCache = false, bStarted = false, bDone = false, bTaken = false, bNeedsRetry = false;
};

FMeasurementJob::FMeasurementJob(UEdGraph* Graph, float LayoutScale, const FMeasurementOptions& Options)
    : State(MakeUnique<FState>())
{
    State->Graph = Graph; State->Scale = LayoutScale; State->Visibility = Options.PinVisibility;
    State->bHasCache = Options.Cache != nullptr;
    if (Options.Cache) { State->Cache = Options.Cache->AsShared(); }
}
FMeasurementJob::~FMeasurementJob() = default;

bool FMeasurementJob::Advance(double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureGraph);
    auto& S = *State;
    if (S.bDone) { return true; }
    const auto Fail = [&S](const TCHAR* Reason) { S.Reason = Reason; S.bDone = true; return true; };
    UEdGraph* Graph = S.Graph.Get();
    const double ValidationStarted = FPlatformTime::Seconds();
    if (!ValidateMeasurementGraph(Graph, S.Reason)) { S.bDone = true; return true; }
    Deadline += FPlatformTime::Seconds() - ValidationStarted;
    const auto Cache = S.Cache.Pin();
    if (S.bHasCache && !Cache) { return Fail(TEXT("The measurement cache was closed.")); }
    if (!S.bStarted)
    {
        if (!FMath::IsFinite(S.Scale) || S.Scale <= 0.0f)
        {
            return Fail(TEXT("A finite, positive display scale is required."));
        }
        if (S.Visibility != SGraphEditor::Pin_Show && S.Visibility != SGraphEditor::Pin_HideNoConnection &&
            S.Visibility != SGraphEditor::Pin_HideNoConnectionNoDefault)
        {
            return Fail(TEXT("The graph pin-visibility mode is invalid."));
        }
        if (Cache) { Cache->Begin(Graph, S.Scale, S.Visibility); S.Revision = Cache->GetRevision(); }
        S.Nodes.Reserve(Graph->Nodes.Num()); S.Result.Nodes.Reserve(Graph->Nodes.Num());
        S.PendingNodes.Reserve(Graph->Nodes.Num());
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const int32 Index = S.Nodes.Num();
            S.Nodes.Add({Node, CaptureMeasurementState(*Node)});
            FMeasuredNode& Measured = S.Result.Nodes.AddDefaulted_GetRef();
            const FMeasuredNode* Cached = Cache ? Cache->Find(*Node, S.Nodes.Last().Signature) : nullptr;
            if (Cached) { Measured = *Cached; }
            else { S.PendingNodes.Add(Index); }
        }
        S.bStarted = true;
        if (S.PendingNodes.IsEmpty() && FPlatformTime::Seconds() >= Deadline) { return false; }
    }
    if (Graph->Nodes.Num() != S.Nodes.Num() || (Cache && Cache->GetRevision() != S.Revision))
    {
        return Fail(TEXT("The graph or measurement context changed during capture."));
    }
    for (int32 I = 0; I < S.Nodes.Num(); ++I)
    {
        if (S.Nodes[I].Node.Get() != Graph->Nodes[I]) { return Fail(TEXT("Node identities changed during capture.")); }
    }
    TSharedPtr<SGraphPanel> MeasurementPanel;
    while (S.Next < S.PendingNodes.Num())
    {
        const int32 NodeIndex = S.PendingNodes[S.Next];
        const auto& Entry = S.Nodes[NodeIndex];
        UEdGraphNode* Node = Entry.Node.Get();
        if (Entry.Signature != CaptureMeasurementState(*Node)) { return Fail(TEXT("A node changed during capture.")); }
        FMeasuredNode Measured;
        if (!MeasurementPanel && S.Visibility != SGraphEditor::Pin_Show)
        {
            MeasurementPanel = SNew(SGraphPanel).GraphObj(Graph).IsEditable(true).InitialZoomToFit(false);
            MeasurementPanel->SetPinVisibility(S.Visibility);
        }
        if (!MeasureNode(*Node, S.Scale, Measured, S.Reason, MeasurementPanel, S.Visibility, S.bNeedsRetry))
        {
            S.Reason = FString::Printf(TEXT("%s: %s"), *Node->GetName(), *S.Reason);
            S.bDone = true; return true;
        }
        if (!S.Graph.IsValid() || !Entry.Node.IsValid()) { return Fail(TEXT("The graph or node closed during measurement.")); }
        if (Cache)
        {
            if (Cache->GetRevision() != S.Revision) { return Fail(TEXT("The measurement context changed during capture.")); }
            Cache->Store(*Node, Entry.Signature, Measured);
        }
        S.Result.Nodes[NodeIndex] = MoveTemp(Measured); ++S.Next;
        if (FPlatformTime::Seconds() >= Deadline) { return false; }
    }
    if (!ValidateMeasurementGraph(Graph, S.Reason)) { S.bDone = true; return true; }
    if (Graph->Nodes.Num() != S.Nodes.Num()) { return Fail(TEXT("Node identities changed during capture.")); }
    for (int32 I = 0; I < S.Nodes.Num(); ++I)
    {
        const auto& Entry = S.Nodes[I];
        if (Entry.Node.Get() != Graph->Nodes[I]) { return Fail(TEXT("Node identities changed during capture.")); }
        if (Entry.Signature != CaptureMeasurementState(*Entry.Node.Get())) { return Fail(TEXT("A node changed during capture.")); }
    }
    S.Result.Nodes.Sort([](const FMeasuredNode& A, const FMeasuredNode& B) { return A.Id < B.Id; });
    S.bDone = true;
    return true;
}

bool FMeasurementJob::TakeResult(FGraphMeasurement& Out, FString& Reason, bool* OutNeedsLayoutRetry)
{
    auto& S = *State;
    Out.Nodes.Reset();
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = S.bNeedsRetry; }
    if (!S.bDone || S.bTaken) { Reason = TEXT("Measurement is incomplete or was already taken."); return false; }
    S.bTaken = true; Reason = S.Reason;
    if (!Reason.IsEmpty()) { return false; }
    Out = MoveTemp(S.Result); return true;
}

bool MeasureGraph(UEdGraph* Graph, float LayoutScale, FGraphMeasurement& OutMeasurement, FString& OutReason,
    const FMeasurementOptions& Options, bool* OutNeedsLayoutRetry)
{
    FMeasurementJob Job(Graph, LayoutScale, Options);
    Job.Advance(TNumericLimits<double>::Max());
    return Job.TakeResult(OutMeasurement, OutReason, OutNeedsLayoutRetry);
}

bool MeasureCommentHeader(UEdGraphNode_Comment* Comment, float LayoutScale, int32 ProposedWidth,
    FMeasuredRect& OutHeader, FString& OutReason, bool* OutNeedsLayoutRetry)
{
    OutHeader = {}; OutReason.Reset();
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = false; }
    if (!IsInGameThread() || !IsValid(Comment) || !FSlateApplication::IsInitialized() ||
        !FMath::IsFinite(LayoutScale) || LayoutScale <= 0 || ProposedWidth <= 32 || ProposedWidth > 16777216)
    {
        OutReason = TEXT("Proposed comment measurement requires a live comment and a valid width/display scale."); return false;
    }
    FMeasuredNode Measured; bool bRetry = false;
    if (!MeasureNode(*Comment, LayoutScale, Measured, OutReason, {}, SGraphEditor::Pin_Show, bRetry, ProposedWidth))
    {
        if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = bRetry; }
        return false;
    }
    if (!Measured.CommentHeader.IsSet()) { OutReason = TEXT("The proposed comment has no measured title."); return false; }
    OutHeader = Measured.CommentHeader.GetValue();
    return true;
}
}
