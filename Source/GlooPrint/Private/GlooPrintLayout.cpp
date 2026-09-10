// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintLayout.h"
#include "GlooPrintRouting.h"

#include "Algo/Reverse.h"
#include "Algo/BinarySearch.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace GlooPrint
{
namespace
{
FMeasuredRect Translate(const FMeasuredRect& Rect, FVector2f Delta)
{
    return {Rect.Min + Delta, Rect.Max + Delta};
}

bool Contains(const FMeasuredRect& A, const FMeasuredRect& B)
{
    return A.Min.X <= B.Min.X && A.Min.Y <= B.Min.Y && A.Max.X >= B.Max.X && A.Max.Y >= B.Max.Y;
}

bool Intersects(const FMeasuredRect& A, const FMeasuredRect& B)
{
    return A.Min.X < B.Max.X && A.Max.X > B.Min.X && A.Min.Y < B.Max.Y && A.Max.Y > B.Min.Y;
}

bool HasBodyOverlap(const FLayoutGraph& Graph, const TArray<int32>& UnitOf, const TArray<FMeasuredRect>& Bodies)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ValidateLayoutBodies);
    struct FProjection
    {
        TArray<int32> Starts, Ends;
        uint64 Pairs = 0;
    };
    const auto Coordinate = [&Bodies](int32 Node, bool bX, bool bEnd)
    {
        const FVector2f& Point = bEnd ? Bodies[Node].Max : Bodies[Node].Min;
        return bX ? Point.X : Point.Y;
    };
    const auto Project = [&Graph, &Bodies, &Coordinate](bool bX)
    {
        FProjection Result;
        Result.Starts.Reserve(Bodies.Num());
        for (int32 I = 0; I < Bodies.Num(); ++I) { if (!Graph.Nodes[I].bComment) { Result.Starts.Add(I); } }
        Result.Ends = Result.Starts;
        Result.Starts.Sort([&](int32 A, int32 B)
        {
            const float From = Coordinate(A, bX, false), To = Coordinate(B, bX, false);
            return From != To ? From < To : A < B;
        });
        Result.Ends.Sort([&](int32 A, int32 B)
        {
            const float From = Coordinate(A, bX, true), To = Coordinate(B, bX, true);
            return From != To ? From < To : A < B;
        });
        int32 End = 0;
        for (int32 Start = 0; Start < Result.Starts.Num(); ++Start)
        {
            const float Min = Coordinate(Result.Starts[Start], bX, false);
            while (End < Result.Ends.Num() && Coordinate(Result.Ends[End], bX, true) < Min) { ++End; }
            Result.Pairs += uint64(Start - End);
        }
        return Result;
    };
    FProjection Sweep = Project(true);
    if (Sweep.Pairs == 0) { return false; }
    FProjection Vertical = Project(false);
    if (Vertical.Pairs == 0) { return false; }
    const bool bX = Sweep.Pairs <= Vertical.Pairs;
    if (!bX) { Sweep = MoveTemp(Vertical); }
    TArray<int32> Active, Slots;
    Active.Reserve(Sweep.Starts.Num()); Slots.Init(INDEX_NONE, Bodies.Num());
    int32 End = 0;
    for (const int32 I : Sweep.Starts)
    {
        const float Min = Coordinate(I, bX, false);
        while (End < Sweep.Ends.Num() && Coordinate(Sweep.Ends[End], bX, true) < Min)
        {
            const int32 Expired = Sweep.Ends[End++], Slot = Slots[Expired];
            const int32 Last = Active.Pop(EAllowShrinking::No);
            if (Slot < Active.Num()) { Active[Slot] = Last; Slots[Last] = Slot; }
            Slots[Expired] = INDEX_NONE;
        }
        for (const int32 J : Active)
        {
            if (UnitOf[I] != UnitOf[J] && Intersects(Bodies[I], Bodies[J])) { return true; }
        }
        Slots[I] = Active.Add(I);
    }
    return false;
}

FMeasuredRect Union(const FMeasuredRect& A, const FMeasuredRect& B)
{
    return {FVector2f(FMath::Min(A.Min.X, B.Min.X), FMath::Min(A.Min.Y, B.Min.Y)),
        FVector2f(FMath::Max(A.Max.X, B.Max.X), FMath::Max(A.Max.Y, B.Max.Y))};
}

struct FDisjointSets
{
    TArray<int32> Parent;
    explicit FDisjointSets(int32 Count)
    {
        Parent.SetNumUninitialized(Count);
        for (int32 I = 0; I < Count; ++I) { Parent[I] = I; }
    }
    int32 Find(int32 I)
    {
        while (Parent[I] != I) { Parent[I] = Parent[Parent[I]]; I = Parent[I]; }
        return I;
    }
    void Join(int32 A, int32 B)
    {
        A = Find(A); B = Find(B);
        Parent[FMath::Max(A, B)] = FMath::Min(A, B);
    }
};

struct FMember
{
    int32 Node = INDEX_NONE;
    FVector2f Offset = FVector2f::ZeroVector;
};

struct FUnit
{
    int32 Key = INDEX_NONE;
    int32 Comment = INDEX_NONE;
    int32 Parent = INDEX_NONE;
    TArray<int32> Children;
    TArray<FMember, TInlineAllocator<1>> Members;
    FMeasuredRect Bounds;
    FVector2f Position = FVector2f::ZeroVector;
    FVector2f Size = FVector2f::ZeroVector;
    bool bFixed = false;
    bool bRigid = false;
};

struct FProjectedEdge
{
    int32 From;
    int32 To;
    float FromY;
    float ToY;
    int32 PinOrdinal;
    bool bExecution;
};

struct FBoundaryExecution
{
    int32 Column;
    float Y;
    bool bOutput;
};

bool EarlierOutput(const FProjectedEdge& A, const FProjectedEdge& B)
{
    if (A.FromY != B.FromY) { return A.FromY < B.FromY; }
    if (A.PinOrdinal != B.PinOrdinal) { return A.PinOrdinal < B.PinOrdinal; }
    return A.To < B.To;
}

