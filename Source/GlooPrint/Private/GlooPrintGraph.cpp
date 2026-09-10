// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintGraph.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace GlooPrint
{
bool CanFormatGraph(UEdGraph* Graph, FString& OutReason)
{
    OutReason.Reset();
    if (!IsInGameThread() || !GEditor || !IsValid(Graph))
    {
        OutReason = TEXT("Formatting requires a live graph on the editor thread.");
        return false;
    }
    if (!Graph->GetSchema() || Graph->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass())
    {
        OutReason = TEXT("Format Graph supports ordinary Blueprint K2 graphs.");
        return false;
    }
    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!IsValid(Blueprint) || Blueprint->bBeingCompiled || Blueprint->bIsRegeneratingOnLoad)
    {
        OutReason = TEXT("The Blueprint is unavailable or is being compiled/reconstructed.");
        return false;
    }
    if (FBlueprintEditorUtils::IsGraphReadOnly(Graph) || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
    {
        OutReason = TEXT("Formatting is unavailable in a read-only graph or during play/simulation.");
        return false;
    }
    if (!GEditor->CanTransact() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsNormalExecution())
    {
        OutReason = TEXT("The editor cannot start a layout transaction right now.");
        return false;
    }
    return true;
}

static bool BuildSnapshot(UEdGraph* Graph, FGraphMeasurement Measurement, const TSet<FGuid>& Selection,
    FLayoutGraph& OutGraph, FString& OutReason)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BuildSnapshot);
    FLayoutGraph Result;
    Result.Nodes.Reserve(Measurement.Nodes.Num());
    TMap<FGuid, UEdGraphNode*> NodesById;
    NodesById.Reserve(Graph->Nodes.Num());
    int32 PinCount = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        NodesById.Add(Node->NodeGuid, Node);
        PinCount += Node->Pins.Num();
    }
    Result.Pins.Reserve(PinCount);
    TMap<const UEdGraphPin*, int32> PinIndices;
    PinIndices.Reserve(PinCount);
    TArray<const UEdGraphPin*> OriginalPins;
    OriginalPins.Reserve(PinCount);
    for (FMeasuredNode& Geometry : Measurement.Nodes)
    {
        UEdGraphNode* Node = NodesById.FindChecked(Geometry.Id);
        const int32 NodeIndex = Result.Nodes.Num();
        FLayoutNode& Item = Result.Nodes.AddDefaulted_GetRef();
        Item.Geometry = MoveTemp(Geometry);
        Item.FirstPin = Result.Pins.Num();
        Item.PinCount = Node->Pins.Num();
        Item.bComment = Node->IsA<UEdGraphNode_Comment>();
        Item.bReroute = Node->IsA<UK2Node_Knot>();
        Item.OriginalSize = FIntPoint(Node->NodeWidth, Node->NodeHeight);
        bool bExecutionInput = false;
        bool bExecutionOutput = false;
        for (int32 Ordinal = 0; Ordinal < Node->Pins.Num(); ++Ordinal)
        {
            const UEdGraphPin* Pin = Node->Pins[Ordinal];
            FLayoutPin Value;
            Value.Id = Pin->PinId;
            Value.Node = NodeIndex;
            Value.Ordinal = Ordinal;
            Value.bOutput = Pin->Direction == EGPD_Output;
            Value.Kind = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? ELinkKind::Execution :
                (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate ||
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ? ELinkKind::Delegate : ELinkKind::Data);
            Value.Offset = Item.Geometry.Pins[Ordinal].AttachmentOffset;
            bExecutionInput |= Value.Kind == ELinkKind::Execution && !Value.bOutput;
            bExecutionOutput |= Value.Kind == ELinkKind::Execution && Value.bOutput;
            PinIndices.Add(Pin, Result.Pins.Add(Value));
            OriginalPins.Add(Pin);
        }
        Item.bEntry = bExecutionOutput && !bExecutionInput;
    }

    TSet<uint64> DirectedLinks;
    for (int32 Index = 0; Index < OriginalPins.Num(); ++Index)
    {
        for (const UEdGraphPin* Linked : OriginalPins[Index]->LinkedTo)
        {
            const int32* Other = PinIndices.Find(Linked);
            if (!Other || Result.Pins[Index].bOutput == Result.Pins[*Other].bOutput ||
                Result.Pins[Index].Kind != Result.Pins[*Other].Kind ||
                !Result.Pins[Index].Offset.IsSet() || !Result.Pins[*Other].Offset.IsSet())
            {
                OutReason = TEXT("A connection has a missing, incompatible, external, or unmeasured endpoint.");
                return false;
            }
            const uint64 Key = (uint64(uint32(Index)) << 32) | uint32(*Other);
            if (DirectedLinks.Contains(Key))
            {
                OutReason = TEXT("A pin contains a duplicate connection.");
                return false;
            }
            DirectedLinks.Add(Key);
        }
    }
    Result.Edges.Reserve(DirectedLinks.Num() / 2);
    for (const uint64 Key : DirectedLinks)
    {
        const int32 From = int32(Key >> 32);
        const int32 To = int32(Key & 0xffffffffu);
        if (!DirectedLinks.Contains((uint64(uint32(To)) << 32) | uint32(From)))
        {
            OutReason = TEXT("A connection is not reciprocal.");
            return false;
        }
        if (Result.Pins[From].bOutput)
        {
            Result.Edges.Add({From, To, Result.Pins[From].Kind});
        }
    }
    Result.Edges.Sort([](const FLayoutEdge& A, const FLayoutEdge& B)
    {
        return A.From != B.From ? A.From < B.From : A.To < B.To;
    });
    for (int32 Index = 0; Index < Result.Edges.Num(); ++Index)
    {
        const FLayoutEdge& Edge = Result.Edges[Index];
        Result.Nodes[Result.Pins[Edge.From].Node].Outgoing.Add(Index);
        Result.Nodes[Result.Pins[Edge.To].Node].Incoming.Add(Index);
    }
    for (int32 Pass = 0; Pass < 4 && Result.Anchor == INDEX_NONE; ++Pass)
    {
        for (int32 Index = 0; Index < Result.Nodes.Num(); ++Index)
        {
            const FLayoutNode& Node = Result.Nodes[Index];
            if (!Node.bComment && (Pass >= 2 || Selection.Contains(Node.Geometry.Id)) &&
                ((Pass % 2) != 0 || Node.bEntry))
            {
                Result.Anchor = Index;
                break;
            }
        }
    }
    OutGraph = MoveTemp(Result);
    return true;
}

