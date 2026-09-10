// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintRouting.h"
#include "GlooPrintLayout.h"
#include "GlooPrintRouteChannels.h"
#include "GlooPrintObstacleIndex.h"

#include "Algo/Sort.h"
#include "Algo/Reverse.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace GlooPrint
{
namespace
{
constexpr float Clearance = WireNodeClearance;
constexpr float ExitLength = WireExitLength;
constexpr float CornerRadius = 12;
using Routing::FObstacles;
constexpr int32 MaxChannels = RouteChannelLimit;
constexpr int32 MaxSearchExpansions = 2048;
constexpr double BendCost = 24;

FBox2f SegmentBounds(FVector2f A, FVector2f B)
{
    FBox2f Box(A, A);
    Box += B;
    return Box;
}

bool ValidPoint(FVector2f P)
{
    return FMath::IsFinite(P.X) && FMath::IsFinite(P.Y) && FMath::Abs(P.X) <= 16777216 && FMath::Abs(P.Y) <= 16777216;
}

void Simplify(TArray<FVector2f>& Points)
{
    int32 Count = 0;
    for (FVector2f P : Points)
    {
        if (Count > 0 && P == Points[Count - 1]) { continue; }
        while (Count >= 2)
        {
            const FVector2f A = Points[Count - 1] - Points[Count - 2], B = P - Points[Count - 1];
            if (A.X * B.Y != A.Y * B.X || FVector2f::DotProduct(A, B) < 0) { break; }
            --Count;
        }
        Points[Count++] = P;
    }
    Points.SetNum(Count, EAllowShrinking::No);
}

void AddCurve(FWireRoute& Route, FVector2f A, FVector2f B, FVector2f TA, FVector2f TB)
{
    if (A == B) { return; }
    auto& Curve = Route.Curves.AddDefaulted_GetRef();
    Curve.Start = A; Curve.End = B; Curve.StartTangent = TA; Curve.EndTangent = TB;
    MeasureRouteCurve(Curve); Route.Length += Curve.Length;
    Route.Bounds += A; Route.Bounds += B; Route.Bounds += A + TA / 3; Route.Bounds += B - TB / 3;
}

void StyleRoute(FWireRoute& Route, const FObstacles& Obstacles, EGlooPrintWireStyle Style)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_StyleRoute);
    Route.Curves.Reserve(2 * Route.Points.Num() - 3);
    FVector2f Start = Route.Points[0];
    for (int32 I = 1; I + 1 < Route.Points.Num(); ++I)
    {
        const FVector2f Corner = Route.Points[I];
        const FVector2f Incoming = Corner - Route.Points[I - 1], Outgoing = Route.Points[I + 1] - Corner;
        const FVector2f U = Incoming.GetSafeNormal(), V = Outgoing.GetSafeNormal();
        const bool bDiagonal = Style == EGlooPrintWireStyle::Diagonal45;
        float Radius = FMath::Min3(bDiagonal ? 32.f : CornerRadius, Incoming.Size() * 0.5f, Outgoing.Size() * 0.5f);
        for (int32 Attempt = 0; Radius > 0; ++Attempt)
        {
            FBox2f Hull = SegmentBounds(Corner - U * Radius, Corner + V * Radius);
            Hull += Corner;
            if (Obstacles.ClearBox(Hull)) { break; }
            Radius = Attempt < 5 ? Radius * 0.5f : 0;
        }
        const FVector2f A = Corner - U * Radius, B = Corner + V * Radius;
        AddCurve(Route, Start, A, A - Start, A - Start);
        if (bDiagonal) { AddCurve(Route, A, B, B - A, B - A); }
        else
        {
            constexpr float QuarterCircleTangent = 1.65685424949f;
            AddCurve(Route, A, B, U * Radius * QuarterCircleTangent, V * Radius * QuarterCircleTangent);
        }
        Start = B;
    }
    const FVector2f End = Route.Points.Last();
    AddCurve(Route, Start, End, End - Start, End - Start);
}

bool OrderChannels(TArray<float>& Channels, float Center)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_OrderRouteChannels);
    Channels.Sort([Center](float A, float B)
    {
        const float DA = FMath::Abs(A - Center), DB = FMath::Abs(B - Center);
        return DA != DB ? DA < DB : A < B;
    });
    int32 Count = 0;
    bool bTruncated = false;
    for (float Value : Channels)
    {
        if (Count > 0 && Channels[Count - 1] == Value) { continue; }
        if (Count == MaxChannels) { bTruncated = true; break; }
        Channels[Count++] = Value;
    }
    Channels.SetNum(Count, EAllowShrinking::No);
    return bTruncated;
}


struct FSearchState
{
    int32 Key = 0;
    int32 Previous = INDEX_NONE;
    double Cost = 0;
    bool bClosed = false;
};

struct FSearchQueueItem
{
    int32 State = 0;
    int32 Key = 0;
    double Cost = 0;
    double Estimate = 0;
};

struct FSearchScratch
{
    TArray<float> X, Y;
    TArray<FSearchState> States;
    TArray<FSearchQueueItem> Queue;
    TMap<int32, int32> StateIndices;
    TMap<uint64, bool> SegmentClearance;
};

