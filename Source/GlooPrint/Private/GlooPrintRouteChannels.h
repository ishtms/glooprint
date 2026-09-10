// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Algo/BinarySearch.h"

namespace GlooPrint
{
inline constexpr int32 RouteChannelLimit = 128;

struct FChannelCoordinates
{
    TArray<float> Values;

    void SortUnique()
    {
        Values.Sort();
        int32 Count = 0;
        for (int32 I = 0; I < Values.Num(); ++I)
        {
            if (Count == 0 || Values[I] != Values[Count - 1]) { Values[Count++] = Values[I]; }
        }
        Values.SetNum(Count, EAllowShrinking::No);
    }

    void Add(float Value)
    {
        const int32 Index = Algo::LowerBound(Values, Value);
        if (!Values.IsValidIndex(Index) || Values[Index] != Value) { Values.Insert(Value, Index); }
    }

    void AppendNearest(TArray<float>& Out, float Center, float Shift = 0) const
    {
        int32 Right = Algo::LowerBoundBy(Values, Center, [Shift](float Value) { return Value + Shift; });
        int32 Left = Right - 1, LeftNext = 0, LeftEnd = -1, Count = 0;
        TOptional<float> Last;
        while (Count < RouteChannelLimit && (Left >= 0 || LeftNext <= LeftEnd || Right < Values.Num()))
        {
            if (LeftNext > LeftEnd && Left >= 0)
            {
                LeftEnd = Left; LeftNext = Left;
                const float Distance = FMath::Abs(Values[Left] + Shift - Center);
                if (Left > 0 && FMath::Abs(Values[Left - 1] + Shift - Center) == Distance)
                {
                    int32 Low = 0, High = Left;
                    while (Low < High)
                    {
                        const int32 Middle = Low + (High - Low) / 2;
                        if (FMath::Abs(Values[Middle] + Shift - Center) > Distance) { Low = Middle + 1; }
                        else { High = Middle; }
                    }
                    LeftNext = Low;
                }
                Left = LeftNext - 1;
            }
            const bool bLeft = LeftNext <= LeftEnd && (Right == Values.Num() ||
                FMath::Abs(Values[LeftNext] + Shift - Center) <= FMath::Abs(Values[Right] + Shift - Center));
            const float Value = Values[bLeft ? LeftNext++ : Right++] + Shift;
            if (Last.IsSet() && Last.GetValue() == Value) { continue; }
            Out.Add(Value); Last = Value; ++Count;
        }
    }
};

}
