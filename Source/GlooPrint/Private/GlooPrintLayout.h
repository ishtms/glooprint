// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintGraph.h"

namespace GlooPrint
{
struct FLayoutSettings
{
    float HorizontalSpacing = 96;
    float VerticalSpacing = 48;
    float CommentPadding = 32;
    uint32 GridSize = 16;
    bool operator==(const FLayoutSettings& Other) const = default;
};

struct FLayoutResult
{
    TArray<FIntPoint> Positions;
    TArray<FIntPoint> Sizes;
    TBitArray<> FeedbackEdges;
    bool bLimitedComments = false;
    uint64 OrderingCrossings = 0;
    int32 OrderingSweeps = 4;
    TOptional<uint64> LastCandidateCrossings;
};

enum class ELayoutFailure : uint8 { None, InvalidInput, Constraints };

class FLayoutJob final
{
public:
    FLayoutJob(FLayoutGraph Graph, FLayoutSettings Settings);
    ~FLayoutJob();
    bool Advance(double Deadline);
    bool TakeResult(FLayoutResult& OutResult, FString& OutReason, ELayoutFailure* OutFailure = nullptr);
private:
    struct FState;
    TUniquePtr<FState> State;
};

bool ComputeLayout(const FLayoutGraph& Graph, const FLayoutSettings& Settings,
    FLayoutResult& OutResult, FString& OutReason, ELayoutFailure* OutFailure = nullptr);
}