bool SearchChannels(FVector2f Start, FVector2f End, FSearchScratch& Work,
    TFunctionRef<bool(FVector2f, FVector2f)> ClearSegment, FRouteSearchStats& Stats,
    TArray<FVector2f>& OutPath)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_RouteSearch);
    OutPath.Reset();
    Stats.bChannelsTruncated = OrderChannels(Work.X, (Start.X + End.X) * 0.5f);
    Stats.bChannelsTruncated |= OrderChannels(Work.Y, (Start.Y + End.Y) * 0.5f);
    Work.X.AddUnique(Start.X); Work.X.AddUnique(End.X); Work.X.Sort();
    Work.Y.AddUnique(Start.Y); Work.Y.AddUnique(End.Y); Work.Y.Sort();
    Stats.XChannels = Work.X.Num(); Stats.YChannels = Work.Y.Num();
    const int32 StartVertex = Work.X.IndexOfByKey(Start.X) * Work.Y.Num() + Work.Y.IndexOfByKey(Start.Y);
    const int32 EndVertex = Work.X.IndexOfByKey(End.X) * Work.Y.Num() + Work.Y.IndexOfByKey(End.Y);
    Work.States.Reset(); Work.Queue.Reset(); Work.StateIndices.Reset(); Work.SegmentClearance.Reset();
    Work.States.Reserve(MaxSearchExpansions * 4 + 1);
    Work.Queue.Reserve(MaxSearchExpansions * 4 + 1);
    Work.StateIndices.Reserve(MaxSearchExpansions * 4 + 1);
    Work.SegmentClearance.Reserve(MaxSearchExpansions * 4);
    const auto Point = [&Work](int32 Vertex) { return FVector2f(Work.X[Vertex / Work.Y.Num()], Work.Y[Vertex % Work.Y.Num()]); };
    const auto Estimate = [End](FVector2f P) { return FMath::Abs(double(P.X) - End.X) + FMath::Abs(double(P.Y) - End.Y); };
    const auto Earlier = [](const FSearchQueueItem& A, const FSearchQueueItem& B)
    {
        if (A.Estimate != B.Estimate) { return A.Estimate < B.Estimate; }
        if (A.Cost != B.Cost) { return A.Cost > B.Cost; }
        return A.Key < B.Key;
    };
    constexpr int32 DX[] = {1, 0, -1, 0}, DY[] = {0, 1, 0, -1};
    Work.States.Add({StartVertex * 4, INDEX_NONE, 0, false});
    Work.StateIndices.Add(StartVertex * 4, 0);
    Work.Queue.HeapPush({0, StartVertex * 4, 0, Estimate(Start)}, Earlier);
    int32 Found = INDEX_NONE;
    while (!Work.Queue.IsEmpty())
    {
        FSearchQueueItem Item;
        Work.Queue.HeapPop(Item, Earlier, EAllowShrinking::No);
        if (Work.States[Item.State].bClosed || Work.States[Item.State].Cost != Item.Cost) { continue; }
        if (Stats.ExpandedStates == MaxSearchExpansions) { Stats.bStateLimitReached = true; break; }
        Work.States[Item.State].bClosed = true;
        const FSearchState Current = Work.States[Item.State];
        ++Stats.ExpandedStates;
        const int32 Vertex = Current.Key / 4;
        if (Vertex == EndVertex) { Found = Item.State; break; }
        const int32 X = Vertex / Work.Y.Num(), Y = Vertex % Work.Y.Num(), Arrival = Current.Key % 4;
        const FVector2f A = Point(Vertex);
        for (int32 Direction = 0; Direction < 4; ++Direction)
        {
            if (Direction == (Arrival + 2) % 4) { continue; }
            const int32 NX = X + DX[Direction], NY = Y + DY[Direction];
            if (!Work.X.IsValidIndex(NX) || !Work.Y.IsValidIndex(NY)) { continue; }
            const int32 NextVertex = NX * Work.Y.Num() + NY;
            if (NextVertex == EndVertex && Direction == 2) { continue; }
            const int32 Key = NextVertex * 4 + Direction;
            const int32* Existing = Work.StateIndices.Find(Key);
            if (Existing && Work.States[*Existing].bClosed) { continue; }
            const FVector2f B = Point(NextVertex);
            double Cost = Current.Cost + FMath::Abs(double(B.X) - A.X) + FMath::Abs(double(B.Y) - A.Y);
            if (Direction != Arrival) { Cost += BendCost; }
            if (NextVertex == EndVertex && Direction != 0) { Cost += BendCost; }
            if (Existing && Work.States[*Existing].Cost <= Cost) { continue; }
            const uint64 SegmentKey = (uint64(FMath::Min(Vertex, NextVertex)) << 32) | uint32(FMath::Max(Vertex, NextVertex));
            const bool* Cached = Work.SegmentClearance.Find(SegmentKey);
            bool bClear;
            if (Cached) { bClear = *Cached; }
            else
            {
                ++Stats.SegmentChecks;
                bClear = ClearSegment(A, B);
                Work.SegmentClearance.Add(SegmentKey, bClear);
            }
            if (!bClear) { continue; }
            int32 Next;
            if (Existing)
            {
                Next = *Existing;
                Work.States[Next].Cost = Cost; Work.States[Next].Previous = Item.State;
            }
            else
            {
                Next = Work.States.Add({Key, Item.State, Cost, false});
                Work.StateIndices.Add(Key, Next);
            }
            Work.Queue.HeapPush({Next, Key, Cost, Cost + Estimate(B)}, Earlier);
        }
    }
    Stats.CreatedStates = Work.States.Num();
    if (Found == INDEX_NONE) { return false; }
    for (int32 I = Found, Remaining = Work.States.Num(); I != INDEX_NONE && Remaining-- > 0; I = Work.States[I].Previous)
    {
        OutPath.Add(Point(Work.States[I].Key / 4));
    }
    Algo::Reverse(OutPath);
    Simplify(OutPath);
    return !OutPath.IsEmpty() && OutPath[0] == Start && OutPath.Last() == End;
}
}