void PlaceExecutionChains(TArray<FUnit>& Units, const TArray<int32>& Children, const TArray<FProjectedEdge>& Edges,
    const TArray<int32>& Layer, const TArray<int32>& Order, const TArray<int32>& Rank, const TArray<float>& OrderY,
    const TBitArray<>& Pure, FDisjointSets& Islands, float Spacing)
{
    const int32 Count = Children.Num();
    TArray<int32> MainInput, MainOutput, ChainOf;
    MainInput.Init(INDEX_NONE, Count); MainOutput.Init(INDEX_NONE, Count); ChainOf.Init(INDEX_NONE, Count);
    TArray<float> Offset;
    Offset.Init(0, Count);
    for (int32 I = 0; I < Edges.Num(); ++I)
    {
        const auto& Edge = Edges[I];
        if (!Edge.bExecution || Rank[Edge.From] >= Rank[Edge.To] || Layer[Edge.From] >= Layer[Edge.To]) { continue; }
        const int32 In = MainInput[Edge.To], Out = MainOutput[Edge.From];
        if (In == INDEX_NONE || Edge.From < Edges[In].From || (Edge.From == Edges[In].From && EarlierOutput(Edge, Edges[In])))
        {
            MainInput[Edge.To] = I;
        }
        if (Out == INDEX_NONE || EarlierOutput(Edge, Edges[Out]))
        {
            MainOutput[Edge.From] = I;
        }
    }
    struct FChain
    {
        int32 Root = INDEX_NONE;
        TArray<int32> Members, Children;
        float Min = MAX_flt;
    };
    TArray<FChain> Chains;
    TArray<int32> Roots;
    for (const int32 Node : Order)
    {
        if (Pure[Node]) { continue; }
        const int32 In = MainInput[Node];
        if (In != INDEX_NONE && MainOutput[Edges[In].From] == In)
        {
            ChainOf[Node] = ChainOf[Edges[In].From];
            Offset[Node] = Offset[Edges[In].From] + Edges[In].FromY - Edges[In].ToY;
        }
        else
        {
            const int32 Id = Chains.AddDefaulted(); ChainOf[Node] = Id; Chains[Id].Root = Node;
            if (In == INDEX_NONE) { Roots.Add(Id); }
            else { Chains[ChainOf[Edges[In].From]].Children.Add(Id); }
        }
        auto& Chain = Chains[ChainOf[Node]];
        Chain.Members.Add(Node);
        const auto& Bounds = Units[Children[Node]].Bounds;
        Chain.Min = FMath::Min(Chain.Min, Offset[Node] + Bounds.Min.Y);
    }
    Roots.Sort([&](int32 A, int32 B)
    {
        const int32 RA = Chains[A].Root, RB = Chains[B].Root;
        const int32 IA = Islands.Find(RA), IB = Islands.Find(RB);
        if (IA != IB) { return IA < IB; }
        if (OrderY[RA] != OrderY[RB]) { return OrderY[RA] < OrderY[RB]; }
        return RA < RB;
    });
    for (auto& Chain : Chains)
    {
        Chain.Children.Sort([&](int32 A, int32 B)
        {
            const auto& EA = Edges[MainInput[Chains[A].Root]];
            const auto& EB = Edges[MainInput[Chains[B].Root]];
            if (Layer[EA.From] != Layer[EB.From]) { return Layer[EA.From] > Layer[EB.From]; }
            if (EA.From != EB.From) { return EA.From < EB.From; }
            return EarlierOutput(EA, EB);
        });
    }
    Algo::Reverse(Roots);
    TArray<float> ColumnBottom, BranchFloor;
    TArray<int32> ColumnIsland;
    ColumnBottom.Init(0, Count); ColumnIsland.Init(INDEX_NONE, Count);
    BranchFloor.Init(-MAX_flt, Count);
    while (!Roots.IsEmpty())
    {
        const auto& Chain = Chains[Roots.Pop(EAllowShrinking::No)];
        const int32 Island = Islands.Find(Chain.Root);
        float Base = -Chain.Min;
        const int32 In = MainInput[Chain.Root];
        if (In != INDEX_NONE)
        {
            const auto& Edge = Edges[In];
            Base = FMath::Max(Base, Units[Children[Edge.From]].Position.Y + Edge.FromY - Edge.ToY);
            Base = FMath::Max(Base, BranchFloor[Edge.From] - Edge.ToY);
        }
        for (const int32 Node : Chain.Members)
        {
            const auto& Unit = Units[Children[Node]];
            const float Bottom = ColumnIsland[Layer[Node]] == Island ? ColumnBottom[Layer[Node]] : 0;
            Base = FMath::Max(Base, Bottom - FMath::RoundToFloat(Offset[Node]) - Unit.Bounds.Min.Y);
        }
        Base = FMath::CeilToFloat(Base);
        for (const int32 Node : Chain.Members)
        {
            auto& Unit = Units[Children[Node]];
            Unit.Position.Y = Base + FMath::RoundToFloat(Offset[Node]);
            ColumnBottom[Layer[Node]] = FMath::CeilToFloat(Unit.Position.Y + Unit.Bounds.Max.Y + Spacing);
            ColumnIsland[Layer[Node]] = Island;
        }
        for (const int32 Node : Chain.Members)
        {
            const int32 Out = MainOutput[Node];
            if (Out != INDEX_NONE && ChainOf[Edges[Out].To] == ChainOf[Node])
            {
                BranchFloor[Node] = Units[Children[Edges[Out].To]].Position.Y + Edges[Out].ToY + Spacing;
            }
        }
        if (In != INDEX_NONE)
        {
            BranchFloor[Edges[In].From] = Units[Children[Chain.Root]].Position.Y + Edges[In].ToY + Spacing;
        }
        for (int32 I = Chain.Children.Num() - 1; I >= 0; --I) { Roots.Add(Chain.Children[I]); }
    }
}

TArray<TOptional<float>> ExternalConsumerOrder(const FLayoutGraph& Graph, const TArray<int32>& Owner,
    const TArray<int32>& BoundaryData, const TArray<FProjectedEdge>& Edges, const TArray<TArray<int32>>& Outgoing,
    const TArray<int32>& Order, const TArray<int32>& Rank, const TBitArray<>& Pure, FDisjointSets& Dependencies)
{
    const int32 Count = Order.Num();
    TArray<TOptional<float>> Scores; Scores.SetNum(Count);
    if (BoundaryData.IsEmpty()) { return Scores; }
    TArray<int32> Consumer, Resolved, Path;
    Consumer.Init(INDEX_NONE, Count); Resolved.Init(-2, Graph.Nodes.Num());
    TBitArray<> Ambiguous(false, Count);
    TArray<TArray<float>> Targets; Targets.SetNum(Count);
    const auto TerminalPin = [&](int32 Pin)
    {
        Path.Reset();
        while (Owner[Graph.Pins[Pin].Node] == INDEX_NONE && Graph.Nodes[Graph.Pins[Pin].Node].bReroute)
        {
            const int32 Node = Graph.Pins[Pin].Node;
            if (Resolved[Node] != -2) { Pin = Resolved[Node]; break; }
            Resolved[Node] = INDEX_NONE; Path.Add(Node);
            const auto& Links = Graph.Nodes[Node].Outgoing;
            if (Links.Num() != 1 || Graph.Edges[Links[0]].Kind != ELinkKind::Data) { Pin = INDEX_NONE; break; }
            Pin = Graph.Edges[Links[0]].To;
        }
        if (Pin != INDEX_NONE && Owner[Graph.Pins[Pin].Node] != INDEX_NONE) { Pin = INDEX_NONE; }
        for (const int32 Node : Path) { Resolved[Node] = Pin; }
        return Pin;
    };
    for (const int32 E : BoundaryData)
    {
        const auto& Edge = Graph.Edges[E]; const int32 Node = Owner[Graph.Pins[Edge.From].Node];
        if (!Pure[Node]) { continue; }
        const int32 Group = Dependencies.Find(Node), Pin = TerminalPin(Edge.To);
        if (Pin == INDEX_NONE) { Ambiguous[Group] = true; continue; }
        const auto& To = Graph.Pins[Pin];
        if (Consumer[Group] == INDEX_NONE) { Consumer[Group] = To.Node; }
        else if (Consumer[Group] != To.Node) { Ambiguous[Group] = true; }
        Targets[Node].Add(To.Offset.GetValue().Y);
    }
    for (int32 I = Order.Num() - 1; I >= 0; --I)
    {
        const int32 Node = Order[I], Group = Dependencies.Find(Node);
        if (!Pure[Node] || Consumer[Group] == INDEX_NONE || Ambiguous[Group]) { continue; }
        for (const int32 E : Outgoing[Node])
        {
            const int32 Next = Edges[E].To;
            if (!Pure[Next] || Rank[Next] <= Rank[Node]) { Ambiguous[Group] = true; continue; }
            if (Scores[Next].IsSet()) { Targets[Node].Add(Scores[Next].GetValue()); }
        }
        auto& Values = Targets[Node];
        if (Values.IsEmpty()) { Ambiguous[Group] = true; continue; }
        Values.Sort(); Scores[Node] = (Values[Values.Num() / 2] + Values[(Values.Num() - 1) / 2]) * 0.5f;
    }
    for (int32 I = 0; I < Count; ++I)
    {
        if (Ambiguous[Dependencies.Find(I)]) { Scores[I].Reset(); }
    }
    return Scores;
}

