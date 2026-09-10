// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintMeasurement.h"

class UEdGraph;

namespace GlooPrint
{
enum class ELinkKind : uint8 { Execution, Data, Delegate };

struct FLayoutPin
{
    FGuid Id;
    int32 Node = INDEX_NONE;
    int32 Ordinal = 0;
    bool bOutput = false;
    ELinkKind Kind = ELinkKind::Data;
    TOptional<FVector2f> Offset;
};

struct FLayoutEdge
{
    int32 From = INDEX_NONE;
    int32 To = INDEX_NONE;
    ELinkKind Kind = ELinkKind::Data;
};

struct FLayoutNode
{
    FMeasuredNode Geometry;
    int32 FirstPin = 0;
    int32 PinCount = 0;
    TArray<int32> Incoming;
    TArray<int32> Outgoing;
    bool bEntry = false;
    bool bComment = false;
    bool bReroute = false;
    FIntPoint OriginalSize = FIntPoint::ZeroValue;
};

struct FLayoutGraph
{
    TArray<FLayoutNode> Nodes;
    TArray<FLayoutPin> Pins;
    TArray<FLayoutEdge> Edges;
    int32 Anchor = INDEX_NONE;
};

class FGraphCaptureJob final
{
public:
    FGraphCaptureJob(UEdGraph* Graph, float Scale, TSet<FGuid> Selection, const FMeasurementOptions& Options = {});
    ~FGraphCaptureJob();
    bool Advance(double Deadline);
    bool TakeResult(FLayoutGraph& Out, FString& Reason, bool* OutNeedsLayoutRetry = nullptr);
private:
    struct FState;
    TUniquePtr<FState> State;
};

bool CanFormatGraph(UEdGraph* Graph, FString& OutReason);
bool CaptureGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection,
    FLayoutGraph& OutGraph, FString& OutReason, const FMeasurementOptions& MeasurementOptions = {}, bool* OutNeedsLayoutRetry = nullptr);

bool CaptureGraphForRouting(UEdGraph* Graph, float LayoutScale, FLayoutGraph& OutGraph, FString& OutReason,
    const FMeasurementOptions& MeasurementOptions = {}, bool* OutNeedsLayoutRetry = nullptr);
}