struct FRoutingJob::FState
{
    enum class EPhase : uint8 { Nodes, Channels, Edges, Order, Routes, Done };
    struct FReservedSegment
    {
        FVector2f A, B;
        int32 From, To;
        bool bFirst, bLast;
        bool bPinApproach = false;
    };
    struct FPinTurns
    {
        TOptional<float> Min, Max;
        void Add(float X)
        {
            Min = Min.IsSet() ? FMath::Min(Min.GetValue(), X) : X;
            Max = Max.IsSet() ? FMath::Max(Max.GetValue(), X) : X;
        }
    };
    struct FTurn
    {
        FVector2f Stem, Step, Corner;
        double LocalCost;
    };
    FLayoutGraph Graph;
    EGlooPrintWireStyle Style;
    EPhase Phase = EPhase::Nodes;
    FString Reason;
    FRouteSet Result;
    FObstacles Obstacles;
    TArray<FBox2f> Bodies;
    FChannelCoordinates BodyX, BodyMinY, BodyMaxY, ReservedX, ReservedY;
    TArray<int32> Order, FanOut, FanIn;
    TArray<FReservedSegment> Reserved;
    struct FTerminalReservation { int32 Segment, Obstacle; };
    TMap<int32, FTerminalReservation> TerminalReservations;
    TBitArray<> PendingPinApproaches, SharedPins;
    FObstacles ReservedHorizontal, ReservedVertical;
    FSearchScratch SearchWork;
    TArray<float> XChannels, YChannels, Frontier, TurnChannels;
    TArray<FTurn> Departures, Approaches;
    TArray<FVector2f> BestPoints, SearchPath, SearchCandidate;
    TArray<FPinTurns> PinTurns;
    int32 NextNode = 0, NextEdge = 0, NextRoute = 0;
    bool bTaken = false;

    FState(FLayoutGraph InGraph, EGlooPrintWireStyle InStyle) : Graph(MoveTemp(InGraph)), Style(InStyle)
    {
        if (Style != EGlooPrintWireStyle::Rounded90 && Style != EGlooPrintWireStyle::Diagonal45)
        {
            Reason = TEXT("Custom routing requires a supported custom wire style."); Phase = EPhase::Done; return;
        }
        Bodies.Reserve(Graph.Nodes.Num()); Order.Reserve(Graph.Edges.Num());
        Result.Wires.Reserve(Graph.Edges.Num());
        Obstacles.Reserve(Graph.Nodes.Num());
        Reserved.Reserve(Graph.Edges.Num());
        XChannels.Reserve(2 + MaxChannels * 2); YChannels.Reserve(1 + MaxChannels * 3);
        Frontier.Reserve(12); TurnChannels.Reserve(MaxChannels + 2);
        FanOut.Init(0, Graph.Pins.Num()); FanIn.Init(0, Graph.Pins.Num());
        PinTurns.SetNum(Graph.Pins.Num());
        PendingPinApproaches.Init(false, Graph.Pins.Num()); SharedPins.Init(false, Graph.Pins.Num());
    }
    bool AddNode(int32 I);
    bool AddEdge(int32 I);
    FVector2f TerminalEnd(int32 Pin, FVector2f Attachment, bool bOutput) const;
    void ReserveSegment(FVector2f A, FVector2f B, int32 From, int32 To, bool bFirst, bool bLast);
    void RouteOne(int32 Index);
    void Step();
};

bool FRoutingJob::FState::AddNode(int32 I)
{
    const auto& Node = Graph.Nodes[I];
    const FVector2f Position(Node.Geometry.Position), Size = Node.Geometry.BodySize;
    if (!ValidPoint(Position) || !ValidPoint(Position + Size) || Size.X <= 0 || Size.Y <= 0)
    {
        Reason = TEXT("Routing requires finite measured node bounds."); return false;
    }
    Bodies.Add(FBox2f(Position, Position + Size).ExpandBy(Clearance));
    if (!Node.bComment) { Obstacles.Add(Bodies.Last(), I); }
    else if (Node.Geometry.CommentHeader.IsSet())
    {
        const auto& Header = Node.Geometry.CommentHeader.GetValue();
        if (!ValidPoint(Position + Header.Min) || !ValidPoint(Position + Header.Max) ||
            Header.Min.X > Header.Max.X || Header.Min.Y > Header.Max.Y)
        {
            Reason = TEXT("Routing requires finite comment header bounds."); return false;
        }
        Obstacles.Add(FBox2f(Position + Header.Min, Position + Header.Max).ExpandBy(Clearance), I);
    }
    return true;
}

bool FRoutingJob::FState::AddEdge(int32 I)
{
    const auto& E = Graph.Edges[I];
    if (!Graph.Pins.IsValidIndex(E.From) || !Graph.Pins.IsValidIndex(E.To))
    {
        Reason = TEXT("Routing encountered an invalid edge."); return false;
    }
    for (int32 PinIndex : {E.From, E.To})
    {
        const auto& Pin = Graph.Pins[PinIndex];
        if (!Graph.Nodes.IsValidIndex(Pin.Node) || !Pin.Offset.IsSet() || !ValidPoint(Pin.Offset.GetValue()))
        {
            Reason = TEXT("Routing requires every linked pin attachment."); return false;
        }
    }
    for (int32 PinIndex : {E.From, E.To})
    {
        if (PendingPinApproaches[PinIndex]) { SharedPins[PinIndex] = true; continue; }
        PendingPinApproaches[PinIndex] = true;
        const auto& Pin = Graph.Pins[PinIndex];
        const bool bOutput = PinIndex == E.From;
        const FVector2f Attachment = FVector2f(Graph.Nodes[Pin.Node].Geometry.Position) + Pin.Offset.GetValue();
        const FVector2f Terminal = TerminalEnd(PinIndex, Attachment, bOutput);
        if (!Obstacles.ClearLine(Attachment, Terminal, Pin.Node)) { continue; }
        FVector2f ReservedEnd = Terminal;
        ReservedEnd.X += bOutput ? WireLaneSpacing : -WireLaneSpacing;
        if (!Obstacles.ClearLine(Attachment, ReservedEnd, Pin.Node)) { ReservedEnd = Terminal; }
        const int32 Id = Reserved.Add({Attachment, ReservedEnd, bOutput ? PinIndex : INDEX_NONE,
            bOutput ? INDEX_NONE : PinIndex, bOutput, !bOutput, true});
        ReservedHorizontal.Add(SegmentBounds(Attachment, ReservedEnd).ExpandBy(WireLaneSpacing), Id);
    }
    Order.Add(I);
    return true;
}