void PlacePureDependencies(TArray<FUnit>& Units, const TArray<int32>& Children,
    const TArray<FProjectedEdge>& Edges, const TArray<TArray<int32>>& Outgoing,
    const TArray<TArray<int32>>& Layers, const TArray<int32>& Layer, const TArray<float>& OrderY,
    const TBitArray<>& Pure, FDisjointSets& Dependencies, TArray<int32>& GroupOf, float Spacing,
    const TArray<FBoundaryExecution>& Boundaries, const TArray<TOptional<float>>& ConsumerOrder)
{
    struct FProfile
    {
        int32 Column;
        float Min, Max;
        FVector2f Left = FVector2f::ZeroVector, Right = FVector2f::ZeroVector;
    };
    struct FGroup
    {
        int32 Key;
        TArray<int32> Members;
        TArray<FProfile> Profile;
        float Min = MAX_flt;
    };
    TArray<FGroup> Groups;
    TArray<TArray<FVector2f>> Occupied;
    Occupied.SetNum(Layers.Num());
    TArray<TPair<int32, float>> Inputs;
    TArray<float> Desired, Corrections;
    for (int32 L = Layers.Num() - 1; L >= 0; --L)
    {
        Inputs.Reset();
        for (const int32 Node : Layers[L])
        {
            const auto& Unit = Units[Children[Node]];
            if (!Pure[Node])
            {
                Occupied[L].Add({Unit.Position.Y + Unit.Bounds.Min.Y, Unit.Position.Y + Unit.Bounds.Max.Y});
                continue;
            }
            Desired.Reset();
            for (const int32 E : Outgoing[Node])
            {
                const auto& Edge = Edges[E];
                if (Layer[Edge.To] > L)
                {
                    Desired.Add(Units[Children[Edge.To]].Position.Y + Edge.ToY - Edge.FromY);
                }
            }
            Desired.Sort();
            Inputs.Emplace(Node, Desired.IsEmpty() ? 0.f : Desired[Desired.Num() / 2]);
        }
        Inputs.Sort([&](const auto& A, const auto& B)
        {
            const int32 GA = Dependencies.Find(A.Key), GB = Dependencies.Find(B.Key);
            if (GA != GB) { return GA < GB; }
            const float OA = ConsumerOrder[A.Key].Get(MAX_flt), OB = ConsumerOrder[B.Key].Get(MAX_flt);
            if (OA != OB) { return OA < OB; }
            if (A.Value != B.Value) { return A.Value < B.Value; }
            if (OrderY[A.Key] != OrderY[B.Key]) { return OrderY[A.Key] < OrderY[B.Key]; }
            return A.Key < B.Key;
        });
        for (int32 Start = 0; Start < Inputs.Num();)
        {
            const int32 Key = Dependencies.Find(Inputs[Start].Key);
            int32 End = Start + 1;
            while (End < Inputs.Num() && Dependencies.Find(Inputs[End].Key) == Key) { ++End; }
            if (GroupOf[Key] == INDEX_NONE) { GroupOf[Key] = Groups.Add({Key}); }
            auto& Group = Groups[GroupOf[Key]];
            float Cursor = -MAX_flt;
            Corrections.Reset();
            for (int32 I = Start; I < End; ++I)
            {
                auto& Unit = Units[Children[Inputs[I].Key]];
                Unit.Position.Y = FMath::CeilToFloat(FMath::Max(Inputs[I].Value, Cursor - Unit.Bounds.Min.Y));
                Corrections.Add(Inputs[I].Value - Unit.Position.Y);
                Cursor = Unit.Position.Y + Unit.Bounds.Max.Y + Spacing;
            }
            Corrections.Sort();
            const int32 Middle = Corrections.Num() / 2;
            const float Shift = FMath::RoundToFloat((Corrections[Middle] + Corrections[(Corrections.Num() - 1) / 2]) * 0.5f);
            FProfile Profile{L, MAX_flt, -MAX_flt};
            for (int32 I = Start; I < End; ++I)
            {
                const int32 Node = Inputs[I].Key;
                auto& Unit = Units[Children[Node]];
                Unit.Position.Y += Shift;
                Profile.Min = FMath::Min(Profile.Min, Unit.Position.Y + Unit.Bounds.Min.Y);
                Profile.Max = FMath::Max(Profile.Max, Unit.Position.Y + Unit.Bounds.Max.Y);
                Group.Members.Add(Node);
            }
            Group.Min = FMath::Min(Group.Min, Profile.Min);
            Group.Profile.Add(Profile);
            Start = End;
        }
    }
    Groups.Sort([](const auto& A, const auto& B) { return A.Min != B.Min ? A.Min < B.Min : A.Key < B.Key; });
    TArray<FVector2f> Forbidden;
    for (auto& Group : Groups)
    {
        Forbidden.Reset();
        for (const auto& Profile : Group.Profile)
        {
            for (const auto& Obstacle : Occupied[Profile.Column])
            {
                const float Min = FMath::FloorToFloat(Obstacle.X - Spacing - Profile.Max) + 1;
                const float Max = FMath::CeilToFloat(Obstacle.Y + Spacing - Profile.Min) - 1;
                if (Min <= Max) { Forbidden.Add({Min, Max}); }
            }
        }
        if (!Boundaries.IsEmpty())
        {
            Group.Profile.Sort([](const auto& A, const auto& B) { return A.Column < B.Column; });
            FVector2f Left(MAX_flt, -MAX_flt), Right(MAX_flt, -MAX_flt);
            for (int32 I = 0; I < Group.Profile.Num(); ++I)
            {
                auto& A = Group.Profile[I]; auto& B = Group.Profile[Group.Profile.Num() - 1 - I];
                Left = {FMath::Min(Left.X, A.Min), FMath::Max(Left.Y, A.Max)}; A.Left = Left;
                Right = {FMath::Min(Right.X, B.Min), FMath::Max(Right.Y, B.Max)}; B.Right = Right;
            }
            for (const auto& Boundary : Boundaries)
            {
                const int32 Index = Boundary.bOutput ? Algo::UpperBoundBy(Group.Profile, Boundary.Column, &FProfile::Column)
                    : Algo::LowerBoundBy(Group.Profile, Boundary.Column, &FProfile::Column) - 1;
                if (!Group.Profile.IsValidIndex(Index)) { continue; }
                const FVector2f Span = Boundary.bOutput ? Group.Profile[Index].Right : Group.Profile[Index].Left;
                const float Min = FMath::FloorToFloat(Boundary.Y - WireNodeClearance - Span.Y) + 1;
                const float Max = FMath::CeilToFloat(Boundary.Y + WireNodeClearance - Span.X) - 1;
                if (Min <= Max) { Forbidden.Add({Min, Max}); }
            }
        }
        Forbidden.Sort([](const auto& A, const auto& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
        Desired.Reset();
        for (const int32 Node : Group.Members)
        {
            for (const int32 E : Outgoing[Node])
            {
                const auto& Edge = Edges[E];
                if (!Pure[Edge.To])
                {
                    Desired.Add(Units[Children[Edge.To]].Position.Y + Edge.ToY - Units[Children[Node]].Position.Y - Edge.FromY);
                }
            }
        }
        Desired.Sort();
        const float Preferred = Desired.IsEmpty() ? 0.f : FMath::RoundToFloat(
            (Desired[Desired.Num() / 2] + Desired[(Desired.Num() - 1) / 2]) * 0.5f);
        const float Floor = FMath::CeilToFloat(-Group.Min);
        float Below = FMath::Max(Preferred, Floor), Above = Below;
        for (const auto& Interval : Forbidden)
        {
            if (Below >= Interval.X && Below <= Interval.Y) { Below = Interval.Y + 1; }
        }
        for (int32 I = Forbidden.Num() - 1; I >= 0; --I)
        {
            const auto& Interval = Forbidden[I];
            if (Above >= Interval.X && Above <= Interval.Y) { Above = Interval.X - 1; }
        }
        const float Shift = Above >= Floor && FMath::Abs(Above - Preferred) <= FMath::Abs(Below - Preferred) ? Above : Below;
        for (const int32 Node : Group.Members) { Units[Children[Node]].Position.Y += Shift; }
        for (const auto& Profile : Group.Profile)
        {
            Occupied[Profile.Column].Add({Profile.Min + Shift, Profile.Max + Shift});
        }
    }
}

struct FLayoutScratch
{
    TArray<FMember> Members;
    TArray<TPair<int32, FVector2f>> Stack;
    TArray<float> Neighbors;
};

void Collect(const TArray<FUnit>& Units, int32 Root, FLayoutScratch& Scratch)
{
    auto& OutMembers = Scratch.Members; OutMembers.Reset();
    auto& Stack = Scratch.Stack; Stack.Reset();
    Stack.Emplace(Root, FVector2f::ZeroVector);
    while (!Stack.IsEmpty())
    {
        const auto Current = Stack.Pop(EAllowShrinking::No);
        const FUnit& Unit = Units[Current.Key];
        if (Unit.Comment != INDEX_NONE) { OutMembers.Add({Unit.Comment, Current.Value}); }
        for (const FMember& Member : Unit.Members)
        {
            OutMembers.Add({Member.Node, Member.Offset + Current.Value});
        }
        for (const int32 Child : Unit.Children)
        {
            Stack.Emplace(Child, Current.Value + Units[Child].Position);
        }
    }
}

uint64 CountOrderingCrossings(const TArray<FUnit>& Units, const TArray<int32>& Children,
    const TArray<FProjectedEdge>& Edges, const TArray<int32>& Layer)
{
    struct FAttachmentOrder { uint64 Columns; float From, To; };
    TArray<FAttachmentOrder> Links;
    Links.Reserve(Edges.Num());
    for (const auto& Edge : Edges)
    {
        if (Layer[Edge.From] >= Layer[Edge.To]) { continue; }
        const float From = Units[Children[Edge.From]].Position.Y + Edge.FromY;
        const float To = Units[Children[Edge.To]].Position.Y + Edge.ToY;
        if (!FMath::IsFinite(From) || !FMath::IsFinite(To)) { return MAX_uint64; }
        Links.Add({(uint64(uint32(Layer[Edge.From])) << 32) | uint32(Layer[Edge.To]), From, To});
    }
    Links.Sort([](const auto& A, const auto& B)
    {
        if (A.Columns != B.Columns) { return A.Columns < B.Columns; }
        return A.From != B.From ? A.From < B.From : A.To < B.To;
    });
    uint64 Crossings = 0;
    TArray<float> Ends;
    TArray<int32> Tree;
    for (int32 Start = 0; Start < Links.Num();)
    {
        int32 End = Start + 1;
        while (End < Links.Num() && Links[End].Columns == Links[Start].Columns) { ++End; }
        if (End - Start > 1)
        {
            Ends.Reset(End - Start);
            for (int32 I = Start; I < End; ++I) { Ends.Add(Links[I].To); }
            Ends.Sort(); Tree.Init(0, Ends.Num() + 1);
            int32 Seen = 0;
            for (int32 First = Start; First < End;)
            {
                int32 Last = First + 1;
                while (Last < End && Links[Last].From == Links[First].From) { ++Last; }
                for (int32 I = First; I < Last; ++I)
                {
                    int32 Prefix = 0;
                    for (int32 Index = Algo::LowerBound(Ends, Links[I].To) + 1; Index > 0; Index -= Index & -Index) { Prefix += Tree[Index]; }
                    Crossings += uint64(Seen - Prefix);
                }
                for (int32 I = First; I < Last; ++I)
                {
                    for (int32 Index = Algo::LowerBound(Ends, Links[I].To) + 1; Index < Tree.Num(); Index += Index & -Index) { ++Tree[Index]; }
                    ++Seen;
                }
                First = Last;
            }
        }
        Start = End;
    }
    return Crossings;
}

TOptional<FMeasuredRect> PlaceChildren(const FLayoutGraph& Graph, const FLayoutSettings& Settings,
    TArray<FUnit>& Units, TArray<int32> Children, int32 OrderingSweeps, uint64& OutCrossings, FLayoutScratch& Scratch)
{
    Children.RemoveAll([&](int32 I) { return Units[I].bFixed; });
    Children.Sort([&](int32 A, int32 B) { return Units[A].Key < Units[B].Key; });
    const int32 Count = Children.Num();
    if (!Count) { return {}; }

    TArray<int32> Owner;
    Owner.Init(INDEX_NONE, Graph.Nodes.Num());
    TBitArray<> Pure(false, Count);
    TArray<FVector2f> Offset;
    Offset.SetNumZeroed(Graph.Nodes.Num());
    TArray<int32> Present;
    for (int32 I = 0; I < Count; ++I)
    {
        const FUnit& Unit = Units[Children[I]];
        Pure[I] = !Unit.bRigid && (Unit.Comment != INDEX_NONE || Unit.Members.Num() == 1);
        Collect(Units, Children[I], Scratch);
        for (const FMember& Member : Scratch.Members)
        {
            Owner[Member.Node] = I;
            Offset[Member.Node] = Member.Offset;
            Present.Add(Member.Node);
        }
    }
    for (const FLayoutPin& Pin : Graph.Pins)
    {
        if (Pin.Kind == ELinkKind::Execution && Owner.IsValidIndex(Pin.Node) && Owner[Pin.Node] != INDEX_NONE)
        {
            Pure[Owner[Pin.Node]] = false;
        }
    }
    TArray<FProjectedEdge> Edges;
    TArray<TArray<int32>> Outgoing, Incoming;
    Outgoing.SetNum(Count); Incoming.SetNum(Count);
    FDisjointSets Islands(Count);
    TArray<int32> ReturnCounts;
    ReturnCounts.Init(0, Count);
    TArray<int32> BoundaryPins, BoundaryData;
    for (const int32 Node : Present)
    {
        for (const int32 EdgeIndex : Graph.Nodes[Node].Outgoing)
        {
            const FLayoutEdge& Edge = Graph.Edges[EdgeIndex];
            const FLayoutPin& From = Graph.Pins[Edge.From];
            const FLayoutPin& To = Graph.Pins[Edge.To];
            const int32 A = Owner[From.Node], B = Owner[To.Node];
            if (B == INDEX_NONE)
            {
                if (Edge.Kind == ELinkKind::Execution) { BoundaryPins.Add(Edge.From); }
                else if (Edge.Kind == ELinkKind::Data) { BoundaryData.Add(EdgeIndex); }
                continue;
            }
            if (A == B)
            {
                if (Units[Children[A]].Comment == INDEX_NONE &&
                    Offset[From.Node].X + From.Offset.GetValue().X >= Offset[To.Node].X + To.Offset.GetValue().X)
                {
                    ++ReturnCounts[A];
                }
                continue;
            }
            const int32 Index = Edges.Add({A, B, Offset[From.Node].Y + From.Offset.GetValue().Y,
                Offset[To.Node].Y + To.Offset.GetValue().Y, From.Ordinal, Edge.Kind == ELinkKind::Execution});
            Outgoing[A].Add(Index); Incoming[B].Add(Index);
            if (Edge.Kind == ELinkKind::Execution) { Pure[A] = false; Pure[B] = false; }
            Islands.Join(A, B);
        }
        for (const int32 EdgeIndex : Graph.Nodes[Node].Incoming)
        {
            const auto& Edge = Graph.Edges[EdgeIndex];
            if (Edge.Kind == ELinkKind::Execution && Owner[Graph.Pins[Edge.From].Node] == INDEX_NONE) { BoundaryPins.Add(Edge.To); }
        }
    }

    FDisjointSets Dependencies(Count);
    for (const auto& Edge : Edges)
    {
        if (Pure[Edge.From] && Pure[Edge.To]) { Dependencies.Join(Edge.From, Edge.To); }
    }
    TBitArray<> CommentDependencies(false, Count);
    for (int32 I = 0; I < Count; ++I)
    {
        if (Pure[I] && Units[Children[I]].Comment != INDEX_NONE) { CommentDependencies[Dependencies.Find(I)] = true; }
    }

    TBitArray<> Visited(false, Count);
    TArray<int32> Finish;
    TArray<FIntPoint> Stack;
    for (int32 Start = 0; Start < Count; ++Start)
    {
        if (Visited[Start]) { continue; }
        Visited[Start] = true;
        Stack.Emplace(Start, 0);
        while (!Stack.IsEmpty())
        {
            FIntPoint& Top = Stack.Last();
            if (Top.Y == Outgoing[Top.X].Num())
            {
                Finish.Add(Top.X); Stack.Pop(EAllowShrinking::No); continue;
            }
            const int32 Next = Edges[Outgoing[Top.X][Top.Y++]].To;
            if (!Visited[Next]) { Visited[Next] = true; Stack.Emplace(Next, 0); }
        }
    }
    TArray<int32> Component;
    Component.Init(INDEX_NONE, Count);
    TArray<TArray<int32>> Components;
    TArray<int32> Pending;
    for (int32 I = Finish.Num() - 1; I >= 0; --I)
    {
        const int32 Start = Finish[I];
        if (Component[Start] != INDEX_NONE) { continue; }
        const int32 Id = Components.AddDefaulted();
        Component[Start] = Id; Pending.Add(Start);
        while (!Pending.IsEmpty())
        {
            const int32 Node = Pending.Pop(EAllowShrinking::No);
            Components[Id].Add(Node);
            for (const int32 Edge : Incoming[Node])
            {
                const int32 Next = Edges[Edge].From;
                if (Component[Next] == INDEX_NONE) { Component[Next] = Id; Pending.Add(Next); }
            }
        }
        Components[Id].Sort();
    }
    TArray<int32> Order, Rank, Layer;
    Rank.SetNumUninitialized(Count); Layer.Init(0, Count);
    for (const TArray<int32>& Members : Components) { Order.Append(Members); }
    for (int32 I = 0; I < Order.Num(); ++I) { Rank[Order[I]] = I; }
    for (const int32 Node : Order)
    {
        for (const int32 E : Outgoing[Node])
        {
            const auto& Edge = Edges[E]; const int32 Next = Edge.To;
            const bool bFlowValue = !Edge.bExecution && !Pure[Node] && Pure[Next] &&
                CommentDependencies[Dependencies.Find(Next)] && Component[Node] != Component[Next];
            if (Rank[Next] > Rank[Node] && !bFlowValue) { Layer[Next] = FMath::Max(Layer[Next], Layer[Node] + 1); }
        }
    }
    for (int32 I = Order.Num() - 1; I >= 0; --I)
    {
        const int32 Node = Order[I];
        if (!Pure[Node] || Components[Component[Node]].Num() != 1) { continue; }
        int32 Latest = MAX_int32;
        for (const int32 E : Outgoing[Node])
        {
            const int32 Next = Edges[E].To;
            if (Rank[Next] > Rank[Node]) { Latest = FMath::Min(Latest, Layer[Next] - 1); }
        }
        if (Latest != MAX_int32) { Layer[Node] = FMath::Max(Layer[Node], Latest); }
    }

    TArray<TArray<int32>> IslandNodes;
    IslandNodes.SetNum(Count);
    for (int32 I = 0; I < Count; ++I) { IslandNodes[Islands.Find(I)].Add(I); }
    for (const auto& Edge : Edges)
    {
        if (Rank[Edge.From] >= Rank[Edge.To] || Layer[Edge.From] >= Layer[Edge.To]) { ++ReturnCounts[Edge.From]; }
    }
    TArray<float> OrderY, Scores, PinY;
    TArray<int32> PinOrder;
    OrderY.Init(0, Count); Scores.SetNumUninitialized(Count); PinY.SetNumUninitialized(Count); PinOrder.SetNumUninitialized(Count);
    for (const TArray<int32>& Island : IslandNodes)
    {
        if (Island.IsEmpty()) { continue; }
        int32 MaxLayer = 0;
        for (const int32 Node : Island) { MaxLayer = FMath::Max(MaxLayer, Layer[Node]); }
        TArray<TArray<int32>> Layers;
        Layers.SetNum(MaxLayer + 1);
        for (const int32 Node : Island) { OrderY[Node] = float(Layers[Layer[Node]].Add(Node)); }
        for (int32 Sweep = 0; Sweep < OrderingSweeps; ++Sweep)
        {
            const bool bForward = Sweep % 2 == 0;
            for (int32 Step = 0; Step <= MaxLayer; ++Step)
            {
                const int32 L = bForward ? Step : MaxLayer - Step;
                for (const int32 Node : Layers[L])
                {
                    PinOrder[Node] = 0; PinY[Node] = 0;
                    const TArray<int32>& Links = bForward ? Incoming[Node] : Outgoing[Node];
                    auto& Neighbors = Scratch.Neighbors; Neighbors.Reset(Links.Num());
                    int32 Dominant = INDEX_NONE;
                    for (const int32 E : Links)
                    {
                        const FProjectedEdge& Edge = Edges[E];
                        const int32 Other = bForward ? Edge.From : Edge.To;
                        if (Rank[Edge.From] >= Rank[Edge.To]) { continue; }
                        Neighbors.Add(OrderY[Other]);
                    }
                    Neighbors.Sort();
                    Scores[Node] = Neighbors.IsEmpty() ? OrderY[Node] : Neighbors[Neighbors.Num() / 2];
                    for (const int32 E : Incoming[Node])
                    {
                        const FProjectedEdge& Edge = Edges[E];
                        if (Edge.bExecution && Rank[Edge.From] < Rank[Node] &&
                            (Dominant == INDEX_NONE || Edge.From < Edges[Dominant].From ||
                                (Edge.From == Edges[Dominant].From && EarlierOutput(Edge, Edges[Dominant]))))
                        {
                            Dominant = E;
                        }
                    }
                    if (Dominant != INDEX_NONE)
                    {
                        Scores[Node] = OrderY[Edges[Dominant].From];
                        PinY[Node] = Edges[Dominant].FromY;
                        PinOrder[Node] = Edges[Dominant].PinOrdinal;
                    }
                }
                Layers[L].Sort([&](int32 A, int32 B)
                {
                    if (Scores[A] != Scores[B]) { return Scores[A] < Scores[B]; }
                    if (PinY[A] != PinY[B]) { return PinY[A] < PinY[B]; }
                    if (PinOrder[A] != PinOrder[B]) { return PinOrder[A] < PinOrder[B]; }
                    return A < B;
                });
                for (int32 I = 0; I < Layers[L].Num(); ++I) { OrderY[Layers[L][I]] = float(I); }
            }
        }
    }
    TArray<int32> DependencyGroupOf;
    DependencyGroupOf.Init(INDEX_NONE, Count);
    const auto ConsumerOrder = ExternalConsumerOrder(Graph, Owner, BoundaryData, Edges, Outgoing, Order, Rank, Pure, Dependencies);
    PlaceExecutionChains(Units, Children, Edges, Layer, Order, Rank, OrderY, Pure, Islands, Settings.VerticalSpacing);
    TMap<int32, TArray<FBoundaryExecution>> IslandBoundaries;
    for (const int32 PinIndex : BoundaryPins)
    {
        const auto& Pin = Graph.Pins[PinIndex]; const int32 Node = Owner[Pin.Node];
        IslandBoundaries.FindOrAdd(Islands.Find(Node)).Add({Layer[Node],
            Units[Children[Node]].Position.Y + Offset[Pin.Node].Y + Pin.Offset.GetValue().Y, Pin.bOutput});
    }
    const TArray<FBoundaryExecution> NoBoundaries;
    TArray<FMeasuredRect> IslandBounds;
    IslandBounds.SetNum(Count);
    float Widest = 0;
    const float Grid = float(Settings.GridSize);
    struct FColumnEvent { int32 Column; float Y; int32 Delta; };
    TArray<FColumnEvent> Events;
    Events.Reserve(Edges.Num() * 2);
    TArray<float> ColumnGaps;
    for (const TArray<int32>& Island : IslandNodes)
    {
        if (Island.IsEmpty()) { continue; }
        int32 MaxLayer = 0;
        for (const int32 Node : Island) { MaxLayer = FMath::Max(MaxLayer, Layer[Node]); }
        TArray<TArray<int32>> Layers;
        Layers.SetNum(MaxLayer + 1);
        for (const int32 Node : Island) { Layers[Layer[Node]].Add(Node); }
        for (auto& Nodes : Layers)
        {
            Nodes.Sort([&](int32 A, int32 B) { return OrderY[A] != OrderY[B] ? OrderY[A] < OrderY[B] : A < B; });
        }
        const auto* Boundaries = IslandBoundaries.Find(Islands.Find(Island[0]));
        PlacePureDependencies(Units, Children, Edges, Outgoing, Layers, Layer, OrderY, Pure, Dependencies, DependencyGroupOf,
            Settings.VerticalSpacing, Boundaries ? *Boundaries : NoBoundaries, ConsumerOrder);
        Events.Reset();
        for (const int32 Node : Island)
        {
            for (const int32 E : Outgoing[Node])
            {
                const auto& Edge = Edges[E];
                const int32 First = Layer[Edge.From], Last = Layer[Edge.To] - 1;
                if (First > Last) { continue; }
                const float FromY = Units[Children[Edge.From]].Position.Y + Edge.FromY;
                const float ToY = Units[Children[Edge.To]].Position.Y + Edge.ToY;
                if (FMath::Abs(FromY - ToY) <= 0.5f) { continue; }
                const float Top = FMath::Min(FromY, ToY), Bottom = FMath::Max(FromY, ToY);
                Events.Add({First, Top, 1}); Events.Add({First, Bottom, -1});
                if (Last != First) { Events.Add({Last, Top, 1}); Events.Add({Last, Bottom, -1}); }
            }
        }
        Events.Sort([](const auto& A, const auto& B)
        {
            if (A.Column != B.Column) { return A.Column < B.Column; }
            if (A.Y != B.Y) { return A.Y < B.Y; }
            return A.Delta > B.Delta;
        });
        ColumnGaps.Init(Settings.HorizontalSpacing, MaxLayer + 1);
        int32 Active = 0, Column = INDEX_NONE;
        for (const auto& Event : Events)
        {
            if (Event.Column != Column) { Column = Event.Column; Active = 0; }
            Active += Event.Delta;
            ColumnGaps[Column] = FMath::Max(ColumnGaps[Column], 2 * WireExitLength + FMath::Max(0, Active - 1) * WireLaneSpacing);
        }
        float X = 0;
        for (int32 L = 0; L <= MaxLayer; ++L)
        {
            float Width = 0;
            for (const int32 Node : Layers[L])
            {
                FUnit& Unit = Units[Children[Node]];
                Unit.Position.X = FMath::RoundToFloat(X - Unit.Bounds.Min.X);
                Width = FMath::Max(Width, Unit.Bounds.Max.X - Unit.Bounds.Min.X);
            }
            for (const int32 Node : Layers[L])
            {
                if (Pure[Node])
                {
                    auto& Unit = Units[Children[Node]];
                    Unit.Position.X = FMath::RoundToFloat(X + Width - Unit.Bounds.Max.X);
                }
            }
            X = (FMath::FloorToFloat((X + Width + ColumnGaps[L]) / Grid) + 1.f) * Grid;
        }
        FMeasuredRect Bounds;
        bool bFirst = true;
        int32 Returns = 0;
        for (const int32 Node : Island)
        {
            const auto& Unit = Units[Children[Node]];
            const auto Rect = Translate(Unit.Bounds, Unit.Position);
            Bounds = bFirst ? Rect : Union(Bounds, Rect); bFirst = false;
            Returns += ReturnCounts[Node];
        }
        if (Returns)
        {
            const float Margin = WireExitLength + float(Returns) * WireLaneSpacing;
            Bounds.Min -= FVector2f(Margin); Bounds.Max += FVector2f(Margin);
        }
        IslandBounds[Island[0]] = Bounds;
        Widest = FMath::Max(Widest, (FMath::CeilToFloat(Bounds.Max.X / Grid) - FMath::FloorToFloat(Bounds.Min.X / Grid)) * Grid);
    }
    float X = 0, Y = 0, RowHeight = 0;
    TOptional<FMeasuredRect> PackedBounds;
    for (const auto& Island : IslandNodes)
    {
        if (Island.IsEmpty()) { continue; }
        const auto& Bounds = IslandBounds[Island[0]];
        const float Left = FMath::FloorToFloat(Bounds.Min.X / Grid) * Grid;
        const float Width = FMath::CeilToFloat(Bounds.Max.X / Grid) * Grid - Left;
        const float Top = FMath::FloorToFloat(Bounds.Min.Y);
        const float Height = FMath::CeilToFloat(Bounds.Max.Y) - Top;
        if (X > 0 && X + Width > Widest)
        {
            X = 0; Y += RowHeight + FMath::CeilToFloat(Settings.VerticalSpacing * 2); RowHeight = 0;
        }
        const FVector2f Delta(X - Left, Y - Top);
        for (const int32 Node : Island) { Units[Children[Node]].Position += Delta; }
        const auto Placed = Translate(Bounds, Delta);
        PackedBounds = PackedBounds.IsSet() ? Union(PackedBounds.GetValue(), Placed) : Placed;
        RowHeight = FMath::Max(RowHeight, Height);
        X = FMath::CeilToFloat((X + Width + Settings.HorizontalSpacing) / Grid) * Grid;
    }
    const uint64 Crossings = CountOrderingCrossings(Units, Children, Edges, Layer);
    OutCrossings = Crossings == MAX_uint64 || OutCrossings == MAX_uint64 ? MAX_uint64 : OutCrossings + Crossings;
    return PackedBounds;
}
}

static bool ComputeLayoutCandidate(const FLayoutGraph& Graph, const FLayoutSettings& Settings, int32 OrderingSweeps,
    FLayoutResult& OutResult, FString& OutReason, ELayoutFailure* OutFailure, FLayoutScratch& Scratch)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_Layout);
    OutResult = FLayoutResult(); OutReason.Reset();
    if (OutFailure) { *OutFailure = ELayoutFailure::InvalidInput; }
    if (!FMath::IsFinite(Settings.HorizontalSpacing) || !FMath::IsFinite(Settings.VerticalSpacing) ||
        !FMath::IsFinite(Settings.CommentPadding) || Settings.HorizontalSpacing < 16 ||
        Settings.VerticalSpacing < 16 || Settings.CommentPadding < 8 || Settings.GridSize < 1 || Settings.GridSize > 100)
    {
        OutReason = TEXT("Layout spacing or graph grid is invalid."); return false;
    }
    const int32 Count = Graph.Nodes.Num();
    FLayoutResult Result;
    Result.OrderingSweeps = OrderingSweeps;
    Result.Positions.SetNumUninitialized(Count); Result.Sizes.SetNumUninitialized(Count);
    Result.FeedbackEdges.Init(false, Graph.Edges.Num());
    TArray<FMeasuredRect> Bodies;
    Bodies.Reserve(Count);
    TArray<int32> Comments;
    TBitArray<> EmptyComments(false, Count);
    for (int32 I = 0; I < Count; ++I)
    {
        const FLayoutNode& Node = Graph.Nodes[I];
        const FMeasuredNode& Geometry = Node.Geometry;
        if (!FMath::IsFinite(Geometry.BodySize.X) || !FMath::IsFinite(Geometry.BodySize.Y) ||
            Geometry.BodySize.X <= 0 || Geometry.BodySize.Y <= 0 ||
            !FMath::IsFinite(Geometry.VisualBounds.Min.X) || !FMath::IsFinite(Geometry.VisualBounds.Min.Y) ||
            !FMath::IsFinite(Geometry.VisualBounds.Max.X) || !FMath::IsFinite(Geometry.VisualBounds.Max.Y) ||
            Geometry.VisualBounds.Max.X < Geometry.VisualBounds.Min.X || Geometry.VisualBounds.Max.Y < Geometry.VisualBounds.Min.Y)
        {
            OutReason = TEXT("A node has invalid layout geometry."); return false;
        }
        const FVector2f Position(float(Geometry.Position.X), float(Geometry.Position.Y));
        Bodies.Add({Position, Position + Geometry.BodySize});
        Result.Positions[I] = Geometry.Position; Result.Sizes[I] = Node.OriginalSize;
        if (Node.bComment) { Comments.Add(I); }
    }
    if (Graph.Anchor == INDEX_NONE)
    {
        if (OutFailure) { *OutFailure = ELayoutFailure::None; }
        OutResult = MoveTemp(Result); return true;
    }
    if (!Graph.Nodes.IsValidIndex(Graph.Anchor) || Graph.Nodes[Graph.Anchor].bComment)
    {
        OutReason = TEXT("The layout anchor is invalid."); return false;
    }
    for (const FLayoutEdge& Edge : Graph.Edges)
    {
        if (!Graph.Pins.IsValidIndex(Edge.From) || !Graph.Pins.IsValidIndex(Edge.To) ||
            !Graph.Nodes.IsValidIndex(Graph.Pins[Edge.From].Node) || !Graph.Nodes.IsValidIndex(Graph.Pins[Edge.To].Node) ||
            !Graph.Pins[Edge.From].Offset.IsSet() || !Graph.Pins[Edge.To].Offset.IsSet() ||
            !FMath::IsFinite(Graph.Pins[Edge.From].Offset.GetValue().X) || !FMath::IsFinite(Graph.Pins[Edge.From].Offset.GetValue().Y) ||
            !FMath::IsFinite(Graph.Pins[Edge.To].Offset.GetValue().X) || !FMath::IsFinite(Graph.Pins[Edge.To].Offset.GetValue().Y))
        {
            OutReason = TEXT("The layout contains an invalid edge."); return false;
        }
    }
    FDisjointSets Regions(Count);
    TBitArray<> Ambiguous(false, Count);
    for (const int32 A : Comments)
    {
        bool bHasMember = false;
        for (int32 B = 0; B < Count; ++B)
        {
            if (A != B && Contains(Bodies[A], Bodies[B])) { bHasMember = true; }
        }
        EmptyComments[A] = !bHasMember;
        for (const int32 B : Comments)
        {
            if (B <= A || !Intersects(Bodies[A], Bodies[B])) { continue; }
            Regions.Join(A, B);
            const bool bAB = Contains(Bodies[A], Bodies[B]), bBA = Contains(Bodies[B], Bodies[A]);
            if ((!bAB && !bBA) || (bAB && bBA)) { Ambiguous[A] = true; Ambiguous[B] = true; }
        }
    }
    TBitArray<> RigidRoots(false, Count);
    for (const int32 C : Comments) { if (Ambiguous[C]) { RigidRoots[Regions.Find(C)] = true; } }
    TBitArray<> RigidNodes(false, Count);
    for (const int32 C : Comments)
    {
        if (!RigidRoots[Regions.Find(C)]) { continue; }
        RigidNodes[C] = true;
        for (int32 I = 0; I < Count; ++I)
        {
            if (Intersects(Bodies[C], Bodies[I])) { RigidNodes[I] = true; }
        }
    }
    for (const int32 C : Comments)
    {
        if (!RigidNodes[C]) { continue; }
        for (int32 I = 0; I < Count; ++I)
        {
            if (RigidNodes[I] && Intersects(Bodies[C], Bodies[I])) { Regions.Join(C, I); }
        }
    }

    TArray<FUnit> Units;
    Units.Reserve(Count);
    TArray<int32> UnitOf;
    UnitOf.Init(INDEX_NONE, Count);
    TMap<int32, int32> RigidUnits;
    for (int32 I = 0; I < Count; ++I)
    {
        if (RigidNodes[I])
        {
            const int32 Root = Regions.Find(I);
            int32* Existing = RigidUnits.Find(Root);
            int32 Index;
            if (Existing) { Index = *Existing; }
            else
            {
                Index = Units.AddDefaulted(); RigidUnits.Add(Root, Index);
                Units[Index].Key = I; Units[Index].bRigid = true;
                Units[Index].Bounds = Translate(Graph.Nodes[I].Geometry.VisualBounds, Bodies[I].Min);
            }
            UnitOf[I] = Index;
            FUnit& Unit = Units[Index];
            Unit.Members.Add({I, Bodies[I].Min});
            Unit.Bounds = Union(Unit.Bounds, Translate(Graph.Nodes[I].Geometry.VisualBounds, Bodies[I].Min));
            Unit.bFixed |= EmptyComments[I];
            Result.bLimitedComments = true;
        }
        else
        {
            const int32 Index = Units.AddDefaulted(); UnitOf[I] = Index;
            FUnit& Unit = Units[Index];
            Unit.Key = I; Unit.Bounds = Graph.Nodes[I].Geometry.VisualBounds;
            Unit.Size = Graph.Nodes[I].Geometry.BodySize;
            Unit.bFixed = EmptyComments[I];
            if (Graph.Nodes[I].bComment && !EmptyComments[I]) { Unit.Comment = I; }
            else { Unit.Members.Add({I, FVector2f::ZeroVector}); }
        }
    }
    for (FUnit& Unit : Units)
    {
        if (Unit.bRigid)
        {
            const FVector2f Origin = Unit.Bounds.Min;
            for (FMember& Member : Unit.Members) { Member.Offset -= Origin; }
            Unit.Bounds = Translate(Unit.Bounds, -Origin); Unit.Size = Unit.Bounds.Max;
            if (Unit.bFixed) { Unit.Position = Origin; }
        }
        else if (Unit.bFixed) { Unit.Position = Bodies[Unit.Key].Min; }
    }
    for (int32 I = 0; I < Units.Num(); ++I)
    {
        FUnit& Unit = Units[I];
        if (Unit.bRigid || Unit.bFixed) { continue; }
        double BestArea = TNumericLimits<double>::Max();
        for (const int32 C : Comments)
        {
            if (RigidNodes[C] || EmptyComments[C] || C == Unit.Key || !Contains(Bodies[C], Bodies[Unit.Key])) { continue; }
            const FVector2f Size = Bodies[C].Max - Bodies[C].Min;
            const double Area = double(Size.X) * Size.Y;
            if (Area < BestArea) { BestArea = Area; Unit.Parent = UnitOf[C]; }
        }
    }
    TArray<int32> Roots, Remaining;
    Remaining.Init(0, Units.Num());
    for (int32 I = 0; I < Units.Num(); ++I)
    {
        const int32 Parent = Units[I].Parent;
        if (Parent == INDEX_NONE) { Roots.Add(I); }
        else { Units[Parent].Children.Add(I); ++Remaining[Parent]; }
    }
    TArray<int32> Ready;
    for (int32 I = 0; I < Units.Num(); ++I) { if (!Remaining[I]) { Ready.Add(I); } }
    for (int32 Head = 0; Head < Ready.Num(); ++Head)
    {
        FUnit& Unit = Units[Ready[Head]];
        if (Unit.Comment != INDEX_NONE)
        {
            const auto ContentBounds = PlaceChildren(Graph, Settings, Units, Unit.Children, OrderingSweeps, Result.OrderingCrossings, Scratch);
            if (!ContentBounds.IsSet())
            {
                OutReason = TEXT("A comment contains only fixed regions; its grouping cannot yet be safely reformatted."); return false;
            }
            const auto& Bounds = ContentBounds.GetValue();
            const auto& Header = Graph.Nodes[Unit.Comment].Geometry.CommentHeader;
            const float HeaderBottom = Header.IsSet() ? FMath::Max(0.f, Header.GetValue().Max.Y) : 32;
            const FVector2f RequiredShift = FVector2f(Settings.CommentPadding, HeaderBottom + Settings.CommentPadding) - Bounds.Min;
            const FVector2f Shift(FMath::CeilToFloat(RequiredShift.X), FMath::CeilToFloat(RequiredShift.Y));
            for (const int32 Child : Unit.Children) { Units[Child].Position += Shift; }
            Unit.Size = FVector2f(FMath::CeilToFloat(FMath::Max(160.0f, Bounds.Max.X + Shift.X + Settings.CommentPadding)),
                FMath::CeilToFloat(Bounds.Max.Y + Shift.Y + Settings.CommentPadding));
            Unit.Bounds = {FVector2f::ZeroVector, Unit.Size};
        }
        if (Unit.Parent != INDEX_NONE && --Remaining[Unit.Parent] == 0) { Ready.Add(Unit.Parent); }
    }
    if (Ready.Num() != Units.Num()) { OutReason = TEXT("Comment membership contains a cycle."); return false; }
    PlaceChildren(Graph, Settings, Units, Roots, OrderingSweeps, Result.OrderingCrossings, Scratch);
    if (Result.OrderingCrossings == MAX_uint64)
    {
        if (OutFailure) { *OutFailure = ELayoutFailure::Constraints; }
        OutReason = TEXT("A proposed connection has invalid pin coordinates."); return false;
    }
    TArray<FVector2f> Proposed;
    Proposed.SetNumZeroed(Count);
    TBitArray<> Fixed(false, Count);
    for (const int32 Root : Roots)
    {
        Collect(Units, Root, Scratch);
        for (const FMember& Member : Scratch.Members)
        {
            Proposed[Member.Node] = Member.Offset + Units[Root].Position;
            Fixed[Member.Node] = Units[Root].bFixed;
        }
    }
    const FVector2f AnchorOriginal(float(Graph.Nodes[Graph.Anchor].Geometry.Position.X), float(Graph.Nodes[Graph.Anchor].Geometry.Position.Y));
    const FVector2f Shift = AnchorOriginal - Proposed[Graph.Anchor];
    if (Fixed[Graph.Anchor] && !Shift.IsNearlyZero()) { OutReason = TEXT("The anchor belongs to a fixed comment region."); return false; }
    for (int32 I = 0; I < Count; ++I)
    {
        const FVector2f Position = Proposed[I] + (Fixed[I] ? FVector2f::ZeroVector : Shift);
        if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) ||
            FMath::Abs(Position.X) > 16777216 || FMath::Abs(Position.Y) > 16777216)
        {
            if (OutFailure) { *OutFailure = ELayoutFailure::Constraints; }
            OutReason = TEXT("The proposed layout exceeds reliable graph coordinates."); return false;
        }
        Result.Positions[I] = FIntPoint(FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y));
    }
    for (const FUnit& Unit : Units)
    {
        if (Unit.Comment != INDEX_NONE) { Result.Sizes[Unit.Comment] = FIntPoint(FMath::CeilToInt(Unit.Size.X), FMath::CeilToInt(Unit.Size.Y)); }
    }
    TArray<FMeasuredRect> NewBodies;
    NewBodies.Reserve(Count);
    for (int32 I = 0; I < Count; ++I)
    {
        const FVector2f Position(float(Result.Positions[I].X), float(Result.Positions[I].Y));
        const FVector2f Size = Graph.Nodes[I].bComment ? FVector2f(float(Result.Sizes[I].X), float(Result.Sizes[I].Y)) : Graph.Nodes[I].Geometry.BodySize;
        NewBodies.Add({Position, Position + Size});
    }
    if (HasBodyOverlap(Graph, UnitOf, NewBodies))
    {
        if (OutFailure) { *OutFailure = ELayoutFailure::Constraints; }
        OutReason = TEXT("The proposed layout contains overlapping nodes."); return false;
    }
    for (const int32 C : Comments)
    {
        for (int32 I = 0; I < Count; ++I)
        {
            if (I != C && Contains(Bodies[C], Bodies[I]) != Contains(NewBodies[C], NewBodies[I]))
            {
                if (OutFailure) { *OutFailure = ELayoutFailure::Constraints; }
                OutReason = TEXT("The proposed layout would change comment membership; no changes were applied."); return false;
            }
            if (I != C && EmptyComments[C] && !Intersects(Bodies[C], Bodies[I]) && Intersects(NewBodies[C], NewBodies[I]))
            {
                if (OutFailure) { *OutFailure = ELayoutFailure::Constraints; }
                OutReason = TEXT("The proposed layout intersects a fixed empty comment; no changes were applied."); return false;
            }
        }
    }
    for (int32 I = 0; I < Graph.Edges.Num(); ++I)
    {
        const FLayoutEdge& Edge = Graph.Edges[I];
        const FLayoutPin& From = Graph.Pins[Edge.From];
        const FLayoutPin& To = Graph.Pins[Edge.To];
        Result.FeedbackEdges[I] = Result.Positions[From.Node].X + From.Offset.GetValue().X >=
            Result.Positions[To.Node].X + To.Offset.GetValue().X;
    }
    OutResult = MoveTemp(Result);
    if (OutFailure) { *OutFailure = ELayoutFailure::None; }
    return true;
}