struct FGraphCaptureJob::FState
{
    TWeakObjectPtr<UEdGraph> Graph;
    TSet<FGuid> Selection;
    FMeasurementJob Measurement;
    FLayoutGraph Result;
    FString Reason;
    bool bDone = false, bTaken = false, bNeedsRetry = false;
    FState(UEdGraph* InGraph, float Scale, TSet<FGuid> InSelection, const FMeasurementOptions& Options)
        : Graph(InGraph), Selection(MoveTemp(InSelection)), Measurement(InGraph, Scale, Options) {}
};
FGraphCaptureJob::FGraphCaptureJob(UEdGraph* Graph, float Scale, TSet<FGuid> Selection, const FMeasurementOptions& Options)
    : State(MakeUnique<FState>(Graph, Scale, MoveTemp(Selection), Options)) {}
FGraphCaptureJob::~FGraphCaptureJob() = default;

bool FGraphCaptureJob::Advance(double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_Snapshot);
    auto& S = *State;
    if (S.bDone) { return true; }
    if (!S.Measurement.Advance(Deadline)) { return false; }
    FGraphMeasurement Measured;
    if (S.Measurement.TakeResult(Measured, S.Reason, &S.bNeedsRetry))
    {
        BuildSnapshot(S.Graph.Get(), MoveTemp(Measured), S.Selection, S.Result, S.Reason);
    }
    S.bDone = true; return true;
}

bool FGraphCaptureJob::TakeResult(FLayoutGraph& Out, FString& Reason, bool* OutNeedsLayoutRetry)
{
    auto& S = *State;
    Out = {};
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = S.bNeedsRetry; }
    if (!S.bDone || S.bTaken) { Reason = TEXT("Capture is incomplete or was already taken."); return false; }
    S.bTaken = true; Reason = S.Reason;
    if (!Reason.IsEmpty()) { return false; }
    Out = MoveTemp(S.Result); return true;
}

static bool CaptureGraphSnapshot(UEdGraph* Graph, float Scale, const TSet<FGuid>& Selection,
    FLayoutGraph& Out, FString& Reason, const FMeasurementOptions& Options, bool* OutNeedsLayoutRetry)
{
    FGraphCaptureJob Job(Graph, Scale, Selection, Options);
    Job.Advance(TNumericLimits<double>::Max());
    return Job.TakeResult(Out, Reason, OutNeedsLayoutRetry);
}

bool CaptureGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection,
    FLayoutGraph& OutGraph, FString& OutReason, const FMeasurementOptions& MeasurementOptions, bool* OutNeedsLayoutRetry)
{
    if (!CanFormatGraph(Graph, OutReason))
    {
        OutGraph = {};
        if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = false; }
        return false;
    }
    return CaptureGraphSnapshot(Graph, LayoutScale, Selection, OutGraph, OutReason, MeasurementOptions, OutNeedsLayoutRetry);
}

bool CaptureGraphForRouting(UEdGraph* Graph, float LayoutScale, FLayoutGraph& OutGraph, FString& OutReason,
    const FMeasurementOptions& MeasurementOptions, bool* OutNeedsLayoutRetry)
{
    return CaptureGraphSnapshot(Graph, LayoutScale, {}, OutGraph, OutReason, MeasurementOptions, OutNeedsLayoutRetry);
}
}
