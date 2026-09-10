// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace GlooPrint::Routing
{
struct FObstacle
{
    FBox2f Box;
    int32 Node;
};

class FObstacles
{
    static constexpr float CellSize = 256;
    static constexpr int32 MaxQueryCells = 256;
public:
    TArray<FObstacle> Items;
    FBox2f Bounds = FBox2f(ForceInit);

    void Reserve(int32 Count)
    {
        Items.Reserve(Count); Visited.Reserve(Count);
    }

    int32 Add(FBox2f Box, int32 Node)
    {
        const int32 Index = Items.Add({Box, Node});
        Visited.Add(0);
        Bounds += Box;
        const FIntPoint Min = Cell(Box.Min), Max = Cell(Box.Max);
        const auto IndexAxis = [Index](TMap<int32, TArray<int32>>& Axis, TArray<int32>& Wide, int32 First, int32 Last)
        {
            if (int64(Last) - First + 1 > MaxQueryCells) { Wide.Add(Index); return; }
            for (int32 I = First; I <= Last; ++I) { Axis.FindOrAdd(I).Add(Index); }
        };
        IndexAxis(Columns, WideColumns, Min.X, Max.X);
        IndexAxis(Rows, WideRows, Min.Y, Max.Y);
        if (CountCells(Min, Max) > MaxQueryCells) { Large.Add(Index); return Index; }
        for (int32 X = Min.X; X <= Max.X; ++X)
        {
            for (int32 Y = Min.Y; Y <= Max.Y; ++Y) { Cells.FindOrAdd({X, Y}).Add(Index); }
        }
        return Index;
    }

    void GrowHorizontal(int32 Index, const FBox2f& Box)
    {
        const FBox2f Old = Items[Index].Box;
        check(Box.Min.Y == Old.Min.Y && Box.Max.Y == Old.Max.Y &&
            Box.Min.X <= Old.Min.X && Box.Max.X >= Old.Max.X);
        Items[Index].Box = Box; Bounds += Box;
        const FIntPoint Min = Cell(Box.Min), Max = Cell(Box.Max);
        const FIntPoint OldMin = Cell(Old.Min), OldMax = Cell(Old.Max);
        const auto NewColumns = [&](auto AddColumn)
        {
            for (int32 X = Min.X; X < OldMin.X; ++X) { AddColumn(X); }
            for (int32 X = OldMax.X + 1; X <= Max.X; ++X) { AddColumn(X); }
        };
        if (int64(OldMax.X) - OldMin.X + 1 <= MaxQueryCells)
        {
            if (int64(Max.X) - Min.X + 1 > MaxQueryCells) { WideColumns.Add(Index); }
            else { NewColumns([&](int32 X) { Columns.FindOrAdd(X).Add(Index); }); }
        }
        if (CountCells(OldMin, OldMax) <= MaxQueryCells)
        {
            if (CountCells(Min, Max) > MaxQueryCells) { Large.Add(Index); }
            else
            {
                NewColumns([&](int32 X)
                {
                    for (int32 Y = Min.Y; Y <= Max.Y; ++Y) { Cells.FindOrAdd({X, Y}).Add(Index); }
                });
            }
        }
    }

    void Finish() { Visited.Init(0, Items.Num()); }

    bool ClearBox(const FBox2f& Box, int32 IgnoreNode = INDEX_NONE, int32 OtherIgnoreNode = INDEX_NONE) const
    {
        return Query(Box, [this, &Box, IgnoreNode, OtherIgnoreNode](int32 Index)
        {
            const auto& Obstacle = Items[Index];
            return Obstacle.Node == IgnoreNode || Obstacle.Node == OtherIgnoreNode || !Overlaps(Box, Obstacle.Box);
        });
    }

    bool ClearLine(FVector2f A, FVector2f B, int32 IgnoreNode = INDEX_NONE) const
    {
        if (A.X != B.X && A.Y != B.Y) { return false; }
        FBox2f Box(A, A); Box += B;
        return Query(Box, [this, A, B, IgnoreNode](int32 Index)
        {
            const auto& O = Items[Index];
            if (O.Node == IgnoreNode) { return true; }
            if (A.Y == B.Y)
            {
                return A.Y <= O.Box.Min.Y || A.Y >= O.Box.Max.Y ||
                    FMath::Max(A.X, B.X) <= O.Box.Min.X || FMath::Min(A.X, B.X) >= O.Box.Max.X;
            }
            if (A.X == B.X)
            {
                return A.X <= O.Box.Min.X || A.X >= O.Box.Max.X ||
                    FMath::Max(A.Y, B.Y) <= O.Box.Min.Y || FMath::Min(A.Y, B.Y) >= O.Box.Max.Y;
            }
            return false;
        });
    }

private:
    static FIntPoint Cell(FVector2f P) { return {FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize)}; }
    static int64 CountCells(FIntPoint Min, FIntPoint Max) { return (int64(Max.X) - Min.X + 1) * (int64(Max.Y) - Min.Y + 1); }
    static bool Overlaps(const FBox2f& A, const FBox2f& B)
    {
        return A.Min.X < B.Max.X && A.Max.X > B.Min.X && A.Min.Y < B.Max.Y && A.Max.Y > B.Min.Y;
    }
public:
    template<typename Predicate> bool Query(const FBox2f& Box, Predicate Accept) const
    {
        const FIntPoint Min = Cell(Box.Min), Max = Cell(Box.Max);
        const bool bColumns = int64(Max.X) - Min.X <= int64(Max.Y) - Min.Y;
        const int32 First = bColumns ? Min.X : Min.Y, Last = bColumns ? Max.X : Max.Y;
        const auto& Axis = bColumns ? Columns : Rows;
        const auto& Wide = bColumns ? WideColumns : WideRows;
        bool bSmallerThanSpill = false;
        if (First == Last && !Large.IsEmpty())
        {
            const auto* Indices = Axis.Find(First);
            bSmallerThanSpill = (Indices ? Indices->Num() : 0) + Wide.Num() < Large.Num();
        }
        if (CountCells(Min, Max) > MaxQueryCells || bSmallerThanSpill)
        {
            if (int64(Last) - First + 1 <= MaxQueryCells)
            {
                if (++Stamp == 0) { Visited.Init(0, Items.Num()); ++Stamp; }
                for (int32 Index : Wide) { Visited[Index] = Stamp; if (!Accept(Index)) { return false; } }
                for (int32 I = First; I <= Last; ++I)
                {
                    if (const auto* Indices = Axis.Find(I))
                    {
                        for (int32 Index : *Indices)
                        {
                            if (Visited[Index] == Stamp) { continue; }
                            Visited[Index] = Stamp;
                            if (!Accept(Index)) { return false; }
                        }
                    }
                }
                return true;
            }
            for (int32 I = 0; I < Items.Num(); ++I) { if (!Accept(I)) { return false; } }
            return true;
        }
        if (++Stamp == 0) { Visited.Init(0, Items.Num()); ++Stamp; }
        for (int32 Index : Large) { Visited[Index] = Stamp; if (!Accept(Index)) { return false; } }
        for (int32 X = Min.X; X <= Max.X; ++X)
        {
            for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
            {
                if (const auto* Indices = Cells.Find({X, Y}))
                {
                    for (int32 Index : *Indices)
                    {
                        if (Visited[Index] == Stamp) { continue; }
                        Visited[Index] = Stamp;
                        if (!Accept(Index)) { return false; }
                    }
                }
            }
        }
        return true;
    }
private:
    TMap<FIntPoint, TArray<int32>> Cells;
    TMap<int32, TArray<int32>> Columns, Rows;
    TArray<int32> WideColumns, WideRows;
    TArray<int32> Large;
    mutable TArray<uint32> Visited;
    mutable uint32 Stamp = 0;
};

}