struct FLayoutJob::FState
{
    FLayoutGraph Graph;
    FLayoutSettings Settings;
    FLayoutScratch Scratch;
    FLayoutResult Best;
    TOptional<uint64> LastCrossings;
    FString Reason;
    ELayoutFailure Failure = ELayoutFailure::InvalidInput;
    int32 NextSweep = 1;
    bool bHaveCandidate = false, bDone = false, bTaken = false;
    FState(FLayoutGraph InGraph, FLayoutSettings InSettings) : Graph(MoveTemp(InGraph)), Settings(InSettings) {}
};

FLayoutJob::FLayoutJob(FLayoutGraph Graph, FLayoutSettings Settings)
    : State(MakeUnique<FState>(MoveTemp(Graph), Settings)) {}
FLayoutJob::~FLayoutJob() = default;

bool FLayoutJob::Advance(double Deadline)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_ChooseLayout);
    if (State->bDone) { return true; }
    do
    {
        FLayoutResult Candidate;
        if (!ComputeLayoutCandidate(State->Graph, State->Settings, State->NextSweep, Candidate, State->Reason, &State->Failure, State->Scratch))
        {
            if (State->Failure == ELayoutFailure::InvalidInput) { State->bDone = true; return true; }
        }
        else
        {
            if (State->NextSweep == 4) { State->LastCrossings = Candidate.OrderingCrossings; }
            if (!State->bHaveCandidate || Candidate.OrderingCrossings <= State->Best.OrderingCrossings)
            {
                State->Best = MoveTemp(Candidate); State->bHaveCandidate = true;
            }
        }
        State->bDone = ++State->NextSweep > 4;
    }
    while (!State->bDone && FPlatformTime::Seconds() < Deadline);
    return State->bDone;
}

bool FLayoutJob::TakeResult(FLayoutResult& OutResult, FString& OutReason, ELayoutFailure* OutFailure)
{
    OutResult = {}; OutReason.Reset();
    if (OutFailure) { *OutFailure = State->Failure; }
    if (!State->bDone || State->bTaken)
    {
        OutReason = TEXT("Layout result is not available."); return false;
    }
    if (!State->bHaveCandidate || State->Failure == ELayoutFailure::InvalidInput)
    {
        OutReason = State->Reason; return false;
    }
    State->bTaken = true;
    State->Best.LastCandidateCrossings = State->LastCrossings;
    OutResult = MoveTemp(State->Best);
    if (OutFailure) { *OutFailure = ELayoutFailure::None; }
    return true;
}

bool ComputeLayout(const FLayoutGraph& Graph, const FLayoutSettings& Settings,
    FLayoutResult& OutResult, FString& OutReason, ELayoutFailure* OutFailure)
{
    FLayoutJob Job(Graph, Settings);
    Job.Advance(TNumericLimits<double>::Max());
    return Job.TakeResult(OutResult, OutReason, OutFailure);
}
}
