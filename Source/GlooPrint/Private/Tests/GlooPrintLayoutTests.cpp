// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "GlooPrintLayout.h"
#include "GlooPrintRouting.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include <limits>

namespace GlooPrint::Tests
{
namespace
{
int32 AddNode(FLayoutGraph& Graph, FIntPoint Position, FVector2f Size, float InputY = 40, float OutputY = 40)
{
    const int32 Index = Graph.Nodes.AddDefaulted();
    FLayoutNode& Node = Graph.Nodes[Index];
    Node.Geometry.Id = FGuid(0, 0, 0, Index + 1);
    Node.Geometry.Position = Position;
    Node.Geometry.BodySize = Size;
    Node.Geometry.VisualBounds = {FVector2f::ZeroVector, Size};
    Node.FirstPin = Graph.Pins.Num(); Node.PinCount = 2;
    Graph.Pins.Add({FGuid(0, 0, Index + 1, 1), Index, 0, false, ELinkKind::Execution, FVector2f(0, InputY)});
    Graph.Pins.Add({FGuid(0, 0, Index + 1, 2), Index, 1, true, ELinkKind::Execution, FVector2f(Size.X, OutputY)});
    return Index;
}

void Link(FLayoutGraph& Graph, int32 A, int32 B, ELinkKind Kind = ELinkKind::Execution, int32 PinOrdinal = 1)
{
    int32 From = Graph.Nodes[A].FirstPin + 1;
    if (PinOrdinal != 1)
    {
        FLayoutPin Pin = Graph.Pins[From];
        Pin.Ordinal = PinOrdinal; Pin.Offset.GetValue().Y += 25 * (PinOrdinal - 1);
        From = Graph.Pins.Add(Pin);
    }
    const int32 Edge = Graph.Edges.Add({From, Graph.Nodes[B].FirstPin, Kind});
    Graph.Nodes[A].Outgoing.Add(Edge); Graph.Nodes[B].Incoming.Add(Edge);
}

void CheckNoOverlap(FAutomationTestBase& Test, const FLayoutGraph& Graph, const FLayoutResult& Result)
{
    for (int32 A = 0; A < Graph.Nodes.Num(); ++A)
    {
        if (Graph.Nodes[A].bComment) { continue; }
        for (int32 B = 0; B < A; ++B)
        {
            if (Graph.Nodes[B].bComment) { continue; }
            const auto& PA = Result.Positions[A]; const auto& PB = Result.Positions[B];
            const auto SA = Graph.Nodes[A].Geometry.BodySize, SB = Graph.Nodes[B].Geometry.BodySize;
            Test.TestFalse(TEXT("Movable bodies do not overlap"), PA.X < PB.X + SB.X && PA.X + SA.X > PB.X && PA.Y < PB.Y + SB.Y && PA.Y + SA.Y > PB.Y);
        }
    }
}

bool CheckColdIdempotence(FAutomationTestBase& Test, FLayoutGraph Graph, const FLayoutResult& First, const FLayoutSettings& Settings = {})
{
    for (int32 I = 0; I < Graph.Nodes.Num(); ++I)
    {
        Graph.Nodes[I].Geometry.Position = First.Positions[I];
        if (Graph.Nodes[I].bComment)
        {
            Graph.Nodes[I].OriginalSize = First.Sizes[I];
            Graph.Nodes[I].Geometry.BodySize = FVector2f(float(First.Sizes[I].X), float(First.Sizes[I].Y));
            Graph.Nodes[I].Geometry.VisualBounds.Max = Graph.Nodes[I].Geometry.BodySize;
        }
    }
    FLayoutResult Second;
    FString Reason;
    if (!Test.TestTrue(TEXT("Second computation succeeds with fresh state"), ComputeLayout(Graph, Settings, Second, Reason)))
    {
        Test.AddError(Reason); return false;
    }
    Test.TestTrue(TEXT("Cold second layout has identical positions"), First.Positions == Second.Positions);
    Test.TestTrue(TEXT("Cold second layout has identical comment bounds"), First.Sizes == Second.Sizes);
    Test.TestTrue(TEXT("Cold second layout has identical feedback classification"), First.FeedbackEdges == Second.FeedbackEdges);
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutFlowTest, "GlooPrint.Layout.FlowCyclesAndIslands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLayoutFlowTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    AddNode(Graph, {250, 700}, {180, 140}, 35, 60);
    AddNode(Graph, {-500, -200}, {240, 100}, 30, 40);
    AddNode(Graph, {-300, -300}, {120, 120}, 40, 70);
    AddNode(Graph, {-900, -900}, {160, 100}, 40, 40);
    AddNode(Graph, {250, 700}, {100, 80});
    Link(Graph, 0, 1);
    Link(Graph, 0, 2, ELinkKind::Execution, 2);
    Link(Graph, 1, 3); Link(Graph, 2, 3);
    Graph.Anchor = 1;
    FLayoutResult Result;
    FString Reason;
    if (!TestTrue(TEXT("Diamond and disconnected island lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Selected anchor stationary"), Result.Positions[1], Graph.Nodes[1].Geometry.Position);
    TestTrue(TEXT("Branch order follows visible output order"), Result.Positions[1].Y < Result.Positions[2].Y);
    TestTrue(TEXT("Join after both incoming branches"), Result.Positions[3].X > Result.Positions[1].X && Result.Positions[3].X > Result.Positions[2].X);
    TestEqual(TEXT("Main execution attachments aligned"), Result.Positions[0].Y + 60, Result.Positions[1].Y + 30);
    CheckNoOverlap(*this, Graph, Result);
    CheckColdIdempotence(*this, Graph, Result);
    Link(Graph, 3, 0);
    if (TestTrue(TEXT("Execution cycle terminates"), ComputeLayout(Graph, {}, Result, Reason)))
    {
        TestTrue(TEXT("Cycle receives a feedback edge"), Result.FeedbackEdges.CountSetBits() > 0);
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    }
    else { AddError(Reason); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBodyOverlapLayoutTest, "GlooPrint.Layout.BodyOverlapValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBodyOverlapLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 3; ++I)
    {
        AddNode(Graph, {I * 400, I * 400}, {180, 400});
        Graph.Nodes[I].Geometry.VisualBounds.Max.Y = 1;
    }
    Link(Graph, 0, 1); Link(Graph, 0, 2, ELinkKind::Execution, 2); Graph.Anchor = 0;
    FLayoutResult Result; FString Reason; ELayoutFailure Failure;
    TestFalse(TEXT("Overlapping proposed bodies are rejected"), ComputeLayout(Graph, {}, Result, Reason, &Failure));
    TestEqual(TEXT("Overlap is a constraint failure"), Failure, ELayoutFailure::Constraints);
    TestTrue(TEXT("The final body guard identifies the overlap"), Reason.Contains(TEXT("overlapping nodes")));
    TestTrue(TEXT("Overlap returns no partial placement"), Result.Positions.IsEmpty());
    for (const int32 Shift : {-10000000, 10000000})
    {
        FLayoutGraph Tiny;
        for (int32 I = 0; I < 3; ++I) { AddNode(Tiny, {Shift, Shift}, {0.25f, 0.25f}, 0.125f, 0.125f); }
        Link(Tiny, 0, 1); Link(Tiny, 0, 2, ELinkKind::Execution, 2); Tiny.Anchor = 0;
        if (TestTrue(TEXT("Rounded zero-extent bodies remain valid near coordinate limits"), ComputeLayout(Tiny, {}, Result, Reason)))
        {
            const float X = float(Result.Positions[0].X);
            TestEqual(TEXT("Fixture exercises collapsed float body endpoints"), X + Tiny.Nodes[0].Geometry.BodySize.X, X);
            TestEqual(TEXT("Tiny-body anchor stays exact"), Result.Positions[0], Tiny.Nodes[0].Geometry.Position);
            CheckColdIdempotence(*this, Tiny, Result);
        }
        else { AddError(Reason); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInvalidGeometryLayoutTest, "GlooPrint.Layout.InvalidGeometryRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInvalidGeometryLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Valid;
    AddNode(Valid, {0, 0}, {160, 100}); AddNode(Valid, {500, 200}, {160, 100});
    Link(Valid, 0, 1); Valid.Anchor = 0;
    FLayoutResult Good; FString Reason;
    if (!TestTrue(TEXT("Reference graph has a valid layout"), ComputeLayout(Valid, {}, Good, Reason))) { AddError(Reason); return false; }
    const auto Reject = [&](const TCHAR* Case, const FLayoutGraph& Graph, ELayoutFailure Expected = ELayoutFailure::InvalidInput)
    {
        FLayoutResult Result = Good; ELayoutFailure Failure = ELayoutFailure::None;
        TestFalse(Case, ComputeLayout(Graph, {}, Result, Reason, &Failure));
        TestEqual(TEXT("Refusal distinguishes bad input from an unsatisfied constraint"), Failure, Expected);
        TestTrue(TEXT("Refused geometry exposes no partial or stale layout"),
            Result.Positions.IsEmpty() && Result.Sizes.IsEmpty() && Result.FeedbackEdges.Num() == 0);
        TestFalse(TEXT("Refusal explains why formatting cannot proceed"), Reason.IsEmpty());
    };
    FLayoutGraph Invalid = Valid;
    Invalid.Nodes[0].Geometry.BodySize.X = -1;
    Reject(TEXT("Negative measured body size is refused"), Invalid);
    Invalid = Valid; Invalid.Nodes[0].Geometry.VisualBounds.Max.Y = -1;
    Reject(TEXT("Inverted visual bounds are refused"), Invalid);
    Invalid = Valid; Invalid.Pins[Valid.Edges[0].To].Offset.Reset();
    Reject(TEXT("Missing linked-pin geometry is refused"), Invalid);
    Invalid = Valid; Invalid.Edges[0].To = Invalid.Pins.Num();
    Reject(TEXT("Missing linked-pin identity is refused before traversal"), Invalid);
    for (const float NonFinite : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        Invalid = Valid; Invalid.Nodes[0].Geometry.BodySize.Y = NonFinite;
        Reject(TEXT("Non-finite body geometry is refused"), Invalid);
        Invalid = Valid; Invalid.Nodes[0].Geometry.VisualBounds.Min.X = NonFinite;
        Reject(TEXT("Non-finite visual bounds are refused"), Invalid);
        for (const int32 Pin : {Valid.Edges[0].From, Valid.Edges[0].To})
        {
            for (int32 Axis = 0; Axis < 2; ++Axis)
            {
                Invalid = Valid; Invalid.Pins[Pin].Offset.GetValue()[Axis] = NonFinite;
                Reject(TEXT("Either coordinate of either connected pin must be finite"), Invalid);
            }
        }
    }
    for (const int32 Position : {-16777220, 16777220})
    {
        Invalid = Valid; Invalid.Nodes[0].Geometry.Position.X = Position;
        Reject(TEXT("An out-of-range anchor cannot publish unreliable positions"), Invalid, ELayoutFailure::Constraints);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutContinuationTest, "GlooPrint.Layout.ContinuationOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLayoutContinuationTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 8; ++I) { AddNode(Graph, {I * 40, I * 100}, {180, 100}); }
    for (int32 I = 1; I < 8; ++I) { Link(Graph, I - 1, I); }
    Link(Graph, 6, 2); Link(Graph, 1, 5); Graph.Anchor = 3;
    FLayoutResult Expected, Actual; FString Reason;
    if (!TestTrue(TEXT("Reference cycle and branch layout computes"), ComputeLayout(Graph, {}, Expected, Reason))) { AddError(Reason); return false; }
    FLayoutJob Job(Graph, {});
    Graph = {};
    for (int32 I = 0; I < 4; ++I)
    {
        TestEqual(TEXT("Expired budget advances exactly one bounded layout candidate"), Job.Advance(0), I == 3);
        if (I < 3)
        {
            TestFalse(TEXT("An incomplete layout exposes no result"), Job.TakeResult(Actual, Reason));
            TestTrue(TEXT("No partial node positions escape"), Actual.Positions.IsEmpty());
        }
    }
    TestTrue(TEXT("Complete layout can be consumed"), Job.TakeResult(Actual, Reason));
    TestTrue(TEXT("Yielding preserves every position and size"), Actual.Positions == Expected.Positions && Actual.Sizes == Expected.Sizes);
    TestTrue(TEXT("Yielding preserves cycle classification"), Actual.FeedbackEdges == Expected.FeedbackEdges);
    TestEqual(TEXT("Yielding preserves chosen sweep"), Actual.OrderingSweeps, Expected.OrderingSweeps);
    TestEqual(TEXT("Yielding preserves crossing score"), Actual.OrderingCrossings, Expected.OrderingCrossings);
    TestTrue(TEXT("Yielding preserves final candidate score"), Actual.LastCandidateCrossings == Expected.LastCandidateCrossings);
    TestFalse(TEXT("A moved result cannot be consumed twice"), Job.TakeResult(Actual, Reason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisconnectedPackingLayoutTest, "GlooPrint.Layout.DisconnectedGroupPacking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDisconnectedPackingLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 4; ++I) { AddNode(Graph, {I * 400, -700}, {240, 100}); }
    Link(Graph, 0, 1); Link(Graph, 1, 2); Link(Graph, 2, 3); Link(Graph, 3, 1); Link(Graph, 3, 0); Link(Graph, 2, 0);
    for (int32 I = 0; I < 4; ++I) { AddNode(Graph, {I * 300, 400}, {100, 80}); }
    Link(Graph, 4, 4);
    Graph.Anchor = 6;
    FString Reason;
    FLayoutResult Result;
    if (!TestTrue(TEXT("Mixed-width islands with feedback lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("An anchor in a later island stays fixed"), Result.Positions[6], Graph.Nodes[6].Geometry.Position);
    TestEqual(TEXT("Small islands fit in the same row below the wide chain"), Result.Positions[5].Y, Result.Positions[6].Y);
    TestTrue(TEXT("Small-island order is stable from left to right"), Result.Positions[4].X < Result.Positions[5].X && Result.Positions[5].X < Result.Positions[6].X);
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FRouteSet Routes;
        if (!TestTrue(TEXT("Packed cycle/self-loop routes compute"), ComputeLayoutRoutes(Graph, Result, Routes, Reason, Style))) { AddError(Reason); continue; }
        TestEqual(TEXT("Packed routes keep every original connection"), Routes.Wires.Num(), 7);
        TestEqual(TEXT("Packed cycle/self-loop corridors need no fallback"), Routes.FallbackCount, 0);
        for (const auto& Pair : Routes.Wires)
        {
            const bool bSelf = Pair.Key.FromNode == Graph.Nodes[4].Geometry.Id;
            for (int32 I = bSelf ? 5 : 4; I < Graph.Nodes.Num(); ++I)
            {
                const FVector2f Min(Result.Positions[I]); const FVector2f Max = Min + Graph.Nodes[I].Geometry.BodySize;
                TestFalse(TEXT("Return-wire bounds stay out of unrelated packed islands"),
                    Pair.Value.Bounds.Min.X < Max.X && Pair.Value.Bounds.Max.X > Min.X &&
                    Pair.Value.Bounds.Min.Y < Max.Y && Pair.Value.Bounds.Max.Y > Min.Y);
            }
        }
    }
    FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
    for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 I = 0; I < Shuffled.Edges.Num(); ++I)
    {
        const auto& Edge = Shuffled.Edges[I];
        Shuffled.Nodes[Shuffled.Pins[Edge.From].Node].Outgoing.Add(I); Shuffled.Nodes[Shuffled.Pins[Edge.To].Node].Incoming.Add(I);
    }
    FLayoutResult Reordered;
    if (TestTrue(TEXT("Reordered island edges compute"), ComputeLayout(Shuffled, {}, Reordered, Reason)))
    {
        TestTrue(TEXT("Island packing is independent of edge enumeration"), Reordered.Positions == Result.Positions && Reordered.Sizes == Result.Sizes);
    }
    else { AddError(Reason); }
    const int32 Inner = AddNode(Graph, {-100, -850}, {1600, 400});
    const int32 Outer = AddNode(Graph, {-200, -950}, {1900, 600});
    for (const int32 I : {Inner, Outer})
    {
        auto& Node = Graph.Nodes[I]; Node.bComment = true;
        Node.OriginalSize = FIntPoint(int32(Node.Geometry.BodySize.X), int32(Node.Geometry.BodySize.Y));
        Node.Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(Node.Geometry.BodySize.X, 30)};
    }
    FLayoutResult Nested;
    if (TestTrue(TEXT("Nested comments pack around feedback corridors"), ComputeLayout(Graph, {}, Nested, Reason)))
    {
        CheckColdIdempotence(*this, Graph, Nested);
        FLayoutGraph RoutingGraph = Graph;
        for (const int32 I : {Inner, Outer}) { RoutingGraph.Nodes[I].Geometry.CommentHeader->Max.X = float(Nested.Sizes[I].X); }
        for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes;
            if (!TestTrue(TEXT("Nested reserved lanes route in both styles"), ComputeLayoutRoutes(RoutingGraph, Nested, Routes, Reason, Style))) { AddError(Reason); continue; }
            TestEqual(TEXT("Nested return lanes need no fallback"), Routes.FallbackCount, 0);
            for (const auto& Pair : Routes.Wires)
            {
                if (Pair.Key.FromNode == Graph.Nodes[4].Geometry.Id) { continue; }
                const auto& Box = Pair.Value.Bounds;
                for (const int32 C : {Inner, Outer})
                {
                    const FVector2f Min(Nested.Positions[C]); const FVector2f Max = Min + FVector2f(Nested.Sizes[C]);
                    TestTrue(TEXT("Complete return route remains inside each padded comment and below its header"),
                        Box.Min.X >= Min.X + 32 && Box.Min.Y >= Min.Y + 30 + 32 &&
                        Box.Max.X <= Max.X - 32 && Box.Max.Y <= Max.Y - 32);
                }
            }
        }
    }
    else { AddError(Reason); }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridSpacingLayoutTest, "GlooPrint.Layout.GridSpacingAndPinAlignment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridSpacingLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    AddNode(Graph, {500, 300}, {128, 97}, 35, 59.25f);
    AddNode(Graph, {-503, -297}, {176, 101}, 43.5f, 70.25f);
    AddNode(Graph, {0, 600}, {207, 103}, 41, 43);
    Link(Graph, 0, 1); Link(Graph, 1, 2); Graph.Anchor = 1;
    FString Reason;
    for (const uint32 Grid : {1u, 16u, 32u, 100u})
    {
        FLayoutSettings Settings; Settings.GridSize = Grid;
        FLayoutResult Result;
        if (!TestTrue(TEXT("Variable native grid admits layout"), ComputeLayout(Graph, Settings, Result, Reason))) { AddError(Reason); continue; }
        TestEqual(TEXT("Off-grid selected anchor is fixed exactly"), Result.Positions[1], Graph.Nodes[1].Geometry.Position);
        for (int32 I = 0; I < Graph.Nodes.Num(); ++I)
        {
            TestEqual(FString::Printf(TEXT("Column %d respects the %u-unit grid relative to the fixed anchor"), I, Grid),
                (Result.Positions[I].X - Result.Positions[1].X) % int32(Grid), 0);
            if (I == 0) { continue; }
            const float Gap = Result.Positions[I].X - Result.Positions[I - 1].X - Graph.Nodes[I - 1].Geometry.BodySize.X;
            TestTrue(TEXT("Grid rounding retains horizontal clearance without adding more than one grid cell"),
                Gap >= Settings.HorizontalSpacing && Gap <= Settings.HorizontalSpacing + Grid);
        }
        for (const auto& Edge : Graph.Edges)
        {
            const auto& From = Graph.Pins[Edge.From]; const auto& To = Graph.Pins[Edge.To];
            TestTrue(TEXT("Fractional execution attachments take priority over Y snapping"), FMath::Abs(
                Result.Positions[From.Node].Y + From.Offset.GetValue().Y - Result.Positions[To.Node].Y - To.Offset.GetValue().Y) <= 0.5f);
        }
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result, Settings);
        FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
        for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
        for (int32 I = 0; I < Shuffled.Edges.Num(); ++I)
        {
            const auto& Edge = Shuffled.Edges[I];
            Shuffled.Nodes[Shuffled.Pins[Edge.From].Node].Outgoing.Add(I); Shuffled.Nodes[Shuffled.Pins[Edge.To].Node].Incoming.Add(I);
        }
        FLayoutResult Reordered;
        TestTrue(TEXT("Shuffled grid fixture computes"), ComputeLayout(Shuffled, Settings, Reordered, Reason));
        TestTrue(TEXT("Grid placement has stable tie-breaking"), Reordered.Positions == Result.Positions);
    }
    for (const uint32 Grid : {0u, 101u})
    {
        FLayoutSettings Invalid; Invalid.GridSize = Grid; FLayoutResult Result;
        TestFalse(TEXT("Invalid grid is refused before layout"), ComputeLayout(Graph, Invalid, Result, Reason));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTallSequenceLayoutTest, "GlooPrint.Layout.TallSequenceCompaction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTallSequenceLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    const int32 Entry = AddNode(Graph, {0, 0}, {160, 78}, 59, 59);
    const int32 Sequence = AddNode(Graph, {500, 0}, {140, 500}, 43, 43);
    for (int32 I = 1; I < 12; ++I)
    {
        Graph.Pins.Add({FGuid(0, 0, Sequence + 1, I + 2), Sequence, I + 1, true, ELinkKind::Execution, FVector2f(140, 43 + I * 32)});
        ++Graph.Nodes[Sequence].PinCount;
    }
    TArray<int32> Branches;
    for (int32 I = 0; I < 12; ++I)
    {
        const int32 Node = AddNode(Graph, {1000 - (I % 3) * 250, I * 50}, {144, 132}, 43, 43);
        Branches.Add(Node);
        Graph.Pins.Add({FGuid(0, 0, Node + 1, 3), Node, 2, I == 0, ELinkKind::Data, FVector2f(I == 0 ? 144 : 0, 74)});
        ++Graph.Nodes[Node].PinCount;
    }
    const int32 Join = AddNode(Graph, {0, 900}, {180, 132}, 43, 43);
    auto Connect = [&](int32 From, int32 To, ELinkKind Kind)
    {
        const int32 Edge = Graph.Edges.Add({From, To, Kind});
        Graph.Nodes[Graph.Pins[From].Node].Outgoing.Add(Edge); Graph.Nodes[Graph.Pins[To].Node].Incoming.Add(Edge);
    };
    Link(Graph, Entry, Sequence);
    for (int32 I = 0; I < Branches.Num(); ++I)
    {
        Connect(Graph.Nodes[Sequence].FirstPin + I + 1, Graph.Nodes[Branches[I]].FirstPin, ELinkKind::Execution);
        Link(Graph, Branches[I], Join);
    }
    Connect(Graph.Nodes[Branches[0]].FirstPin + 2, Graph.Nodes[Branches.Last()].FirstPin + 2, ELinkKind::Data);
    Graph.Anchor = Entry;
    FLayoutResult Result; FString Reason;
    if (!TestTrue(TEXT("Tall sequence, shared join and later-column sibling lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    const int32 Gap = Result.Positions[Branches[1]].Y - Result.Positions[Branches[0]].Y;
    AddInfo(FString::Printf(TEXT("Tall Sequence: first-to-second branch row gap %d."), Gap));
    TestEqual(TEXT("Tall Sequence does not reserve its height in the branch column"), Gap, 180);
    for (int32 I = 1; I < Branches.Num(); ++I)
    {
        TestTrue(TEXT("Every sibling retains visible pin order, including later columns"), Result.Positions[Branches[I]].Y > Result.Positions[Branches[I - 1]].Y);
    }
    TestTrue(TEXT("Data-dependent sibling actually occupies a later column"), Result.Positions[Branches.Last()].X > Result.Positions[Branches[0]].X);
    TestEqual(TEXT("Entry execution stays aligned with the tall Sequence"), Result.Positions[Entry].Y + 59, Result.Positions[Sequence].Y + 43);
    TestEqual(TEXT("Sequence main execution stays aligned"), Result.Positions[Sequence].Y, Result.Positions[Branches[0]].Y);
    TestEqual(TEXT("Shared join stays on the main chain"), Result.Positions[Join].Y, Result.Positions[Branches[0]].Y);
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    Algo::Reverse(Graph.Edges);
    for (auto& Node : Graph.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 I = 0; I < Graph.Edges.Num(); ++I)
    {
        const auto& Edge = Graph.Edges[I];
        Graph.Nodes[Graph.Pins[Edge.From].Node].Outgoing.Add(I); Graph.Nodes[Graph.Pins[Edge.To].Node].Incoming.Add(I);
    }
    FLayoutResult Shuffled;
    TestTrue(TEXT("Shuffled tall-Sequence fixture computes"), ComputeLayout(Graph, {}, Shuffled, Reason));
    TestTrue(TEXT("Column compaction retains deterministic placement"), Shuffled.Positions == Result.Positions);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCommentsTest, "GlooPrint.Layout.NestedAndAmbiguousComments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLayoutCommentsTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    AddNode(Graph, {100, 100}, {120, 100});
    AddNode(Graph, {100, 300}, {120, 100});
    AddNode(Graph, {500, 100}, {140, 100});
    Link(Graph, 0, 1); Link(Graph, 1, 2); Graph.Anchor = 0;
    auto AddComment = [&](FIntPoint Position, FVector2f Size)
    {
        const int32 I = AddNode(Graph, Position, Size);
        Graph.Nodes[I].bComment = true;
        Graph.Nodes[I].OriginalSize = FIntPoint(int32(Size.X), int32(Size.Y));
        Graph.Nodes[I].Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(Size.X, 30)};
        return I;
    };
    const int32 Inner = AddComment({50, 20}, {220, 430});
    const int32 Outer = AddComment({0, -50}, {750, 600});
    const int32 Empty = AddComment({-5000, -5000}, {200, 100});
    FLayoutResult Result;
    FString Reason;
    if (!TestTrue(TEXT("Nested comment groups lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestTrue(TEXT("Valid comments resize"), Result.Sizes[Inner] != Graph.Nodes[Inner].OriginalSize);
    TestEqual(TEXT("Empty comment does not move"), Result.Positions[Empty], Graph.Nodes[Empty].Geometry.Position);
    TestEqual(TEXT("Empty comment does not resize"), Result.Sizes[Empty], Graph.Nodes[Empty].OriginalSize);
    TestTrue(TEXT("Nested group has outer header clearance"), Result.Positions[Inner].Y >= Result.Positions[Outer].Y + 62);
    TestTrue(TEXT("Inner members lay out horizontally"), Result.Positions[1].X > Result.Positions[0].X);
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);

    AddComment({200, -25}, {700, 400});
    if (TestTrue(TEXT("Ambiguous overlapping region remains format-safe"), ComputeLayout(Graph, {}, Result, Reason)))
    {
        TestTrue(TEXT("Limited formatting reported"), Result.bLimitedComments);
        TestEqual(TEXT("Rigid region preserves relative node arrangement"), Result.Positions[1] - Result.Positions[0],
            Graph.Nodes[1].Geometry.Position - Graph.Nodes[0].Geometry.Position);
        TestEqual(TEXT("Ambiguous comment bounds preserved"), Result.Sizes[Outer], Graph.Nodes[Outer].OriginalSize);
        CheckColdIdempotence(*this, Graph, Result);
    }
    else { AddError(Reason); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFractionalCommentAlignmentTest, "GlooPrint.Layout.FractionalCommentAlignment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFractionalCommentAlignmentTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    AddNode(Graph, {-500, 0}, {120, 100}, 43, 43);
    AddNode(Graph, {100, 100}, {120, 100}, 43, 43);
    AddNode(Graph, {500, 100}, {120, 100}, 59, 59);
    AddNode(Graph, {1100, 100}, {120, 100}, 59, 59);
    Link(Graph, 0, 1); Link(Graph, 1, 2); Link(Graph, 2, 3);
    auto AddComment = [&](FIntPoint Position, FVector2f Size, float HeaderBottom)
    {
        const int32 I = AddNode(Graph, Position, Size);
        Graph.Nodes[I].bComment = true;
        Graph.Nodes[I].OriginalSize = FIntPoint(int32(Size.X), int32(Size.Y));
        Graph.Nodes[I].Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(Size.X, HeaderBottom)};
        return I;
    };
    const int32 Inner = AddComment({0, 0}, {320, 320}, 37.5f);
    const int32 Outer = AddComment({-50, -80}, {900, 600}, 41.25f);
    for (const int32 Anchor : {0, 1})
    {
        Graph.Anchor = Anchor;
        FLayoutResult Result; FString Reason;
        if (!TestTrue(TEXT("Fractional comment titles lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
        TestEqual(TEXT("Comment-chain anchor stays fixed"), Result.Positions[Anchor], Graph.Nodes[Anchor].Geometry.Position);
        for (const auto& Edge : Graph.Edges)
        {
            const auto& A = Graph.Pins[Edge.From]; const auto& B = Graph.Pins[Edge.To];
            TestEqual(TEXT("Execution alignment survives each comment boundary"),
                Result.Positions[A.Node].Y + A.Offset->Y, Result.Positions[B.Node].Y + B.Offset->Y);
        }
        TestTrue(TEXT("Inner title and padding remain clear"), Result.Positions[1].Y >= Result.Positions[Inner].Y + 69.5f);
        TestTrue(TEXT("Outer title and padding remain clear"), Result.Positions[Inner].Y >= Result.Positions[Outer].Y + 73.25f);
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCommentExecutionCorridorTest, "GlooPrint.Layout.CommentExecutionCorridors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCommentExecutionCorridorTest::RunTest(const FString& Parameters)
{
    for (const bool bOutgoing : {false, true})
    {
        FLayoutGraph Graph;
        AddNode(Graph, {-500, 100}, {120, 100});
        const int32 Flow = AddNode(Graph, {500, 100}, {160, 120});
        const int32 Next = AddNode(Graph, {1500, 100}, {120, 100});
        const int32 A = AddNode(Graph, {100, 150}, {100, 60}, 30, 30);
        const int32 B = AddNode(Graph, {300, 150}, {100, 60}, 30, 30);
        for (const int32 I : {A, B})
        {
            Graph.Pins[Graph.Nodes[I].FirstPin].Kind = Graph.Pins[Graph.Nodes[I].FirstPin + 1].Kind = ELinkKind::Data;
        }
        Link(Graph, 0, Flow); Link(Graph, Flow, Next); Link(Graph, A, B, ELinkKind::Data);
        auto ExtraPin = [&](int32 Node, bool bOutput)
        {
            const int32 Ordinal = Graph.Nodes[Node].PinCount++;
            return Graph.Pins.Add({FGuid(0, 2, Node + 1, Ordinal + 1), Node, Ordinal, bOutput, ELinkKind::Data,
                FVector2f(bOutput ? Graph.Nodes[Node].Geometry.BodySize.X : 0, 60)});
        };
        auto DataLink = [&](int32 From, int32 To)
        {
            const int32 E = Graph.Edges.Add({From, To, ELinkKind::Data});
            Graph.Nodes[Graph.Pins[From].Node].Outgoing.Add(E); Graph.Nodes[Graph.Pins[To].Node].Incoming.Add(E);
        };
        DataLink(Graph.Nodes[B].FirstPin + 1, ExtraPin(bOutgoing ? Next : Flow, false));
        if (bOutgoing) { DataLink(ExtraPin(Flow, true), Graph.Nodes[A].FirstPin); }
        const int32 C = Graph.Nodes.AddDefaulted(); auto& Comment = Graph.Nodes[C];
        Comment.Geometry.Id = FGuid(0, 0, 0, C + 1); Comment.Geometry.Position = {0, 0}; Comment.Geometry.BodySize = {1000, 400};
        Comment.Geometry.VisualBounds = {FVector2f::ZeroVector, Comment.Geometry.BodySize};
        Comment.Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(1000, 37.5f)};
        Comment.bComment = true; Comment.OriginalSize = {1000, 400}; Graph.Anchor = 0;
        FString Reason; FLayoutResult Result;
        if (!TestTrue(TEXT("Comment boundary execution corridor lays out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); continue; }
        const float Row = Result.Positions[Flow].Y + 40;
        for (const int32 I : {A, B})
        {
            TestTrue(TEXT("Complete pure expression clears the boundary execution row"),
                Result.Positions[I].Y >= Row + WireNodeClearance || Result.Positions[I].Y + 60 <= Row - WireNodeClearance);
        }
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
        for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes;
            if (!TestTrue(TEXT("Comment corridors route in either style"), ComputeLayoutRoutes(Graph, Result, Routes, Reason, Style))) { AddError(Reason); continue; }
            for (const auto& Edge : Graph.Edges)
            {
                if (Edge.Kind != ELinkKind::Execution) { continue; }
                const auto& From = Graph.Pins[Edge.From]; const auto& To = Graph.Pins[Edge.To];
                const auto* Route = Routes.Wires.Find({Graph.Nodes[From.Node].Geometry.Id, From.Id, Graph.Nodes[To.Node].Geometry.Id, To.Id});
                if (TestTrue(TEXT("Boundary execution keeps its original connection"), Route != nullptr))
                {
                    TestEqual(TEXT("Incoming and outgoing execution cross the comment without detours"), Route->Curves.Num(), 1);
                }
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCommentConsumerOrderTest, "GlooPrint.Layout.CommentConsumerPinOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCommentConsumerOrderTest::RunTest(const FString& Parameters)
{
    for (const bool bRelays : {false, true})
    {
        FLayoutGraph Graph;
        const int32 Consumer = AddNode(Graph, {1800, -100}, {240, 320});
        const int32 Entry = AddNode(Graph, {1400, -300}, {160, 80});
        Link(Graph, Entry, Consumer); Graph.Anchor = Entry;
        auto DataNode = [&](FIntPoint Position, FVector2f Size)
        {
            const int32 Node = AddNode(Graph, Position, Size, 30, 30);
            Graph.Pins[Graph.Nodes[Node].FirstPin].Kind = Graph.Pins[Graph.Nodes[Node].FirstPin + 1].Kind = ELinkKind::Data;
            return Node;
        };
        auto Input = [&](int32 Node, float Y)
        {
            const int32 Ordinal = Graph.Nodes[Node].PinCount++;
            return Graph.Pins.Add({FGuid(0, 9, Node + 1, Ordinal + 1), Node, Ordinal, false, ELinkKind::Data, FVector2f(0, Y)});
        };
        auto ToPin = [&](int32 Node, int32 Pin)
        {
            const int32 E = Graph.Edges.Add({Graph.Nodes[Node].FirstPin + 1, Pin, ELinkKind::Data});
            Graph.Nodes[Node].Outgoing.Add(E); Graph.Nodes[Graph.Pins[Pin].Node].Incoming.Add(E);
        };
        const int32 Root = DataNode({100, 100}, {100, 60});
        int32 Calls[4], Leaves[4];
        for (int32 Branch = 3; Branch >= 0; --Branch)
        {
            Calls[Branch] = Leaves[Branch] = DataNode({300, 250 + Branch * 200}, {160, 120});
            Link(Graph, Root, Calls[Branch], ELinkKind::Data);
            if (Branch % 2)
            {
                Leaves[Branch] = DataNode({700, 250 + Branch * 200}, {160, 100});
                Link(Graph, Calls[Branch], Leaves[Branch], ELinkKind::Data);
                const int32 Literal = DataNode({520, 380 + Branch * 200}, {100, 40});
                ToPin(Literal, Input(Leaves[Branch], 80));
            }
            const int32 Pin = Input(Consumer, 120 + Branch * 40);
            if (bRelays)
            {
                const int32 Relay = DataNode({1400, 200 + Branch * 150}, {42, 24});
                Graph.Nodes[Relay].bReroute = true;
                Link(Graph, Leaves[Branch], Relay, ELinkKind::Data); ToPin(Relay, Pin);
            }
            else { ToPin(Leaves[Branch], Pin); }
        }
        const int32 CommentIndex = Graph.Nodes.AddDefaulted(); auto& Comment = Graph.Nodes[CommentIndex];
        Comment.Geometry.Id = FGuid(0, 0, 0, CommentIndex + 1); Comment.Geometry.Position = {0, 0};
        Comment.Geometry.BodySize = {1100, 1300}; Comment.Geometry.VisualBounds = {FVector2f::ZeroVector, Comment.Geometry.BodySize};
        Comment.Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(1100, 30)};
        Comment.bComment = true; Comment.OriginalSize = {1100, 1300};
        FString Reason; FLayoutResult Result;
        if (!TestTrue(TEXT("Shared comment outputs lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); continue; }
        for (int32 Branch = 1; Branch < 4; ++Branch)
        {
            TestTrue(TEXT("Calculations follow the external consumer's visible pin order"),
                Result.Positions[Calls[Branch - 1]].Y < Result.Positions[Calls[Branch]].Y);
            TestTrue(TEXT("Mixed-depth comment outputs retain the same vertical order"),
                Result.Positions[Leaves[Branch - 1]].Y < Result.Positions[Leaves[Branch]].Y);
        }
        TestEqual(TEXT("External execution chain stays straight"), Result.Positions[Entry].Y, Result.Positions[Consumer].Y);
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
        FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
        for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
        for (int32 E = 0; E < Shuffled.Edges.Num(); ++E)
        {
            Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].From].Node].Outgoing.Add(E);
            Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].To].Node].Incoming.Add(E);
        }
        FLayoutResult Reordered;
        if (TestTrue(TEXT("Shuffled external consumers lay out"), ComputeLayout(Shuffled, {}, Reordered, Reason)))
        {
            TestTrue(TEXT("Consumer ordering ignores edge enumeration"), Reordered.Positions == Result.Positions && Reordered.Sizes == Result.Sizes);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDataCommentPackingTest, "GlooPrint.Layout.DataCommentBesideFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDataCommentPackingTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    const int32 Entry = AddNode(Graph, {-500, 0}, {160, 100});
    const int32 Loop = AddNode(Graph, {450, 90}, {160, 130});
    const int32 First = AddNode(Graph, {1650, 90}, {160, 100});
    const int32 Last = AddNode(Graph, {1900, 90}, {160, 100});
    Link(Graph, Entry, Loop); Link(Graph, Loop, First); Link(Graph, First, Last);
    auto AddData = [&](FIntPoint Position, FVector2f Size)
    {
        const int32 I = AddNode(Graph, Position, Size, 30, 30);
        Graph.Pins[Graph.Nodes[I].FirstPin].Kind = Graph.Pins[Graph.Nodes[I].FirstPin + 1].Kind = ELinkKind::Data;
        return I;
    };
    const int32 Root = AddData({-500, 800}, {120, 60});
    const int32 Shared = AddData({0, 800}, {42, 24});
    const int32 A = AddData({350, 500}, {100, 60});
    const int32 B = AddData({550, 500}, {100, 60});
    const int32 C = AddData({750, 500}, {100, 60});
    const int32 Relay = AddData({1400, 800}, {42, 24});
    Link(Graph, Root, Shared, ELinkKind::Data); Link(Graph, Shared, A, ELinkKind::Data);
    Link(Graph, A, B, ELinkKind::Data); Link(Graph, B, C, ELinkKind::Data); Link(Graph, C, Relay, ELinkKind::Data);
    auto ExtraPin = [&](int32 Node, bool bOutput, FVector2f Offset)
    {
        const int32 Ordinal = Graph.Nodes[Node].PinCount++;
        return Graph.Pins.Add({FGuid(0, 2, Node + 1, Ordinal + 1), Node, Ordinal, bOutput, ELinkKind::Data, Offset});
    };
    auto DataLink = [&](int32 From, int32 To)
    {
        const int32 E = Graph.Edges.Add({From, To, ELinkKind::Data});
        Graph.Nodes[Graph.Pins[From].Node].Outgoing.Add(E); Graph.Nodes[Graph.Pins[To].Node].Incoming.Add(E);
    };
    DataLink(Graph.Nodes[Shared].FirstPin + 1, ExtraPin(Loop, false, {0, 80}));
    DataLink(ExtraPin(Loop, true, {160, 80}), ExtraPin(A, false, {0, 45}));
    DataLink(Graph.Nodes[Relay].FirstPin + 1, ExtraPin(Last, false, {0, 75}));
    auto AddComment = [&](FIntPoint Position, FVector2f Size)
    {
        const int32 I = Graph.Nodes.AddDefaulted(); auto& Node = Graph.Nodes[I];
        Node.Geometry.Id = FGuid(0, 0, 0, I + 1); Node.Geometry.Position = Position; Node.Geometry.BodySize = Size;
        Node.Geometry.VisualBounds = {FVector2f::ZeroVector, Size};
        Node.Geometry.CommentHeader = FMeasuredRect{FVector2f::ZeroVector, FVector2f(Size.X, 37.5f)};
        Node.bComment = true; Node.OriginalSize = FIntPoint(int32(Size.X), int32(Size.Y));
        return I;
    };
    const int32 LoopComment = AddComment({400, 0}, {260, 250});
    AddComment({1600, 0}, {550, 250});
    const int32 DataComment = AddComment({300, 400}, {650, 250});
    Graph.Anchor = Entry; FString Reason; FLayoutResult Result;
    if (!TestTrue(TEXT("Data comment fed by a flow value lays out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestTrue(TEXT("Data comment shares horizontal space with the loop group"),
        Result.Positions[DataComment].X < Result.Positions[LoopComment].X + Result.Sizes[LoopComment].X &&
        Result.Positions[LoopComment].X < Result.Positions[DataComment].X + Result.Sizes[DataComment].X);
    TestTrue(TEXT("Data comment clears the complete execution group"),
        Result.Positions[DataComment].Y >= Result.Positions[LoopComment].Y + Result.Sizes[LoopComment].Y + 48);
    for (const int32 Node : {Loop, First, Last}) { TestEqual(TEXT("Comment packing retains the execution spine"), Result.Positions[Node].Y, Result.Positions[Entry].Y); }
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
    for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 E = 0; E < Shuffled.Edges.Num(); ++E)
    {
        Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].From].Node].Outgoing.Add(E);
        Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].To].Node].Incoming.Add(E);
    }
    FLayoutResult Reordered;
    if (TestTrue(TEXT("Shuffled comment dependencies compute"), ComputeLayout(Shuffled, {}, Reordered, Reason)))
    {
        TestTrue(TEXT("Comment packing ignores edge enumeration order"), Reordered.Positions == Result.Positions && Reordered.Sizes == Result.Sizes);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutLongCycleTest, "GlooPrint.Layout.LongCycleIsBounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLayoutLongCycleTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 2000; ++I)
    {
        AddNode(Graph, {0, 0}, {120, 80});
        if (I) { Link(Graph, I - 1, I); }
    }
    Link(Graph, 1999, 0); Graph.Anchor = 0;
    FLayoutResult Result;
    FString Reason;
    if (!TestTrue(TEXT("Long cycle finishes without recursion"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("All nodes retained"), Result.Positions.Num(), 2000);
    TestEqual(TEXT("One feedback link for a simple cycle"), Result.FeedbackEdges.CountSetBits(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPureGapLayoutTest, "GlooPrint.Layout.PureInputGapPacking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPureGapLayoutTest::RunTest(const FString& Parameters)
{
    for (int32 Variant = 0; Variant < 4; ++Variant)
    {
        const bool bTallInput = (Variant & 1) != 0, bMultipleInputs = (Variant & 2) != 0;
        FLayoutGraph Graph;
        for (int32 Chain = 0; Chain < 2; ++Chain)
        {
            for (int32 Step = 0; Step < 5; ++Step)
            {
                const float Height = Chain == 0 && Step == 4 ? 400.f : 140.f;
                const int32 Node = AddNode(Graph, {Step * -200, Chain * 800}, {160, Height});
                Graph.Pins.Add({FGuid(0, 1, Node + 1, 1), Node, 2, false, ELinkKind::Data, FVector2f(0, 60)});
                ++Graph.Nodes[Node].PinCount;
                if (Step) { Link(Graph, Node - 1, Node); }
            }
        }
        const int32 Input = AddNode(Graph, {-900, -500}, {100, 70}, 35, 35);
        const int32 Shared = AddNode(Graph, {-700, -500}, {120, bTallInput ? 600.f : 50.f}, 20, 20);
        for (const int32 Node : {Input, Shared})
        {
            Graph.Pins[Graph.Nodes[Node].FirstPin].Kind = ELinkKind::Data;
            Graph.Pins[Graph.Nodes[Node].FirstPin + 1].Kind = ELinkKind::Data;
            Graph.Nodes[Node].Geometry.VisualBounds.Min.Y = -0.25f;
            Graph.Nodes[Node].Geometry.VisualBounds.Max.Y += 0.6f;
        }
        Link(Graph, Input, Shared, ELinkKind::Data);
        for (const int32 Consumer : {3, 8})
        {
            const int32 E = Graph.Edges.Add({Graph.Nodes[Shared].FirstPin + 1, Graph.Nodes[Consumer].FirstPin + 2, ELinkKind::Data});
            Graph.Nodes[Shared].Outgoing.Add(E); Graph.Nodes[Consumer].Incoming.Add(E);
        }
        TArray<int32> Dependencies{Input, Shared};
        if (bMultipleInputs)
        {
            const int32 Pin = Graph.Pins.Add({FGuid(0, 1, Shared + 1, 3), Shared, 2, false, ELinkKind::Data, FVector2f(0, 40)});
            ++Graph.Nodes[Shared].PinCount;
            const int32 Second = AddNode(Graph, {-1100, -900}, {110, 90}, 35, 35);
            for (int32 P = Graph.Nodes[Second].FirstPin; P < Graph.Nodes[Second].FirstPin + 2; ++P) { Graph.Pins[P].Kind = ELinkKind::Data; }
            Graph.Nodes[Second].Geometry.VisualBounds.Min.Y = -0.4f;
            Graph.Nodes[Second].Geometry.VisualBounds.Max.Y += 0.7f;
            const int32 E = Graph.Edges.Add({Graph.Nodes[Second].FirstPin + 1, Pin, ELinkKind::Data});
            Graph.Nodes[Second].Outgoing.Add(E); Graph.Nodes[Shared].Incoming.Add(E);
            Dependencies.Add(Second);
        }
        Graph.Anchor = 0;
        FString Reason; FLayoutResult Result;
        if (!TestTrue(TEXT("Shared dependency with a free execution-row gap lays out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); continue; }
        TestEqual(TEXT("Gap packing retains the selected entry"), Result.Positions[0], Graph.Nodes[0].Geometry.Position);
        for (const int32 Node : Dependencies)
        {
            const int32 Column = Node == Shared ? 2 : 1;
            const float Top = Result.Positions[Node].Y + Graph.Nodes[Node].Geometry.VisualBounds.Min.Y;
            const float Bottom = Result.Positions[Node].Y + Graph.Nodes[Node].Geometry.VisualBounds.Max.Y;
            if (!bTallInput)
            {
                TestTrue(TEXT("Shared expression and its dependency use the existing gap with measured clearance"),
                    Top >= Result.Positions[Column].Y + 140 + 48 && Bottom + 48 <= Result.Positions[Column + 5].Y);
            }
            for (const int32 Flow : {Column, Column + 5})
            {
                TestTrue(TEXT("Rounded integer positions retain full visual clearance"),
                    Bottom + 48 <= Result.Positions[Flow].Y || Top >= Result.Positions[Flow].Y + 140 + 48);
            }
        }
        if (bMultipleInputs)
        {
            const int32 Second = Dependencies.Last();
            const float FirstBottom = Result.Positions[Input].Y + Graph.Nodes[Input].Geometry.VisualBounds.Max.Y;
            const float SecondTop = Result.Positions[Second].Y + Graph.Nodes[Second].Geometry.VisualBounds.Min.Y;
            TestTrue(TEXT("Connected sibling inputs reserve a compact block with full fractional clearance"),
                SecondTop - FirstBottom >= 48.f && SecondTop - FirstBottom < 49.f);
        }
        if (bTallInput) { TestTrue(TEXT("A dependency too tall for the gap goes below both rows"), Result.Positions[Shared].Y >= Result.Positions[7].Y + 140 + 48); }
        for (int32 Chain = 0; Chain < 2; ++Chain)
        {
            for (int32 Step = 1; Step < 5; ++Step) { TestEqual(TEXT("Gap packing leaves execution pins straight"), Result.Positions[Chain * 5 + Step].Y, Result.Positions[Chain * 5].Y); }
        }
        CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
        for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes;
            if (TestTrue(TEXT("Packed dependencies retain routes in both styles"), ComputeLayoutRoutes(Graph, Result, Routes, Reason, Style)))
            {
                TestEqual(TEXT("Gap packing retains all original links"), Routes.Wires.Num(), bMultipleInputs ? 12 : 11);
                TestEqual(TEXT("Gap packing needs no fallback"), Routes.FallbackCount, 0);
                for (const auto& Pair : Routes.Wires)
                {
                    TestTrue(FString::Printf(TEXT("Gap route %s -> %s is custom (fallback %d)"),
                        *Pair.Key.FromNode.ToString(), *Pair.Key.ToNode.ToString(), int32(Pair.Value.Fallback)), Pair.Value.Fallback == ERouteFallback::None);
                }
            }
            else { AddError(Reason); }
        }
        FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
        for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
        for (int32 E = 0; E < Shuffled.Edges.Num(); ++E)
        {
            Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].From].Node].Outgoing.Add(E);
            Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].To].Node].Incoming.Add(E);
        }
        FLayoutResult Reordered;
        TestTrue(TEXT("Shuffled gap-packing graph computes"), ComputeLayout(Shuffled, {}, Reordered, Reason));
        TestTrue(TEXT("Gap packing is independent of link enumeration"), Reordered.Positions == Result.Positions);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLaneCapacityLayoutTest, "GlooPrint.Layout.HorizontalWireCapacity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLaneCapacityLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    const int32 Entry = AddNode(Graph, {0, 0}, {160, 100});
    const int32 Sequence = AddNode(Graph, {500, 0}, {140, 440});
    for (int32 I = 1; I < 12; ++I)
    {
        Graph.Pins.Add({FGuid(0, 0, Sequence + 1, I + 2), Sequence, I + 1, true, ELinkKind::Execution, FVector2f(140, 40 + I * 32)});
        ++Graph.Nodes[Sequence].PinCount;
    }
    const int32 Shared = AddNode(Graph, {-400, 900}, {240, 80});
    Graph.Pins[Graph.Nodes[Shared].FirstPin].Kind = ELinkKind::Data;
    Graph.Pins[Graph.Nodes[Shared].FirstPin + 1].Kind = ELinkKind::Data;
    const int32 Join = AddNode(Graph, {0, 1200}, {160, 100});
    Link(Graph, Entry, Sequence);
    for (int32 I = 0; I < 12; ++I)
    {
        const int32 Consumer = AddNode(Graph, {I * -200, 700}, {float(160 + (I % 3) * 32), 100});
        const int32 Data = Graph.Pins.Add({FGuid(0, 1, Consumer + 1, 1), Consumer, 2, false, ELinkKind::Data, FVector2f(0, 72)});
        ++Graph.Nodes[Consumer].PinCount;
        for (const auto Pair : {FIntPoint(Graph.Nodes[Sequence].FirstPin + I + 1, Graph.Nodes[Consumer].FirstPin),
            FIntPoint(Graph.Nodes[Shared].FirstPin + 1, Data)})
        {
            const int32 E = Graph.Edges.Add({Pair.X, Pair.Y, Pair.Y == Data ? ELinkKind::Data : ELinkKind::Execution});
            Graph.Nodes[Graph.Pins[Pair.X].Node].Outgoing.Add(E); Graph.Nodes[Consumer].Incoming.Add(E);
        }
        Link(Graph, Consumer, Join);
    }
    Graph.Anchor = Entry;
    FString Reason; FLayoutResult Result;
    if (!TestTrue(TEXT("Mixed-width dense dependencies lay out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Dense selected entry stays fixed"), Result.Positions[Entry], Graph.Nodes[Entry].Geometry.Position);
    TestTrue(TEXT("An uncrowded execution column keeps normal horizontal spacing"),
        Result.Positions[Sequence].X - Result.Positions[Entry].X - 160 >= 96 &&
        Result.Positions[Sequence].X - Result.Positions[Entry].X - 160 <= 112);
    for (int32 I = 4; I < Graph.Nodes.Num(); ++I)
    {
        TestTrue(TEXT("Dense dependencies retain rightward body clearance"),
            Result.Positions[I].X >= Result.Positions[Shared].X + 240 + 96 &&
            Result.Positions[Join].X >= Result.Positions[I].X + Graph.Nodes[I].Geometry.BodySize.X + 96);
        if (I > 4) { TestEqual(TEXT("Lane capacity does not inflate vertical branch spacing"), Result.Positions[I].Y - Result.Positions[I - 1].Y, 148); }
    }
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FRouteSet Routes;
        if (TestTrue(TEXT("Reserved dense layout routes without spacing repair"), ComputeLayoutRoutes(Graph, Result, Routes, Reason, Style)))
        {
            TestEqual(TEXT("Dense layout keeps all thirty-seven original wires"), Routes.Wires.Num(), 37);
            TestEqual(TEXT("Reserved corridors need no native fallback"), Routes.FallbackCount, 0);
        }
        else { AddError(Reason); }
    }
    FLayoutGraph Shuffled = Graph; Algo::Reverse(Shuffled.Edges);
    for (auto& Node : Shuffled.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 E = 0; E < Shuffled.Edges.Num(); ++E)
    {
        Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].From].Node].Outgoing.Add(E);
        Shuffled.Nodes[Shuffled.Pins[Shuffled.Edges[E].To].Node].Incoming.Add(E);
    }
    FLayoutResult Reordered;
    TestTrue(TEXT("Shuffled wire capacity computes"), ComputeLayout(Shuffled, {}, Reordered, Reason));
    TestTrue(TEXT("Wire capacity is independent of edge enumeration"), Reordered.Positions == Result.Positions);

    FLayoutGraph Long;
    AddNode(Long, {0, 0}, {160, 100});
    const int32 DataOutput = Long.Pins.Add({FGuid(0, 1, 1, 1), 0, 2, true, ELinkKind::Data, FVector2f(160, 80)});
    ++Long.Nodes[0].PinCount;
    for (int32 I = 1; I < 4; ++I) { AddNode(Long, {I * -200, 0}, {160, I == 3 ? 440.f : 100.f}); Link(Long, I - 1, I); }
    for (int32 I = 1; I < 12; ++I)
    {
        Long.Pins.Add({FGuid(0, 0, 4, I + 2), 3, I + 1, true, ELinkKind::Execution, FVector2f(160, 40 + I * 32)});
        ++Long.Nodes[3].PinCount;
    }
    for (int32 I = 0; I < 12; ++I)
    {
        const int32 Consumer = AddNode(Long, {I * -200, 700}, {160, 100});
        const int32 Data = Long.Pins.Add({FGuid(0, 1, Consumer + 1, 1), Consumer, 2, false, ELinkKind::Data, FVector2f(0, 72)});
        ++Long.Nodes[Consumer].PinCount;
        for (const auto Pair : {FIntPoint(Long.Nodes[3].FirstPin + I + 1, Long.Nodes[Consumer].FirstPin), FIntPoint(DataOutput, Data)})
        {
            const int32 E = Long.Edges.Add({Pair.X, Pair.Y, Pair.Y == Data ? ELinkKind::Data : ELinkKind::Execution});
            Long.Nodes[Long.Pins[Pair.X].Node].Outgoing.Add(E); Long.Nodes[Consumer].Incoming.Add(E);
        }
    }
    Long.Anchor = 0; FLayoutResult LongResult;
    if (TestTrue(TEXT("Long shared-data links reserve their terminal columns"), ComputeLayout(Long, {}, LongResult, Reason)))
    {
        TestTrue(TEXT("Crowded long-link start gap reserves more than normal spacing"), LongResult.Positions[1].X - LongResult.Positions[0].X - 160 > 112);
        TestTrue(TEXT("Crowded long-link end gap reserves more than normal spacing"), LongResult.Positions[4].X - LongResult.Positions[3].X - 160 > 112);
        for (int32 I = 1; I < 3; ++I) { TestEqual(TEXT("Long links do not inflate intervening straight corridors"), LongResult.Positions[I + 1].X - LongResult.Positions[I].X - 160, 112); }
        CheckNoOverlap(*this, Long, LongResult); CheckColdIdempotence(*this, Long, LongResult);
        for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes;
            if (TestTrue(TEXT("Long dense links have complete routes"), ComputeLayoutRoutes(Long, LongResult, Routes, Reason, Style)))
            {
                TestEqual(TEXT("Long-link capacity preserves original wires"), Routes.Wires.Num(), 27);
                TestEqual(TEXT("Long-link terminal lanes need no fallback"), Routes.FallbackCount, 0);
                for (const auto& Pair : Routes.Wires)
                {
                    if (Pair.Value.Fallback != ERouteFallback::None)
                    {
                        AddInfo(FString::Printf(TEXT("Long-link fallback %s -> %s: reason%d, expansions%d."),
                            *Pair.Key.FromNode.ToString(), *Pair.Key.ToNode.ToString(), int32(Pair.Value.Fallback), Pair.Value.Search.ExpandedStates));
                    }
                    if (Pair.Key.FromNode == Long.Nodes[0].Geometry.Id && Pair.Key.ToNode == Long.Nodes[6].Geometry.Id)
                    {
                        TestTrue(TEXT("Long fan detour remains a complete custom route"), Pair.Value.Method != ERouteMethod::Native &&
                            Pair.Value.Points.Num() >= 4 && !Pair.Value.Curves.IsEmpty());
                        AddInfo(FString::Printf(TEXT("Long fan detour method%d, %d search expansions."), int32(Pair.Value.Method), Pair.Value.Search.ExpandedStates));
                        TestTrue(TEXT("Long detour retains the bounded search"), Pair.Value.Search.ExpandedStates <= 2048);
                        for (const auto& Curve : Pair.Value.Curves)
                        {
                            for (int32 Sample = 1; Sample < 100; ++Sample)
                            {
                                const FVector2f P = FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, Sample / 100.f);
                                for (int32 N = 0; N < Long.Nodes.Num(); ++N)
                                {
                                    const FVector2f Min(LongResult.Positions[N]), Max = Min + Long.Nodes[N].Geometry.BodySize;
                                    TestFalse(TEXT("Shifted outer detour clears every measured body"), P.X > Min.X && P.X < Max.X && P.Y > Min.Y && P.Y < Max.Y);
                                }
                            }
                        }
                    }
                }
                FRouteSet Cold;
                if (TestTrue(TEXT("Long detours route with fresh scratch state"), ComputeLayoutRoutes(Long, LongResult, Cold, Reason, Style)))
                {
                    for (const auto& Pair : Routes.Wires)
                    {
                        const auto* Wire = Cold.Wires.Find(Pair.Key);
                        TestTrue(TEXT("Cold long routes preserve original endpoints and exact paths"), Wire && Wire->Points == Pair.Value.Points && Wire->Fallback == Pair.Value.Fallback);
                    }
                }
                else { AddError(Reason); }
            }
            else { AddError(Reason); }
        }
    }
    else { AddError(Reason); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSharedDataLayoutTest, "GlooPrint.Layout.SharedInputsNearConsumers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSharedDataLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 Chain = 0; Chain < 2; ++Chain)
    {
        for (int32 Step = 0; Step < 5; ++Step)
        {
            const int32 Node = AddNode(Graph, {Step * -70, Chain * 400}, {160, 100});
            Graph.Pins.Add({FGuid(0, 1, Node + 1, 1), Node, 2, false, ELinkKind::Data, FVector2f(0, 75)});
            ++Graph.Nodes[Node].PinCount;
            if (Step) { Link(Graph, Node - 1, Node); }
            else { Graph.Nodes[Node].bEntry = true; }
        }
    }
    const int32 Input = AddNode(Graph, {-700, -500}, {100, 70}, 35, 35);
    const int32 Shared = AddNode(Graph, {-800, -400}, {120, 70}, 35, 35);
    for (const int32 Node : {Input, Shared})
    {
        Graph.Pins[Graph.Nodes[Node].FirstPin].Kind = ELinkKind::Data;
        Graph.Pins[Graph.Nodes[Node].FirstPin + 1].Kind = ELinkKind::Data;
    }
    Link(Graph, Input, Shared, ELinkKind::Data);
    for (const int32 Consumer : {4, 9})
    {
        const int32 Pin = Graph.Nodes[Consumer].FirstPin + 2;
        const int32 Edge = Graph.Edges.Add({Graph.Nodes[Shared].FirstPin + 1, Pin, ELinkKind::Data});
        Graph.Nodes[Shared].Outgoing.Add(Edge); Graph.Nodes[Consumer].Incoming.Add(Edge);
    }
    Graph.Anchor = 0;
    FLayoutResult Result; FString Reason;
    if (!TestTrue(TEXT("Two execution chains sharing one expression lay out"), ComputeLayout(Graph, {}, Result, Reason)))
    {
        AddError(Reason); return false;
    }
    TestEqual(TEXT("A shared producer remains one node in one placement"), Result.Positions.Num(), 12);
    TestEqual(TEXT("Selected execution entry stays fixed"), Result.Positions[0], Graph.Nodes[0].Geometry.Position);
    const float Gap = Result.Positions[4].X - (Result.Positions[Shared].X + 120);
    TestTrue(TEXT("Shared expression sits within one spacing interval of its consumers"), Gap >= 96 && Gap <= 192);
    TestTrue(TEXT("Expression's own input remains to its left with clearance"),
        Result.Positions[Input].X + 100 + 96 <= Result.Positions[Shared].X);
    for (int32 Chain = 0; Chain < 2; ++Chain)
    {
        for (int32 Step = 1; Step < 5; ++Step)
        {
            const int32 Node = Chain * 5 + Step;
            TestEqual(TEXT("Pure input placement keeps execution attachments aligned"), Result.Positions[Node].Y, Result.Positions[Node - 1].Y);
            TestTrue(TEXT("Execution retains width and routing clearance"), Result.Positions[Node].X >= Result.Positions[Node - 1].X + 256);
        }
    }
    CheckNoOverlap(*this, Graph, Result); CheckColdIdempotence(*this, Graph, Result);
    const FLayoutResult Original = Result;
    Algo::Reverse(Graph.Edges);
    for (auto& Node : Graph.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 E = 0; E < Graph.Edges.Num(); ++E)
    {
        Graph.Nodes[Graph.Pins[Graph.Edges[E].From].Node].Outgoing.Add(E);
        Graph.Nodes[Graph.Pins[Graph.Edges[E].To].Node].Incoming.Add(E);
    }
    if (TestTrue(TEXT("Permuted links still produce a valid shared layout"), ComputeLayout(Graph, {}, Result, Reason)))
    {
        TestTrue(TEXT("Shared placement is independent of edge enumeration"), Result.Positions == Original.Positions);
    }
    else { AddError(Reason); }
    AddInfo(FString::Printf(TEXT("Shared expression: horizontal consumer gap %.1f; first chain Y %d..%d; second chain Y %d..%d."),
        Gap, Original.Positions[0].Y, Original.Positions[4].Y, Original.Positions[5].Y, Original.Positions[9].Y));
    return true;
}
}

#endif