FVector2f FRoutingJob::FState::TerminalEnd(int32 Pin, FVector2f Attachment, bool bOutput) const
{
    const auto& Body = Bodies[Graph.Pins[Pin].Node];
    return {bOutput ? FMath::Max(Attachment.X + ExitLength, Body.Max.X + CornerRadius)
                    : FMath::Min(Attachment.X - ExitLength, Body.Min.X - CornerRadius), Attachment.Y};
}

void FRoutingJob::FState::Step()
{
    switch (Phase)
    {
    case EPhase::Nodes:
        if (NextNode < Graph.Nodes.Num())
        {
            if (!AddNode(NextNode++)) { Phase = EPhase::Done; }
        }
        else { Phase = EPhase::Channels; }
        break;
    case EPhase::Channels:
        Obstacles.Finish();
        BodyX.Values.Reserve(Obstacles.Items.Num() * 2);
        BodyMinY.Values.Reserve(Obstacles.Items.Num()); BodyMaxY.Values.Reserve(Obstacles.Items.Num());
        for (const auto& O : Obstacles.Items)
        {
            BodyX.Values.Add(O.Box.Min.X - CornerRadius); BodyX.Values.Add(O.Box.Max.X + CornerRadius);
            BodyMinY.Values.Add(O.Box.Min.Y - CornerRadius); BodyMaxY.Values.Add(O.Box.Max.Y + CornerRadius);
        }
        BodyX.SortUnique(); BodyMinY.SortUnique(); BodyMaxY.SortUnique();
        Phase = EPhase::Edges;
        break;
    case EPhase::Edges:
        if (NextEdge < Graph.Edges.Num())
        {
            if (!AddEdge(NextEdge++)) { Phase = EPhase::Done; }
        }
        else { Phase = EPhase::Order; }
        break;
    case EPhase::Order:
        Order.Sort([this](int32 A, int32 B)
        {
            const auto& EA = Graph.Edges[A]; const auto& EB = Graph.Edges[B];
            if (EA.Kind != EB.Kind) { return EA.Kind < EB.Kind; }
            const auto& FA = Graph.Pins[EA.From]; const auto& FB = Graph.Pins[EB.From];
            const FGuid NA = Graph.Nodes[FA.Node].Geometry.Id, NB = Graph.Nodes[FB.Node].Geometry.Id;
            if (NA != NB) { return NA < NB; }
            if (FA.Ordinal != FB.Ordinal) { return FA.Ordinal < FB.Ordinal; }
            const auto& TA = Graph.Pins[EA.To]; const auto& TB = Graph.Pins[EB.To];
            const auto& TargetA = Graph.Nodes[TA.Node].Geometry.Id; const auto& TargetB = Graph.Nodes[TB.Node].Geometry.Id;
            if (TargetA != TargetB) { return TargetA < TargetB; }
            if (TA.Ordinal != TB.Ordinal) { return TA.Ordinal < TB.Ordinal; }
            return TA.Id < TB.Id;
        });
        Phase = EPhase::Routes;
        break;
    case EPhase::Routes:
        if (NextRoute < Order.Num()) { RouteOne(Order[NextRoute++]); }
        else { Phase = EPhase::Done; }
        break;
    case EPhase::Done:
        break;
    }
}

void FRoutingJob::FState::ReserveSegment(FVector2f A, FVector2f B, int32 From, int32 To, bool bFirst, bool bLast)
{
    const bool bHorizontal = A.Y == B.Y;
    const int32 Pin = bFirst ? From : To;
    const bool bTerminal = bHorizontal && A.X < B.X && (bFirst != bLast) && SharedPins[Pin];
    if (bTerminal)
    {
        if (const auto* Existing = TerminalReservations.Find(Pin))
        {
            auto& Segment = Reserved[Existing->Segment];
            if (A.X < Segment.A.X || B.X > Segment.B.X)
            {
                Segment.A.X = FMath::Min(A.X, Segment.A.X);
                Segment.B.X = FMath::Max(B.X, Segment.B.X);
                ReservedHorizontal.GrowHorizontal(Existing->Obstacle, SegmentBounds(Segment.A, Segment.B).ExpandBy(WireLaneSpacing));
            }
            return;
        }
    }
    const int32 Id = Reserved.Add({A, B, From, To, bFirst, bLast});
    auto& Index = bHorizontal ? ReservedHorizontal : ReservedVertical;
    const int32 Obstacle = Index.Add(SegmentBounds(A, B).ExpandBy(WireLaneSpacing), Id);
    if (bTerminal) { TerminalReservations.Add(Pin, {Id, Obstacle}); }
}

