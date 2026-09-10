// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintGraph.h"
#include "GlooPrintWireStyle.h"

namespace GlooPrint
{
inline constexpr float WireNodeClearance = 12.f;
inline constexpr float WireExitLength = 24.f;
inline constexpr float WireLaneSpacing = 12.f;

struct FLayoutResult;
struct FRouteKey
{
    FGuid FromNode, FromPin, ToNode, ToPin;
    bool operator==(const FRouteKey& Other) const = default;
    friend uint32 GetTypeHash(const FRouteKey& Key)
    {
        return HashCombine(HashCombine(GetTypeHash(Key.FromNode), GetTypeHash(Key.FromPin)),
            HashCombine(GetTypeHash(Key.ToNode), GetTypeHash(Key.ToPin)));
    }
};

struct FRouteCurve
{
    FVector2f Start, End, StartTangent, EndTangent;
    float Distances[17] = {};
    float Length = 0;
};

enum class ERouteFallback : uint8 { None, BlockedEndpoint, NoChannel, SearchLimit, ChannelLimit };
enum class ERouteMethod : uint8 { Native, Simple, Search, OuterLane };

struct FRouteSearchStats
{
    int32 ExpandedStates = 0;
    int32 CreatedStates = 0;
    int32 SegmentChecks = 0;
    int32 XChannels = 0;
    int32 YChannels = 0;
    bool bStateLimitReached = false;
    bool bChannelsTruncated = false;
};

struct FWireRoute
{
    FRouteKey Key;
    TArray<FVector2f> Points;
    TArray<FRouteCurve> Curves;
    FBox2f Bounds = FBox2f(ForceInit);
    FBox2f StartRegion = FBox2f(ForceInit), EndRegion = FBox2f(ForceInit);
    float Length = 0;
    ERouteFallback Fallback = ERouteFallback::None;
    ERouteMethod Method = ERouteMethod::Native;
    FRouteSearchStats Search;
};

struct FRouteSet
{
    TMap<FRouteKey, FWireRoute> Wires;
    int32 FallbackCount = 0;
};

class FRoutingJob final
{
public:
    explicit FRoutingJob(FLayoutGraph Graph, EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90);
    ~FRoutingJob();
    bool Advance(double Deadline);
    bool TakeResult(FRouteSet& OutRoutes, FString& OutReason, FLayoutGraph* OutSource = nullptr);
    int32 GetCompletedLinks() const;
private:
    struct FState;
    TUniquePtr<FState> State;
};

TUniquePtr<FRoutingJob> CreateLayoutRoutingJob(const FLayoutGraph& Graph, const FLayoutResult& Layout,
    FString& OutReason, EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90);

bool ComputeRoutes(const FLayoutGraph& Graph, FRouteSet& OutRoutes, FString& OutReason,
    EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90);
bool ComputeLayoutRoutes(const FLayoutGraph& Graph, const FLayoutResult& Layout, FRouteSet& OutRoutes,
    FString& OutReason, EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90);
FVector2f EvaluateRoute(const FWireRoute& Route, float Distance, FVector2f* OutDirection = nullptr);
void MeasureRouteCurve(FRouteCurve& Curve);
FVector2f EvaluateRouteCurve(const FRouteCurve& Curve, float Distance, FVector2f* OutDirection = nullptr);
}
