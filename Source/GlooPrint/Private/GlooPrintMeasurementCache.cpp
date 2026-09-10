// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintMeasurementCache.h"
#include "GlooPrintLayout.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "GraphEditAction.h"
#include "Internationalization/TextLocalizationManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/ObjectWriter.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"

namespace GlooPrint
{
namespace
{
void AppendPresentationState(UEdGraphNode& Node, FObjectWriter& Writer)
{
    FString Title = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    bool bHasMessage = Node.bHasCompilerMessage;
    Writer << Title << bHasMessage << Node.ErrorType << Node.ErrorMsg;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        uint64 Identity = reinterpret_cast<UPTRINT>(Pin);
        Writer << Identity;
    }
}

class FLayoutInvariantWriter final : public FObjectWriter
{
public:
    explicit FLayoutInvariantWriter(TArray<uint8>& Bytes) : FObjectWriter(Bytes)
    {
        ArNoDelta = true; ArPortFlags |= PPF_DuplicateVerbatim;
    }
    virtual bool ShouldSkipProperty(const FProperty* Property) const override
    {
        return (Property->GetOwnerStruct() == UEdGraphNode::StaticClass() &&
            (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UEdGraphNode, NodePosX) ||
             Property->GetFName() == GET_MEMBER_NAME_CHECKED(UEdGraphNode, NodePosY))) ||
            FObjectWriter::ShouldSkipProperty(Property);
    }
};

TArray<uint8> CaptureLayoutInvariantState(UEdGraphNode& Node)
{
    TArray<uint8> State;
    FLayoutInvariantWriter Writer(State);
    Node.Serialize(Writer);
    AppendPresentationState(Node, Writer);
    return State;
}
}

FMeasurementCache::~FMeasurementCache()
{
    if (UEdGraph* LiveGraph = Graph.Get()) { LiveGraph->RemoveOnGraphChangedHandler(GraphChangedHandle); }
}

void FMeasurementCache::Begin(UEdGraph* InGraph, float LayoutScale, SGraphEditor::EPinVisibility PinVisibility)
{
    check(IsInGameThread());
    Hits = 0; Misses = 0;
    if (Graph.Get() != InGraph)
    {
        if (UEdGraph* Old = Graph.Get()) { Old->RemoveOnGraphChangedHandler(GraphChangedHandle); }
        Graph = InGraph;
        GraphChangedHandle = InGraph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateSP(this, &FMeasurementCache::OnGraphChanged));
        Invalidate();
    }
    const uint16 CurrentTextRevision = FTextLocalizationManager::Get().GetTextRevision();
    const void* CurrentStyle = &FAppStyle::Get();
    if (Scale != LayoutScale || Visibility != PinVisibility || TextRevision != CurrentTextRevision || StyleIdentity != CurrentStyle)
    {
        Invalidate();
        Scale = LayoutScale; Visibility = PinVisibility; TextRevision = CurrentTextRevision; StyleIdentity = CurrentStyle;
    }
    TSet<FGuid> Present;
    Present.Reserve(InGraph->Nodes.Num());
    for (const UEdGraphNode* Node : InGraph->Nodes) { Present.Add(Node->NodeGuid); }
    for (auto It = Entries.CreateIterator(); It; ++It)
    {
        if (!It.Value().Node.IsValid() || !Present.Contains(It.Key())) { It.RemoveCurrent(); }
    }
}

void FMeasurementCache::Invalidate(bool bContextChanged)
{
    Entries.Reset(); ++Revision;
    if (bContextChanged) { ++ContextRevision; }
}

void FMeasurementCache::OnGraphChanged(const FEdGraphEditAction& Action)
{
    if (Action.Nodes.IsEmpty()) { Invalidate(false); return; }
    ++Revision;
    for (const UEdGraphNode* Node : Action.Nodes)
    {
        if (IsValid(Node)) { Entries.Remove(Node->NodeGuid); }
    }
}

const FMeasuredNode* FMeasurementCache::Find(const UEdGraphNode& Node, const TArray<uint8>& State)
{
    const FEntry* Entry = Entries.Find(Node.NodeGuid);
    if (Entry && Entry->Node.Get() == &Node && Entry->State == State)
    {
        ++Hits; return &Entry->Geometry;
    }
    ++Misses; return nullptr;
}