void FRoutingJob::FState::RouteOne(int32 Index)
{
    const auto& Edge = Graph.Edges[Index];
    const auto& From = Graph.Pins[Edge.From]; const auto& To = Graph.Pins[Edge.To];
    FWireRoute Route;
    Route.Key = {Graph.Nodes[From.Node].Geometry.Id, From.Id, Graph.Nodes[To.Node].Geometry.Id, To.Id};
    const FVector2f Start = FVector2f(Graph.Nodes[From.Node].Geometry.Position) + From.Offset.GetValue();
    const FVector2f End = FVector2f(Graph.Nodes[To.Node].Geometry.Position) + To.Offset.GetValue();
    const float OutLane = FMath::Min(FanOut[Edge.From]++, 64) * WireLaneSpacing;
    const float InLane = FMath::Min(FanIn[Edge.To]++, 64) * WireLaneSpacing;
    const FVector2f Exit = TerminalEnd(Edge.From, Start, true);
    const FVector2f Entry = TerminalEnd(Edge.To, End, false);
    auto ClearSegment = [&](FVector2f A, FVector2f B, bool bFirst, bool bLast)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ClearRouteSegment);
        if (!ValidPoint(A) || !ValidPoint(B)) { return false; }
        const int32 Ignore = bFirst ? From.Node : bLast ? To.Node : INDEX_NONE;
        if (!Obstacles.ClearLine(A, B, Ignore)) { return false; }
        if (A == B) { return true; }
        const auto& Reservations = A.Y == B.Y ? ReservedHorizontal : ReservedVertical;
        return Reservations.Query(SegmentBounds(A, B), [&](int32 ReservedId)
        {
            const auto& R = Reserved[Reservations.Items[ReservedId].Node];
            if (R.bPinApproach && !PendingPinApproaches[R.bFirst ? R.From : R.To]) { return true; }
            if (A.Y == B.Y && R.A.Y == R.B.Y &&
                ((R.bFirst && Edge.From == R.From && A.Y == Start.Y && FMath::Min(A.X, B.X) >= Start.X) ||
                 (R.bLast && Edge.To == R.To && A.Y == End.Y && FMath::Max(A.X, B.X) <= End.X))) { return true; }
            if (A.Y == B.Y && R.A.Y == R.B.Y && FMath::Abs(A.Y - R.A.Y) < WireLaneSpacing)
            {
                return FMath::Max(A.X, B.X) <= FMath::Min(R.A.X, R.B.X) || FMath::Min(A.X, B.X) >= FMath::Max(R.A.X, R.B.X);
            }
            if (A.X == B.X && R.A.X == R.B.X && FMath::Abs(A.X - R.A.X) < WireLaneSpacing)
            {
                return FMath::Max(A.Y, B.Y) <= FMath::Min(R.A.Y, R.B.Y) || FMath::Min(A.Y, B.Y) >= FMath::Max(R.A.Y, R.B.Y);
            }
            return true;
        });
    };
    auto Accept = [&](TConstArrayView<FVector2f> Candidate)
    {
        for (int32 I = 1; I < Candidate.Num(); ++I)
        {
            if (!ClearSegment(Candidate[I - 1], Candidate[I], I == 1, I == Candidate.Num() - 1)) { return false; }
        }
        Route.Points.Reset(Candidate.Num());
        Route.Points.Append(Candidate.GetData(), Candidate.Num());
        Simplify(Route.Points); return true;
    };
    bool bFound = false;
    if (Start.Y == End.Y && Start.X < End.X)
    {
        const FVector2f A(FMath::Min(Exit.X, End.X), Start.Y), B(FMath::Max(Entry.X, Start.X), End.Y);
        if (A.X <= B.X) { bFound = Accept({Start, A, B, End}); }
    }
    if (!bFound && (!ClearSegment(Start, Exit, true, false) || !ClearSegment(Entry, End, false, true)))
    {
        Route.Fallback = ERouteFallback::BlockedEndpoint;
    }
    else if (!bFound)
    {
        const float MiddleX = (Exit.X + Entry.X) * 0.5f;
        auto TryDogleg = [&](float X)
        {
            if (X < Exit.X || X > Entry.X) { return false; }
            return ClearSegment({X, Exit.Y}, {X, Entry.Y}, false, false) &&
                Accept({Start, Exit, {X, Exit.Y}, {X, Entry.Y}, Entry, End});
        };
        if (Exit.X <= Entry.X)
        {
            bFound = TryDogleg(MiddleX);
        }
        XChannels.Reset(); XChannels.Append({Exit.X, Entry.X});
        YChannels.Reset(); YChannels.Add((Start.Y + End.Y) * 0.5f);
        if (!bFound)
        {
            {
                TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BuildRouteChannels);
                BodyX.AppendNearest(XChannels, MiddleX); ReservedX.AppendNearest(XChannels, MiddleX);
                OrderChannels(XChannels, MiddleX);
            }
            for (float X : XChannels)
            {
                if (TryDogleg(X)) { bFound = true; break; }
            }
            if (!bFound && Exit.X <= Entry.X)
            {
                Frontier.Reset();
                for (int32 Pin : {Edge.From, Edge.To})
                {
                    const auto& Turns = PinTurns[Pin];
                    if (Turns.Min.IsSet())
                    {
                        for (float X : {Turns.Min.GetValue(), Turns.Max.GetValue()})
                        {
                            Frontier.Append({X, X - WireLaneSpacing, X + WireLaneSpacing});
                        }
                    }
                }
                OrderChannels(Frontier, MiddleX);
                for (float X : Frontier)
                {
                    if (TryDogleg(X)) { bFound = true; break; }
                }
            }
            if (!bFound)
            {
                TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BuildRouteChannels);
                const float MiddleY = (Start.Y + End.Y) * 0.5f;
                BodyMinY.AppendNearest(YChannels, MiddleY, -OutLane);
                BodyMaxY.AppendNearest(YChannels, MiddleY, InLane); ReservedY.AppendNearest(YChannels, MiddleY);
                OrderChannels(YChannels, MiddleY);
            }
            for (float Y : YChannels)
            {
                if (bFound) { break; }
                bFound = Accept({Start, Exit, {Exit.X, Y}, {Entry.X, Y}, Entry, End});
            }
            if (!bFound)
            {
                const auto Turns = [&](FVector2f Terminal, bool bOutput, TArray<FTurn>& Options)
                {
                    Options.Reset();
                    TurnChannels.Reset(); TurnChannels.Append(XChannels);
                    TurnChannels.AddUnique(Terminal.X - WireLaneSpacing);
                    TurnChannels.AddUnique(Terminal.X + WireLaneSpacing);
                    for (float Extension : {0.f, WireLaneSpacing})
                    for (float Offset : {0.f, -WireLaneSpacing, WireLaneSpacing})
                    {
                        if (Extension != 0 && Offset == 0) { continue; }
                        const FVector2f Stem(Terminal.X + (bOutput ? Extension : -Extension), Terminal.Y);
                        const FVector2f Step(Stem.X, Terminal.Y + Offset);
                        if (!ClearSegment(Terminal, Stem, false, false) || !ClearSegment(Stem, Step, false, false)) { continue; }
                        for (float X : TurnChannels)
                        {
                            if (Offset == 0 && (bOutput ? X < Terminal.X : X > Terminal.X)) { continue; }
                            if (Offset != 0 && X == Stem.X) { continue; }
                            const FVector2f Corner(X, Step.Y);
                            if (ClearSegment(Step, Corner, false, false))
                            {
                                Options.Add({Stem, Step, Corner, Extension + FMath::Abs(X - Stem.X) + FMath::Abs(Offset) + (Offset != 0 ? 2 * BendCost : 0)});
                            }
                        }
                    }
                };
                Turns(Exit, true, Departures); Turns(Entry, false, Approaches);
                double BestCost = TNumericLimits<double>::Max(); BestPoints.Reset();
                for (float Y : YChannels)
                {
                    const double MinimumLength = FMath::Abs(double(Start.X) - End.X) + FMath::Abs(double(Start.Y) - Y) + FMath::Abs(double(End.Y) - Y);
                    if (MinimumLength >= BestCost) { continue; }
                    const auto Pick = [&](const TArray<FTurn>& Options, float OtherX) -> const FTurn*
                    {
                        const FTurn* Best = nullptr; double Cost = TNumericLimits<double>::Max();
                        for (const auto& Option : Options)
                        {
                            const double CandidateCost = Option.LocalCost + FMath::Abs(double(Option.Corner.Y) - Y) + FMath::Abs(double(Option.Corner.X) - OtherX);
                            if (CandidateCost < Cost && ClearSegment(Option.Corner, {Option.Corner.X, Y}, false, false))
                            {
                                Best = &Option; Cost = CandidateCost;
                            }
                        }
                        return Best;
                    };
                    const FTurn* Departure = Pick(Departures, Entry.X);
                    if (!Departure) { continue; }
                    const FTurn* Approach = Pick(Approaches, Exit.X);
                    if (!Approach || !Accept({Start, Exit, Departure->Stem, Departure->Step, Departure->Corner,
                        {Departure->Corner.X, Y}, {Approach->Corner.X, Y}, Approach->Corner, Approach->Step, Approach->Stem, Entry, End})) { continue; }
                    double Cost = FMath::Max(0, Route.Points.Num() - 2) * BendCost;
                    for (int32 I = 1; I < Route.Points.Num(); ++I)
                    {
                        Cost += FMath::Abs(double(Route.Points[I].X) - Route.Points[I - 1].X) + FMath::Abs(double(Route.Points[I].Y) - Route.Points[I - 1].Y);
                    }
                    if (Cost < BestCost) { BestCost = Cost; BestPoints.Reset(); BestPoints.Append(Route.Points); }
                }
                Route.Points.Reset(); Route.Points.Append(BestPoints); bFound = !Route.Points.IsEmpty();
            }
            if (!bFound)
            {
                SearchWork.X.Reset(); SearchWork.X.Append(XChannels);
                SearchWork.Y.Reset(); SearchWork.Y.Append(YChannels);
                for (const auto& O : Obstacles.Items)
                {
                    SearchWork.X.Add(O.Box.Min.X); SearchWork.X.Add(O.Box.Max.X);
                    SearchWork.Y.Add(O.Box.Min.Y); SearchWork.Y.Add(O.Box.Max.Y);
                }
                if (SearchChannels(Exit, Entry, SearchWork,
                    [&](FVector2f A, FVector2f B) { return ClearSegment(A, B, false, false); }, Route.Search, SearchPath))
                {
                    SearchCandidate.Reset(SearchPath.Num() + 2);
                    SearchCandidate.Add(Start); SearchCandidate.Append(SearchPath); SearchCandidate.Add(End);
                    bFound = Accept(SearchCandidate);
                    if (bFound) { Route.Method = ERouteMethod::Search; }
                }
            }
            if (!bFound && Obstacles.Bounds.bIsValid)
            {
                for (float Y : {Obstacles.Bounds.Min.Y - ExitLength - OutLane, Obstacles.Bounds.Max.Y + ExitLength + InLane})
                {
                    if (Accept({Start, Exit, {Exit.X, Y}, {Entry.X, Y}, Entry, End}))
                    {
                        bFound = true; Route.Method = ERouteMethod::OuterLane; break;
                    }
                    float DepartureX = 0, ApproachX = 0;
                    bool bDeparture = false, bApproach = false;
                    for (float X : XChannels)
                    {
                        if (X >= Exit.X && ClearSegment(Exit, {X, Exit.Y}, false, false) &&
                            ClearSegment({X, Exit.Y}, {X, Y}, false, false))
                        {
                            DepartureX = X; bDeparture = true; break;
                        }
                    }
                    for (float X : XChannels)
                    {
                        if (X <= Entry.X && ClearSegment({X, Y}, {X, Entry.Y}, false, false) &&
                            ClearSegment({X, Entry.Y}, Entry, false, false))
                        {
                            ApproachX = X; bApproach = true; break;
                        }
                    }
                    if (bDeparture && bApproach && Accept({Start, Exit, {DepartureX, Exit.Y},
                        {DepartureX, Y}, {ApproachX, Y}, {ApproachX, Entry.Y}, Entry, End}))
                    {
                        bFound = true; Route.Method = ERouteMethod::OuterLane; break;
                    }
                }
            }
        }
        if (bFound && Route.Points.Num() == 6 && Exit.X <= Entry.X &&
            Route.Points[1].X + WireLaneSpacing <= Route.Points[3].X)
        {
            auto& Points = Route.Points;
            for (int32 Turn : {1, 3})
            {
                const float MinX = Turn == 1 ? Exit.X : Points[Turn - 1].X + WireLaneSpacing;
                const float MaxX = Turn == 3 ? Entry.X : Points[Turn + 2].X - WireLaneSpacing;
                for (float X : XChannels)
                {
                    if (FMath::Abs(X - MiddleX) >= FMath::Abs(Points[Turn].X - MiddleX)) { break; }
                    if (X < MinX || X > MaxX) { continue; }
                    const FVector2f A(X, Points[Turn].Y), B(X, Points[Turn + 1].Y);
                    if (ClearSegment(Points[Turn - 1], A, Turn == 1, false) &&
                        ClearSegment(A, B, false, false) &&
                        ClearSegment(B, Points[Turn + 2], false, Turn == 3))
                    {
                        Points[Turn] = A; Points[Turn + 1] = B; break;
                    }
                }
            }
        }
        if (!bFound)
        {
            Route.Fallback = Route.Search.bStateLimitReached ? ERouteFallback::SearchLimit :
                Route.Search.bChannelsTruncated ? ERouteFallback::ChannelLimit : ERouteFallback::NoChannel;
        }
    }
    if (bFound && Route.Points.Num() >= 2)
    {
        if (Route.Method == ERouteMethod::Native) { Route.Method = ERouteMethod::Simple; }
        StyleRoute(Route, Obstacles, Style);
        const float Drift = Clearance * 0.5f;
        const float StartMaxX = FMath::Min(Route.Curves[0].End.X,
            FMath::Max(Start.X + Drift, Route.Curves[0].End.X - ExitLength));
        const float EndMinX = FMath::Max(Route.Curves.Last().Start.X,
            FMath::Min(End.X - Drift, Route.Curves.Last().Start.X + ExitLength));
        FBox2f StartRegion({FMath::Min(float(Graph.Nodes[From.Node].Geometry.Position.X), Start.X),
            FMath::Min(float(Graph.Nodes[From.Node].Geometry.Position.Y), Start.Y - Drift)},
            {StartMaxX, Start.Y + Drift});
        FBox2f EndRegion({EndMinX, FMath::Min(float(Graph.Nodes[To.Node].Geometry.Position.Y), End.Y - Drift)},
            {FMath::Max(float(Graph.Nodes[To.Node].Geometry.Position.X) + Graph.Nodes[To.Node].Geometry.BodySize.X, End.X), End.Y + Drift});
        const bool bSingle = Route.Curves.Num() == 1;
        const FBox2f FullStart = StartRegion, FullEnd = EndRegion;
        bool bStartClear = false, bEndClear = false;
        for (int32 Attempt = 0; Attempt <= 4; ++Attempt)
        {
            FBox2f StartHull = StartRegion; StartHull += Route.Curves[0].End;
            FBox2f EndHull = EndRegion; EndHull += Route.Curves.Last().Start;
            if (bSingle) { StartHull += EndRegion; EndHull += StartRegion; }
            bStartClear = Obstacles.ClearBox(StartHull, From.Node, bSingle ? To.Node : INDEX_NONE);
            bEndClear = Obstacles.ClearBox(EndHull, To.Node, bSingle ? From.Node : INDEX_NONE);
            if ((bStartClear && bEndClear) || Attempt == 4) { break; }
            const float Radius = Drift / float(1 << Attempt);
            const auto Narrow = [Radius](const FBox2f& Full, FVector2f Pin)
            {
                return FBox2f({FMath::Max(Full.Min.X, Pin.X - Radius), FMath::Max(Full.Min.Y, Pin.Y - Radius)},
                    {FMath::Min(Full.Max.X, Pin.X + Radius), FMath::Min(Full.Max.Y, Pin.Y + Radius)});
            };
            if (!bStartClear) { StartRegion = Narrow(FullStart, Start); }
            if (!bEndClear) { EndRegion = Narrow(FullEnd, End); }
        }
        Route.StartRegion = bStartClear ? StartRegion : FBox2f(Start, Start);
        Route.EndRegion = bEndClear ? EndRegion : FBox2f(End, End);
        Route.Bounds += Route.StartRegion; Route.Bounds += Route.EndRegion;
        PendingPinApproaches[Edge.From] = false; PendingPinApproaches[Edge.To] = false;
        bool bFirstTurn = true;
        for (int32 I = 1; I < Route.Points.Num(); ++I)
        {
            const FVector2f A = Route.Points[I - 1], B = Route.Points[I];
            ReserveSegment(A, B, Edge.From, Edge.To, I == 1, I == Route.Points.Num() - 1);
            if (A.X == B.X)
            {
                ReservedX.Add(A.X - WireLaneSpacing); ReservedX.Add(A.X + WireLaneSpacing);
                if (bFirstTurn) { PinTurns[Edge.From].Add(A.X); bFirstTurn = false; }
                if (I + 1 == Route.Points.Num() - 1) { PinTurns[Edge.To].Add(A.X); }
            }
            if (A.Y == B.Y) { ReservedY.Add(A.Y - WireLaneSpacing); ReservedY.Add(A.Y + WireLaneSpacing); }
        }
    }
    else { ++Result.FallbackCount; }
    Result.Wires.Add(Route.Key, MoveTemp(Route));
}

