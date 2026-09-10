// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GraphEditor.h"

class UEdGraph;
class UEdGraphNode_Comment;

namespace GlooPrint
{
class FMeasurementCache;

struct FMeasurementOptions
{
    SGraphEditor::EPinVisibility PinVisibility = SGraphEditor::Pin_Show;
    FMeasurementCache* Cache = nullptr;
};

struct FMeasuredRect
{
    FVector2f Min = FVector2f::ZeroVector;
    FVector2f Max = FVector2f::ZeroVector;
    bool operator==(const FMeasuredRect& Other) const = default;
};

struct FMeasuredPin
{
    FGuid Id;
    TOptional<FVector2f> AttachmentOffset;
};

struct FMeasuredNode
{
    FGuid Id;
    FIntPoint Position = FIntPoint::ZeroValue;
    FVector2f BodySize = FVector2f::ZeroVector;
    FMeasuredRect VisualBounds;
    TOptional<FMeasuredRect> CommentHeader;
    TArray<FMeasuredPin> Pins;
};

struct FGraphMeasurement
{
    TArray<FMeasuredNode> Nodes;
};

class FMeasurementJob final
{
public:
    FMeasurementJob(UEdGraph* Graph, float LayoutScale, const FMeasurementOptions& Options = {});
    ~FMeasurementJob();
    bool Advance(double Deadline);
    bool TakeResult(FGraphMeasurement& Out, FString& Reason, bool* OutNeedsLayoutRetry = nullptr);
private:
    struct FState;
    TUniquePtr<FState> State;
};

bool MeasureGraph(UEdGraph* Graph, float LayoutScale, FGraphMeasurement& OutMeasurement, FString& OutReason,
    const FMeasurementOptions& Options = {}, bool* OutNeedsLayoutRetry = nullptr);

bool ValidateMeasurementGraph(UEdGraph* Graph, FString& OutReason);

bool MeasureCommentHeader(UEdGraphNode_Comment* Comment, float LayoutScale, int32 ProposedWidth,
    FMeasuredRect& OutHeader, FString& OutReason, bool* OutNeedsLayoutRetry = nullptr);
}