void FMeasurementCache::Store(const UEdGraphNode& Node, TArray<uint8> State, const FMeasuredNode& Measurement)
{
    FEntry& Entry = Entries.FindOrAdd(Node.NodeGuid);
    Entry.Node = const_cast<UEdGraphNode*>(&Node);
    Entry.State = MoveTemp(State); Entry.Geometry = Measurement;
}

FMeasurementCache::FLayoutReuse FMeasurementCache::PrepareLayoutReuse(const FLayoutGraph& Snapshot, const FLayoutResult& Layout) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_PrepareLayoutMeasurements);
    FLayoutReuse Reuse;
    FString Reason;
    UEdGraph* LiveGraph = Graph.Get();
    if (Layout.Positions.Num() != Snapshot.Nodes.Num() || Layout.Sizes.Num() != Snapshot.Nodes.Num()) { return Reuse; }
    bool bChanges = false;
    for (int32 I = 0; I < Snapshot.Nodes.Num(); ++I)
    {
        if (Layout.Positions[I] != Snapshot.Nodes[I].Geometry.Position || Layout.Sizes[I] != Snapshot.Nodes[I].OriginalSize)
        {
            bChanges = true; break;
        }
    }
    if (!bChanges || !ValidateMeasurementGraph(LiveGraph, Reason)) { return Reuse; }
    Reuse.Graph = LiveGraph; Reuse.NodeCount = LiveGraph->Nodes.Num(); Reuse.ContextRevision = ContextRevision;
    TMap<FGuid, UEdGraphNode*> LiveNodes;
    LiveNodes.Reserve(Reuse.NodeCount); Reuse.Nodes.Reserve(Reuse.NodeCount);
    for (UEdGraphNode* Node : LiveGraph->Nodes) { LiveNodes.Add(Node->NodeGuid, Node); }
    for (int32 I = 0; I < Snapshot.Nodes.Num(); ++I)
    {
        const auto& Node = Snapshot.Nodes[I];
        if (Node.bComment) { continue; }
        UEdGraphNode* const* Live = LiveNodes.Find(Node.Geometry.Id);
        if (!Live) { continue; }
        const TArray<uint8> CurrentState = CaptureMeasurementState(**Live);
        const FEntry* Entry = Entries.Find(Node.Geometry.Id);
        if (!Entry || Entry->Node.Get() != *Live || Entry->State != CurrentState) { continue; }
        FLayoutReuse::FNode& Candidate = Reuse.Nodes.AddDefaulted_GetRef();
        Candidate.Geometry = Entry->Geometry; Candidate.Geometry.Position = Layout.Positions[I];
        Candidate.Node = *Live; Candidate.InvariantState = CaptureLayoutInvariantState(**Live);
    }
    return Reuse;
}

void FMeasurementCache::RestoreLayoutReuse(FLayoutReuse Reuse)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_RestoreLayoutMeasurements);
    UEdGraph* LiveGraph = Graph.Get();
    FString Reason;
    if (Reuse.Nodes.IsEmpty() || Reuse.Graph.Get() != LiveGraph || Reuse.ContextRevision != ContextRevision ||
        TextRevision != FTextLocalizationManager::Get().GetTextRevision() || StyleIdentity != &FAppStyle::Get() ||
        !ValidateMeasurementGraph(LiveGraph, Reason) || LiveGraph->Nodes.Num() != Reuse.NodeCount) { return; }
    TSet<const UEdGraphNode*> LiveNodes;
    LiveNodes.Reserve(Reuse.NodeCount);
    for (const UEdGraphNode* Node : LiveGraph->Nodes) { LiveNodes.Add(Node); }
    for (auto& Candidate : Reuse.Nodes)
    {
        UEdGraphNode* Node = Candidate.Node.Get();
        if (!Node || !LiveNodes.Contains(Node) || Node->NodeGuid != Candidate.Geometry.Id ||
            FIntPoint(Node->NodePosX, Node->NodePosY) != Candidate.Geometry.Position ||
            CaptureLayoutInvariantState(*Node) != Candidate.InvariantState) { continue; }
        TArray<uint8> State = CaptureMeasurementState(*Node);
        if (Reuse.ContextRevision != ContextRevision || Candidate.Node.Get() != Node) { return; }
        Store(*Node, MoveTemp(State), Candidate.Geometry);
    }
}

TArray<uint8> CaptureMeasurementState(UEdGraphNode& Node)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasurementState);
    TArray<uint8> State;
    FObjectWriter Writer(&Node, State, false, false, false, PPF_DuplicateVerbatim);
    AppendPresentationState(Node, Writer);
    return State;
}
}