FRoutingJob::FRoutingJob(FLayoutGraph Graph, EGlooPrintWireStyle Style)
    : State(MakeUnique<FState>(MoveTemp(Graph), Style)) {}
FRoutingJob::~FRoutingJob() = default;

bool FRoutingJob::Advance(double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_Routing);
    do { State->Step(); }
    while (State->Phase != FState::EPhase::Done && FPlatformTime::Seconds() < Deadline);
    return State->Phase == FState::EPhase::Done;
}

bool FRoutingJob::TakeResult(FRouteSet& OutRoutes, FString& OutReason, FLayoutGraph* OutSource)
{
    OutRoutes = {}; OutReason.Reset();
    if (OutSource) { *OutSource = {}; }
    if (State->Phase != FState::EPhase::Done || State->bTaken)
    {
        OutReason = TEXT("Routing result is not available."); return false;
    }
    OutReason = State->Reason;
    if (!OutReason.IsEmpty()) { return false; }
    State->bTaken = true; OutRoutes = MoveTemp(State->Result);
    if (OutSource) { *OutSource = MoveTemp(State->Graph); }
    return true;
}

int32 FRoutingJob::GetCompletedLinks() const { return State->NextRoute; }

bool ComputeRoutes(const FLayoutGraph& Graph, FRouteSet& OutRoutes, FString& OutReason, EGlooPrintWireStyle Style)
{
    FRoutingJob Job(Graph, Style);
    Job.Advance(TNumericLimits<double>::Max());
    return Job.TakeResult(OutRoutes, OutReason);
}

TUniquePtr<FRoutingJob> CreateLayoutRoutingJob(const FLayoutGraph& Graph, const FLayoutResult& Layout,
    FString& OutReason, EGlooPrintWireStyle Style)
{
    OutReason.Reset();
    if (Layout.Positions.Num() != Graph.Nodes.Num() || Layout.Sizes.Num() != Graph.Nodes.Num())
    {
        OutReason = TEXT("Proposed routing requires a complete layout."); return nullptr;
    }
    FLayoutGraph Proposed = Graph;
    for (int32 I = 0; I < Proposed.Nodes.Num(); ++I)
    {
        auto& Node = Proposed.Nodes[I];
        Node.Geometry.Position = Layout.Positions[I];
        if (Node.bComment) { Node.Geometry.BodySize = FVector2f(Layout.Sizes[I]); }
        else if (Layout.Sizes[I] != Node.OriginalSize)
        {
            OutReason = TEXT("Proposed routing cannot resize an ordinary node."); return nullptr;
        }
    }
    return MakeUnique<FRoutingJob>(MoveTemp(Proposed), Style);
}

bool ComputeLayoutRoutes(const FLayoutGraph& Graph, const FLayoutResult& Layout, FRouteSet& OutRoutes,
    FString& OutReason, EGlooPrintWireStyle Style)
{
    OutRoutes = {};
    auto Job = CreateLayoutRoutingJob(Graph, Layout, OutReason, Style);
    if (!Job) { return false; }
    Job->Advance(TNumericLimits<double>::Max());
    return Job->TakeResult(OutRoutes, OutReason);
}

void MeasureRouteCurve(FRouteCurve& Curve)
{
    Curve.Distances[0] = 0;
    FVector2f Previous = Curve.Start;
    for (int32 I = 1; I <= 16; ++I)
    {
        const FVector2f P = FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, I / 16.f);
        Curve.Distances[I] = Curve.Distances[I - 1] + (P - Previous).Size(); Previous = P;
    }
    Curve.Length = Curve.Distances[16];
}

FVector2f EvaluateRouteCurve(const FRouteCurve& Curve, float Distance, FVector2f* OutDirection)
{
    Distance = FMath::Clamp(Distance, 0.f, Curve.Length);
    int32 Sample = 1;
    while (Sample < 16 && Curve.Distances[Sample] < Distance) { ++Sample; }
    const float Span = Curve.Distances[Sample] - Curve.Distances[Sample - 1];
    const float Alpha = ((Sample - 1) + (Span > 0 ? (Distance - Curve.Distances[Sample - 1]) / Span : 0)) / 16.f;
    if (OutDirection)
    {
        *OutDirection = FMath::CubicInterpDerivative(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, Alpha).GetSafeNormal();
    }
    return FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, Alpha);
}

FVector2f EvaluateRoute(const FWireRoute& Route, float Distance, FVector2f* OutDirection)
{
    if (Route.Curves.IsEmpty()) { if (OutDirection) { *OutDirection = FVector2f(1, 0); } return FVector2f::ZeroVector; }
    Distance = FMath::Clamp(Distance, 0.f, Route.Length);
    const FRouteCurve* Curve = &Route.Curves.Last();
    for (const auto& Piece : Route.Curves)
    {
        Curve = &Piece;
        if (Distance <= Piece.Length) { break; }
        Distance -= Piece.Length;
    }
    return EvaluateRouteCurve(*Curve, Distance, OutDirection);
}
}
