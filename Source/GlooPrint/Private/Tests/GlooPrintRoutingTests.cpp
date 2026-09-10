// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintRouting.h"
#include "GlooPrintRouteChannels.h"
#include "GlooPrintObstacleIndex.h"
#include "Misc/AutomationTest.h"

namespace GlooPrint::Tests
{
namespace
{
int32 RouteNode(FLayoutGraph& Graph, FIntPoint Position, FVector2f Size)
{
    const int32 Index = Graph.Nodes.AddDefaulted();
    auto& Node = Graph.Nodes[Index];
    Node.Geometry.Id = FGuid(0, 0, 0, Index + 1);
    Node.Geometry.Position = Position; Node.Geometry.BodySize = Size;
    Node.FirstPin = Graph.Pins.Num(); Node.PinCount = 2;
    Graph.Pins.Add({FGuid(0, 0, Index + 1, 1), Index, 0, false, ELinkKind::Execution, FVector2f(0, 40)});
    Graph.Pins.Add({FGuid(0, 0, Index + 1, 2), Index, 1, true, ELinkKind::Execution, FVector2f(Size.X, 40)});
    return Index;
}
FRouteKey RouteLink(FLayoutGraph& Graph, int32 From, int32 To)
{
    const int32 A = Graph.Nodes[From].FirstPin + 1, B = Graph.Nodes[To].FirstPin;
    Graph.Edges.Add({A, B, ELinkKind::Execution});
    return {Graph.Nodes[From].Geometry.Id, Graph.Pins[A].Id, Graph.Nodes[To].Geometry.Id, Graph.Pins[B].Id};
}
bool Inside(FVector2f P, FVector2f Min, FVector2f Max)
{
    return P.X > Min.X && P.X < Max.X && P.Y > Min.Y && P.Y < Max.Y;
}
void CheckClear(FAutomationTestBase& Test, const FLayoutGraph& Graph, const FWireRoute& Route, bool bNativePinInsets = false)
{
    for (const auto& Curve : Route.Curves)
    {
        for (int32 Sample = 1; Sample < 100; ++Sample)
        {
            const auto P = FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, Sample / 100.f);
            for (const auto& Node : Graph.Nodes)
            {
                const FVector2f Position(Node.Geometry.Position);
                if (Node.bComment)
                {
                    if (Node.Geometry.CommentHeader.IsSet())
                    {
                        const auto& Header = Node.Geometry.CommentHeader.GetValue();
                        Test.TestFalse(TEXT("Rounded curve clears the comment header"), Inside(P, Position + Header.Min, Position + Header.Max));
                    }
                }
                else
                {
                    const bool bOwnStart = bNativePinInsets && Node.Geometry.Id == Route.Key.FromNode && &Curve == &Route.Curves[0] &&
                        FMath::IsNearlyEqual(P.Y, Curve.Start.Y, 0.001f) && P.X >= Curve.Start.X;
                    const bool bOwnEnd = bNativePinInsets && Node.Geometry.Id == Route.Key.ToNode && &Curve == &Route.Curves.Last() &&
                        FMath::IsNearlyEqual(P.Y, Curve.End.Y, 0.001f) && P.X <= Curve.End.X;
                    Test.TestFalse(TEXT("Rendered curve clears measured bodies outside its own pin insets"),
                        !bOwnStart && !bOwnEnd && Inside(P, Position, Position + Node.Geometry.BodySize));
                }
            }
        }
    }
    for (int32 I = 1; I < Route.Curves.Num(); ++I)
    {
        Test.TestEqual(TEXT("Rendered pieces meet exactly"), Route.Curves[I - 1].End, Route.Curves[I].Start);
    }
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrowingObstacleTest, "GlooPrint.Routing.GrowingTerminalIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGrowingObstacleTest::RunTest(const FString& Parameters)
{
    Routing::FObstacles Index;
    const int32 Id = Index.Add(FBox2f({0, 244}, {100, 268}), 7);
    for (const FBox2f Box : {FBox2f({-300, 244}, {600, 268}),
        FBox2f({-20000, 244}, {20000, 268}), FBox2f({-100000, 244}, {100000, 268})})
    {
        Index.GrowHorizontal(Id, Box);
        for (float X : {Box.Min.X + 1, 50.f, Box.Max.X - 1})
        {
            TestFalse(TEXT("New and old columns block crossing lines"), Index.ClearLine({X, 200}, {X, 300}));
            TestTrue(TEXT("Ignored node still permits crossing"), Index.ClearLine({X, 200}, {X, 300}, 7));
        }
        TestTrue(TEXT("Growth does not fill unrelated rows"), Index.ClearLine({Box.Min.X, 280}, {Box.Max.X, 280}));
        TestTrue(TEXT("Inflated boundaries remain touchable"), Index.ClearLine({Box.Min.X, 244}, {Box.Max.X, 244}));
        TestTrue(TEXT("A line beyond the expanded interval stays clear"),
            Index.ClearLine({Box.Max.X + 1, 200}, {Box.Max.X + 1, 300}));
        Index.GrowHorizontal(Id, Box);
        for (const FBox2f Query : {FBox2f({40, 250}, {60, 260}),
            FBox2f({40, -100000}, {60, 100000})})
        {
            int32 Visits = 0;
            Index.Query(Query, [&](int32 Found) { TestEqual(TEXT("Stable obstacle identity"), Found, Id); ++Visits; return true; });
            TestEqual(TEXT("Grid/axis/spill entries visit an expanded obstacle only once"), Visits, 1);
        }
    }
    TestEqual(TEXT("Expansion retains one obstacle"), Index.Items.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRouteCoordinateTest, "GlooPrint.Routing.ChannelCoordinateBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRouteCoordinateTest::RunTest(const FString& Parameters)
{
    FChannelCoordinates Coordinates;
    Coordinates.Values = {-33, -37, -35, -36, -34, -35}; Coordinates.SortUnique();
    Coordinates.Add(-36); TArray<float> Nearest;
    Coordinates.AppendNearest(Nearest, 16777184.f);
    TestTrue(TEXT("Equal rounded distances use ascending coordinates, including within the left band"),
        Nearest == TArray<float>{-33, -34, -37, -36, -35});
    Coordinates.Values = {16776448, 16776449, 16776450, 16776451, 16776452};
    Nearest.Reset(); Coordinates.AppendNearest(Nearest, 16777216.f, 768);
    TestTrue(TEXT("Shift-collapsed coordinates count only once toward the channel bound"),
        Nearest == TArray<float>{16777216, 16777218, 16777220});
    Coordinates.Values.Reset();
    for (int32 I = 0; I < 256; ++I) { Coordinates.Add(float(I)); }
    Nearest.Reset(); Coordinates.AppendNearest(Nearest, 0);
    TestEqual(TEXT("Only the nearest distinct source channels are returned"), Nearest.Num(), 128);
    TestEqual(TEXT("Channel cap retains the nearest boundary coordinate"), Nearest.Last(), 127.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingTest, "GlooPrint.Routing.ObstaclesCornersAndFeedback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    RouteNode(Graph, {0, 0}, {100, 80}); RouteNode(Graph, {700, 0}, {100, 80});
    const auto Forward = RouteLink(Graph, 0, 1);
    FRouteSet Routes; FString Reason;
    if (!TestTrue(TEXT("Direct routing succeeds"), ComputeRoutes(Graph, Routes, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Aligned execution stays one straight piece"), Routes.Wires[Forward].Curves.Num(), 1);
    TestEqual(TEXT("Simple routes do not invoke A*"), Routes.Wires[Forward].Search.ExpandedStates, 0);
    TestEqual(TEXT("Straight route starts on its output pin"), Routes.Wires[Forward].Points[0], FVector2f(100, 40));
    TestEqual(TEXT("Straight route ends on its input pin"), Routes.Wires[Forward].Points.Last(), FVector2f(700, 40));
    TestTrue(TEXT("Low-detail source can shrink within its clear terminal region"), Routes.Wires[Forward].StartRegion.Min.X <= 20);
    TestTrue(TEXT("Native text shaping can move an output within its existing straight terminal"), Routes.Wires[Forward].StartRegion.Max.X >= 112);
    TestTrue(TEXT("Compact native rows may move either endpoint upward by more than a fixed clearance band"),
        Routes.Wires[Forward].StartRegion.IsInsideOrOn(FVector2f(100, 26.5f)) && Routes.Wires[Forward].EndRegion.IsInsideOrOn(FVector2f(700, 26.5f)));
    FLayoutGraph UpperObstacle = Graph;
    const int32 UpperBody = RouteNode(UpperObstacle, {300, -20}, {100, 30});
    FRouteSet UpperRoutes;
    if (TestTrue(TEXT("Original route remains clear beneath a foreign body"), ComputeRoutes(UpperObstacle, UpperRoutes, Reason)))
    {
        const auto& Wire = UpperRoutes.Wires[Forward];
        FBox2f Hull = Wire.StartRegion; Hull += Wire.EndRegion;
        const FVector2f Position(UpperObstacle.Nodes[UpperBody].Geometry.Position);
        TestFalse(TEXT("Upward pin adaptation still excludes a foreign inflated body"),
            Hull.Intersect(FBox2f(Position - FVector2f(12), Position + UpperObstacle.Nodes[UpperBody].Geometry.BodySize + FVector2f(12))));
        TestEqual(TEXT("Blocked adaptation retains the original custom route"), UpperRoutes.FallbackCount, 0);
    }
    FLayoutGraph Overlapping = Graph;
    RouteNode(Overlapping, {20, 20}, {68, 40});
    FRouteSet Constrained;
    TestTrue(TEXT("Overlapping body retains a clear original wire"), ComputeRoutes(Overlapping, Constrained, Reason));
    TestEqual(TEXT("Original pin still has a custom route"), Constrained.FallbackCount, 0);
    TestTrue(TEXT("Terminal adaptation cannot extend through another body"), Constrained.Wires[Forward].StartRegion.Min.X >= 100);
    FLayoutGraph Room = Graph;
    const int32 Foreign = RouteNode(Room, {20, 20}, {40, 40});
    FRouteSet Bounded;
    TestTrue(TEXT("Smaller terminal regions can clear a distant obstacle"), ComputeRoutes(Room, Bounded, Reason));
    TestEqual(TEXT("Bounded terminal adaptation retains the custom connection"), Bounded.FallbackCount, 0);
    const auto& BoundedWire = Bounded.Wires[Forward];
    TestTrue(TEXT("A clear sub-unit source shift survives broad-region rejection"), BoundedWire.StartRegion.IsInsideOrOn(FVector2f(99.5f, 40.05f)));
    TestTrue(TEXT("A single curve validates both moving endpoints together"), BoundedWire.EndRegion.IsInsideOrOn(FVector2f(700.5f, 40.05f)));
    FBox2f TerminalHull = BoundedWire.StartRegion; TerminalHull += BoundedWire.EndRegion;
    const FVector2f ForeignPosition(Room.Nodes[Foreign].Geometry.Position);
    TestFalse(TEXT("Every combination of adjusted endpoints clears the inflated foreign body"),
        TerminalHull.Intersect(FBox2f(ForeignPosition - FVector2f(12), ForeignPosition + Room.Nodes[Foreign].Geometry.BodySize + FVector2f(12))));
    FRouteCurve Terminal;
    Terminal.Start = {20, 38}; Terminal.End = {124, 40};
    Terminal.StartTangent = Terminal.EndTangent = {104, 0}; MeasureRouteCurve(Terminal);
    TestEqual(TEXT("Adapted pulse starts on the native pin"), EvaluateRouteCurve(Terminal, 0), Terminal.Start);
    TestEqual(TEXT("Adapted pulse reaches the unchanged corridor"), EvaluateRouteCurve(Terminal, Terminal.Length), Terminal.End);
    FVector2f Direction; EvaluateRouteCurve(Terminal, 0, &Direction);
    TestEqual(TEXT("Adapted terminal leaves the native pin horizontally"), Direction, FVector2f(1, 0));

    RouteNode(Graph, {300, -100}, {200, 280});
    const auto Back = RouteLink(Graph, 1, 0);
    const int32 Comment = RouteNode(Graph, {-80, -180}, {1000, 800});
    Graph.Nodes[Comment].bComment = true;
    Graph.Nodes[Comment].Geometry.CommentHeader = FMeasuredRect{{0, 0}, {1000, 40}};
    if (!TestTrue(TEXT("Obstacle, comment and feedback routes compute"), ComputeRoutes(Graph, Routes, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Both directions find a channel"), Routes.FallbackCount, 0);
    TestTrue(TEXT("Blocked straight wire receives bends"), Routes.Wires[Forward].Points.Num() > 2);
    TestTrue(TEXT("Feedback uses a return lane"), Routes.Wires[Back].Points.Num() >= 4);
    bool bCurved = false;
    for (const auto& Pair : Routes.Wires)
    {
        CheckClear(*this, Graph, Pair.Value);
        for (const auto& Curve : Pair.Value.Curves) { bCurved |= Curve.StartTangent.X != Curve.EndTangent.X || Curve.StartTangent.Y != Curve.EndTangent.Y; }
        TestEqual(TEXT("Pulse distance zero is the source"), EvaluateRoute(Pair.Value, 0), Pair.Value.Points[0]);
        TestTrue(TEXT("Pulse distance ends at the destination"), EvaluateRoute(Pair.Value, Pair.Value.Length).Equals(Pair.Value.Points.Last(), 0.001f));
    }
    TestTrue(TEXT("Corners have actual curved tangents"), bCurved);
    const auto& ForwardPoints = Routes.Wires[Forward].Points;
    const auto& BackPoints = Routes.Wires[Back].Points;
    for (int32 A = 1; A < ForwardPoints.Num(); ++A)
    {
        for (int32 B = 1; B < BackPoints.Num(); ++B)
        {
            const FVector2f A0 = ForwardPoints[A - 1], A1 = ForwardPoints[A], B0 = BackPoints[B - 1], B1 = BackPoints[B];
            if (A0.Y == A1.Y && B0.Y == B1.Y && FMath::Min(A0.X, A1.X) < FMath::Max(B0.X, B1.X) &&
                FMath::Max(A0.X, A1.X) > FMath::Min(B0.X, B1.X))
            {
                TestTrue(TEXT("Unrelated horizontal wire lanes stay separated"), FMath::Abs(A0.Y - B0.Y) >= 12);
            }
            if (A0.X == A1.X && B0.X == B1.X && FMath::Min(A0.Y, A1.Y) < FMath::Max(B0.Y, B1.Y) &&
                FMath::Max(A0.Y, A1.Y) > FMath::Min(B0.Y, B1.Y))
            {
                TestTrue(TEXT("Unrelated vertical wire lanes stay separated"), FMath::Abs(A0.X - B0.X) >= 12);
            }
        }
    }
    FRouteSet Cold;
    Swap(Graph.Edges[0], Graph.Edges[1]);
    TestTrue(TEXT("Cold routing with shuffled edge enumeration"), ComputeRoutes(Graph, Cold, Reason));
    for (const auto& Pair : Routes.Wires)
    {
        TestTrue(TEXT("Cold routes preserve deterministic channel choices"), Cold.Wires[Pair.Key].Points == Pair.Value.Points);
        TestEqual(TEXT("Cold routes preserve rounded length"), Cold.Wires[Pair.Key].Length, Pair.Value.Length);
    }
    RouteNode(Graph, {90, 20}, {100, 40});
    TestTrue(TEXT("Unroutable edge is a valid native fallback"), ComputeRoutes(Graph, Cold, Reason));
    TestTrue(TEXT("Blocked exit has an explicit reason"), Cold.Wires[Forward].Fallback == ERouteFallback::BlockedEndpoint);
    TestTrue(TEXT("Blocked exit has no fabricated custom curve"), Cold.Wires[Forward].Curves.IsEmpty());
    TestEqual(TEXT("Fallback retains every connection"), Cold.Wires.Num(), Graph.Edges.Num());
    Graph.Pins[0].Offset.Reset();
    TestFalse(TEXT("Missing linked geometry rejects the snapshot"), ComputeRoutes(Graph, Cold, Reason));
    TestTrue(TEXT("Failure returns no partial routes"), Cold.Wires.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingSharedTerminalTest, "GlooPrint.Routing.SharedTerminalCorridors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingSharedTerminalTest::RunTest(const FString& Parameters)
{
    for (bool bFanIn : {false, true})
    {
        FLayoutGraph Graph;
        for (FIntPoint Position : {FIntPoint(0, -200), FIntPoint(172, 200),
            FIntPoint(0, 0), FIntPoint(400, 0), FIntPoint(400, -200)})
        {
            if (bFanIn) { Position = FIntPoint(500 - Position.X, -80 - Position.Y); }
            RouteNode(Graph, Position, {100, 80});
        }
        auto Link = [&](int32 From, int32 To, ELinkKind Kind)
        {
            const auto Key = bFanIn ? RouteLink(Graph, To, From) : RouteLink(Graph, From, To);
            auto& Edge = Graph.Edges.Last(); Edge.Kind = Kind;
            Graph.Pins[Edge.From].Kind = Kind; Graph.Pins[Edge.To].Kind = Kind;
            return Key;
        };
        const auto Execution = Link(0, 1, ELinkKind::Execution);
        const auto Straight = Link(2, 3, ELinkKind::Data);
        const auto Branch = Link(2, 4, ELinkKind::Data);
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Shared terminal fixture computes"), ComputeRoutes(Graph, Routes, Reason))) { AddError(Reason); return false; }
        TestEqual(bFanIn ? TEXT("Shared input escapes the execution lane") : TEXT("Shared output escapes the execution lane"), Routes.FallbackCount, 0);
        TestEqual(TEXT("Every connection retains its own route key"), Routes.Wires.Num(), 3);
        TestEqual(TEXT("Existing straight data wire stays straight"), Routes.Wires[Straight].Points.Num(), 2);
        const auto& Points = Routes.Wires[Branch].Points;
        if (TestTrue(TEXT("Branch has a complete custom route"), Points.Num() >= 4))
        {
            const float SharedLength = bFanIn ? Points.Last().X - Points[Points.Num() - 2].X : Points[1].X - Points[0].X;
            TestTrue(TEXT("Shared terminal can extend past its nominal 36-unit stub"), SharedLength > 36);
        }
        for (const auto& Pair : Routes.Wires) { CheckClear(*this, Graph, Pair.Value); }
        const auto& Flow = Routes.Wires[Execution].Points;
        for (int32 A = 1; A < Points.Num(); ++A)
        {
            for (int32 B = 1; B < Flow.Num(); ++B)
            {
                if (Points[A].X == Points[A - 1].X && Flow[B].X == Flow[B - 1].X &&
                    FMath::Min(Points[A].Y, Points[A - 1].Y) < FMath::Max(Flow[B].Y, Flow[B - 1].Y) &&
                    FMath::Max(Points[A].Y, Points[A - 1].Y) > FMath::Min(Flow[B].Y, Flow[B - 1].Y))
                {
                    TestTrue(TEXT("Shared data does not occupy the unrelated execution lane"), FMath::Abs(Points[A].X - Flow[B].X) >= 12);
                }
            }
        }
        Swap(Graph.Edges[0], Graph.Edges[2]);
        FRouteSet Cold;
        TestTrue(TEXT("Cold shared-terminal routing computes after reordering"), ComputeRoutes(Graph, Cold, Reason));
        for (const auto& Pair : Routes.Wires)
        {
            TestTrue(TEXT("Reordering retains the same shared-terminal paths"), Cold.Wires[Pair.Key].Points == Pair.Value.Points);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingFractionalSiblingTest, "GlooPrint.Routing.FractionalSharedPinTurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingFractionalSiblingTest::RunTest(const FString& Parameters)
{
    for (const auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FLayoutGraph Graph;
        RouteNode(Graph, {-426, 440}, {42, 24});
        RouteNode(Graph, {1030, 412}, {42, 24});
        RouteNode(Graph, {-272, 391}, {215.5f, 76});
        Graph.Pins[1].Offset = FVector2f(32.5f, 9.5f);
        Graph.Pins[2].Offset = FVector2f(9.5f, 9.5f);
        Graph.Pins[4].Offset = FVector2f(10, 58);
        const auto Far = RouteLink(Graph, 0, 1), Near = RouteLink(Graph, 0, 2);
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Fractional shared-pin branches compute"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); continue; }
        TestEqual(TEXT("The distant branch leaves a custom route to the nearby consumer"), Routes.FallbackCount, 0);
        TestEqual(TEXT("Both original pin pairs remain"), Routes.Wires.Num(), 2);
        TestTrue(TEXT("Both routes retain their original node and pin identities"), Routes.Wires.Contains(Far) && Routes.Wires.Contains(Near));
        for (const auto& Pair : Routes.Wires) { CheckClear(*this, Graph, Pair.Value, true); }
        const auto& Wire = Routes.Wires[Near];
        if (TestFalse(TEXT("Nearby consumer has an actual custom curve"), Wire.Curves.IsEmpty()))
        {
            TestEqual(TEXT("Shared source keeps its fractional attachment"), Wire.Curves[0].Start, FVector2f(-393.5f, 449.5f));
            TestEqual(TEXT("Nearby consumer keeps its distinct original attachment"), Wire.Curves.Last().End, FVector2f(-262, 449));
        }
        Swap(Graph.Edges[0], Graph.Edges[1]); FRouteSet Shuffled;
        if (TestTrue(TEXT("Reordered fractional branches compute cold"), ComputeRoutes(Graph, Shuffled, Reason, Style)))
        {
            for (const auto& Pair : Routes.Wires)
            {
                const auto* Cold = Shuffled.Wires.Find(Pair.Key);
                TestTrue(TEXT("Input enumeration preserves complete branch paths"), Cold && Cold->Points == Pair.Value.Points);
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingPinApproachTest, "GlooPrint.Routing.ReservedPinApproaches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingPinApproachTest::RunTest(const FString& Parameters)
{
    for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    for (bool bReturnFirst : {false, true})
    {
        FLayoutGraph Graph;
        const int32 Branch = RouteNode(Graph, {0, 0}, {200, 96});
        Graph.Pins[1].Offset = FVector2f(200, 43);
        const int32 Else = Graph.Pins.Add({FGuid(0, 0, 1, 3), Branch, 2, true, ELinkKind::Execution, FVector2f(200, 75)});
        ++Graph.Nodes[Branch].PinCount;
        const int32 Sequence = RouteNode(Graph, {700, 280}, {140, 124});
        const int32 Print = RouteNode(Graph, {320, 30}, {290, 356});
        for (int32 Pin : {Graph.Nodes[Sequence].FirstPin, Graph.Nodes[Sequence].FirstPin + 1, Graph.Nodes[Print].FirstPin})
        {
            auto Offset = Graph.Pins[Pin].Offset.GetValue(); Offset.Y = 43; Graph.Pins[Pin].Offset = Offset;
        }
        if (bReturnFirst) { Swap(Graph.Nodes[Branch].Geometry.Id, Graph.Nodes[Sequence].Geometry.Id); }
        RouteLink(Graph, Branch, Sequence);
        Graph.Edges.Add({Else, Graph.Nodes[Sequence].FirstPin, ELinkKind::Execution});
        const auto Return = RouteLink(Graph, Sequence, Print);
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Nearby pin approaches compute"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); return false; }
        TestEqual(TEXT("Route order cannot strand the nearby return input"), Routes.FallbackCount, 0);
        TestEqual(TEXT("Every original connection has its own route"), Routes.Wires.Num(), 3);
        for (const auto& Pair : Routes.Wires) { CheckClear(*this, Graph, Pair.Value); }
        const auto& Back = Routes.Wires[Return].Points;
        for (const auto& Pair : Routes.Wires)
        {
            if (Pair.Key == Return) { continue; }
            const auto& Forward = Pair.Value.Points;
            for (int32 A = 1; A < Back.Num(); ++A)
            for (int32 B = 1; B < Forward.Num(); ++B)
            {
                if (Back[A].Y == Back[A - 1].Y && Forward[B].Y == Forward[B - 1].Y &&
                    FMath::Min(Back[A].X, Back[A - 1].X) < FMath::Max(Forward[B].X, Forward[B - 1].X) &&
                    FMath::Max(Back[A].X, Back[A - 1].X) > FMath::Min(Forward[B].X, Forward[B - 1].X))
                {
                    TestTrue(TEXT("Distinct parallel wires retain lane spacing"), FMath::Abs(Back[A].Y - Forward[B].Y) >= 12);
                }
            }
        }
        Swap(Graph.Edges[0], Graph.Edges[2]);
        FRouteSet Cold;
        TestTrue(TEXT("Cold shuffled pin approaches compute"), ComputeRoutes(Graph, Cold, Reason, Style));
        for (const auto& Pair : Routes.Wires) { TestTrue(TEXT("Shuffling preserves each route"), Cold.Wires[Pair.Key].Points == Pair.Value.Points); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingShiftedTurnsTest, "GlooPrint.Routing.ShiftedTerminalTurns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingShiftedTurnsTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    const int32 Source = RouteNode(Graph, {0, 0}, {100, 240});
    const int32 SecondOut = Graph.Pins.Add({FGuid(0, 0, 1, 3), Source, 2, true, ELinkKind::Execution, FVector2f(100, 64)});
    ++Graph.Nodes[Source].PinCount;
    const int32 Target = RouteNode(Graph, {800, 0}, {100, 240});
    const int32 SecondIn = Graph.Pins.Add({FGuid(0, 0, 2, 3), Target, 2, false, ELinkKind::Execution, FVector2f(0, 64)});
    ++Graph.Nodes[Target].PinCount;
    RouteLink(Graph, Source, Target);
    Graph.Edges.Add({SecondOut, SecondIn, ELinkKind::Execution});
    const FRouteKey Key{Graph.Nodes[Source].Geometry.Id, Graph.Pins[SecondOut].Id, Graph.Nodes[Target].Geometry.Id, Graph.Pins[SecondIn].Id};
    RouteNode(Graph, {350, -60}, {100, 360});
    RouteNode(Graph, {-200, 260}, {1400, 100});
    for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Shifted terminal fixture computes"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); return false; }
        TestEqual(TEXT("Both original connections retain custom paths"), Routes.FallbackCount, 0);
        TestEqual(TEXT("Both original pin pairs remain"), Routes.Wires.Num(), 2);
        const auto& Route = Routes.Wires[Key];
        TestTrue(TEXT("Clear four-bend detour avoids grid search"), Route.Method == ERouteMethod::Simple && Route.Search.ExpandedStates == 0);
        TestEqual(TEXT("The detour needs only four bends"), Route.Points.Num(), 6);
        for (const auto& Pair : Routes.Wires) { CheckClear(*this, Graph, Pair.Value); }
        Swap(Graph.Edges[0], Graph.Edges[1]);
        FRouteSet Cold;
        TestTrue(TEXT("Cold shuffled detours compute"), ComputeRoutes(Graph, Cold, Reason, Style));
        for (const auto& Pair : Routes.Wires) { TestTrue(TEXT("Shuffled detours keep identical paths"), Cold.Wires[Pair.Key].Points == Pair.Value.Points); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingLocalEscapeTest, "GlooPrint.Routing.CrowdedPinEscape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingLocalEscapeTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 24; ++I)
    {
        RouteNode(Graph, {I * 272, 0}, {160, 840});
        for (int32 P = 2; P < 8; ++P)
        {
            Graph.Pins.Add({FGuid(0, 0, I + 1, P + 1), I, P, P % 2 != 0, ELinkKind::Data,
                FVector2f(P % 2 ? 160 : 0, 40 + (P / 2) * 24)});
            ++Graph.Nodes[I].PinCount;
        }
    }
    for (int32 I = 0; I < Graph.Nodes.Num(); ++I)
    {
        if (I + 1 < Graph.Nodes.Num()) { RouteLink(Graph, I, I + 1); }
        for (int32 P = 0; P < 3; ++P)
        {
            const int32 Target = I + 1 + P * 3;
            if (Target < Graph.Nodes.Num())
            {
                Graph.Edges.Add({Graph.Nodes[I].FirstPin + 3 + P * 2, Graph.Nodes[Target].FirstPin + 2 + P * 2, ELinkKind::Data});
            }
        }
    }
    for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Crowded pin escapes compute"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); return false; }
        TestEqual(TEXT("All crowded connections retain custom routes"), Routes.FallbackCount, 0);
        TestEqual(TEXT("No original pin pair is lost"), Routes.Wires.Num(), Graph.Edges.Num());
        for (const FIntVector Example : {FIntVector(11, 15, 5), FIntVector(15, 22, 7)})
        {
            const auto& From = Graph.Nodes[Example.X]; const auto& To = Graph.Nodes[Example.Y];
            const auto& Output = Graph.Pins[From.FirstPin + Example.Z];
            const auto& Input = Graph.Pins[To.FirstPin + Example.Z - 1];
            const auto& Route = Routes.Wires.FindChecked({From.Geometry.Id, Output.Id, To.Geometry.Id, Input.Id});
            if (!TestTrue(TEXT("The escaped connection has a complete path"), Route.Points.Num() >= 2)) { return false; }
            TestTrue(TEXT("A local escape avoids a full grid search"), Route.Method == ERouteMethod::Simple && Route.Search.ExpandedStates == 0);
            TestTrue(FString::Printf(TEXT("Local step %d to %d avoids a detour below the tall nodes (length %.1f)"), Example.X, Example.Y, Route.Length),
                Route.Length < (Example.X == 11 ? 1400.f : 2500.f));
            TestEqual(TEXT("Escape remains attached to the original output"), Route.Points[0], FVector2f(From.Geometry.Position) + Output.Offset.GetValue());
            TestEqual(TEXT("Escape remains attached to the original input"), Route.Points.Last(), FVector2f(To.Geometry.Position) + Input.Offset.GetValue());
            CheckClear(*this, Graph, Route);
        }
        Swap(Graph.Edges[0], Graph.Edges.Last());
        FRouteSet Cold;
        TestTrue(TEXT("Cold shuffled escapes compute"), ComputeRoutes(Graph, Cold, Reason, Style));
        for (const auto& Pair : Routes.Wires) { TestTrue(TEXT("Local escapes retain deterministic paths"), Cold.Wires[Pair.Key].Points == Pair.Value.Points); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingDenseFanTest, "GlooPrint.Routing.DenseFanCorridors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingDenseFanTest::RunTest(const FString& Parameters)
{
    for (bool bFanIn : {false, true})
    {
        FLayoutGraph Graph;
        RouteNode(Graph, {0, 0}, {100, 80});
        TArray<FRouteKey> Keys;
        for (int32 I = 0; I < 12; ++I)
        {
            const int32 Row = I % 2 ? (I + 1) / 2 : -I / 2;
            RouteNode(Graph, {196, Row * 128}, {100, 80});
            Keys.Add(bFanIn ? RouteLink(Graph, I + 1, 0) : RouteLink(Graph, 0, I + 1));
            auto& Edge = Graph.Edges.Last(); Edge.Kind = ELinkKind::Data;
            Graph.Pins[Edge.From].Kind = ELinkKind::Data; Graph.Pins[Edge.To].Kind = ELinkKind::Data;
        }
        if (bFanIn)
        {
            for (auto& Node : Graph.Nodes)
            {
                Node.Geometry.Position = FIntPoint(400 - Node.Geometry.Position.X, -80 - Node.Geometry.Position.Y);
            }
        }
        FRouteSet Routes; FString Reason;
        if (!TestTrue(TEXT("Dense fan computes"), ComputeRoutes(Graph, Routes, Reason))) { AddError(Reason); return false; }
        TestEqual(bFanIn ? TEXT("Twelve incoming wires fit the open corridor") : TEXT("Twelve outgoing wires fit the open corridor"), Routes.FallbackCount, 0);
        TestEqual(TEXT("Dense routing keeps all twelve connections"), Routes.Wires.Num(), 12);
        for (const auto& Pair : Routes.Wires)
        {
            CheckClear(*this, Graph, Pair.Value);
            if (Pair.Value.Fallback != ERouteFallback::None)
            {
                AddInfo(FString::Printf(TEXT("Dense %s fallback %s -> %s: %d"), bFanIn ? TEXT("fan-in") : TEXT("fan-out"),
                    *Pair.Key.FromNode.ToString(), *Pair.Key.ToNode.ToString(), int32(Pair.Value.Fallback)));
            }
        }
        for (int32 I = 0; I < Keys.Num(); ++I)
        {
            const auto& Points = Routes.Wires[Keys[I]].Points;
            const auto& Edge = Graph.Edges[I];
            if (TestTrue(TEXT("Each dense wire has a complete path"), Points.Num() >= 2))
            {
                for (bool bStart : {true, false})
                {
                    const auto& Pin = Graph.Pins[bStart ? Edge.From : Edge.To];
                    TestEqual(TEXT("Dense wire remains on its original pin"), bStart ? Points[0] : Points.Last(),
                        FVector2f(Graph.Nodes[Pin.Node].Geometry.Position) + Pin.Offset.GetValue());
                }
            }
            for (int32 J = I + 1; J < Keys.Num(); ++J)
            {
                const auto& Other = Routes.Wires[Keys[J]].Points;
                for (int32 A = 1; A < Points.Num(); ++A)
                {
                    for (int32 B = 1; B < Other.Num(); ++B)
                    {
                        if (Points[A].X == Points[A - 1].X && Other[B].X == Other[B - 1].X &&
                            FMath::Min(Points[A].Y, Points[A - 1].Y) < FMath::Max(Other[B].Y, Other[B - 1].Y) &&
                            FMath::Max(Points[A].Y, Points[A - 1].Y) > FMath::Min(Other[B].Y, Other[B - 1].Y))
                        {
                            TestTrue(TEXT("Dense fan bends occupy separated vertical lanes"), FMath::Abs(Points[A].X - Other[B].X) >= 12);
                        }
                    }
                }
            }
        }
        FRouteSet Diagonal;
        TestTrue(TEXT("Dense fan supports diagonal styling"), ComputeRoutes(Graph, Diagonal, Reason, EGlooPrintWireStyle::Diagonal45));
        for (const auto& Pair : Routes.Wires)
        {
            TestTrue(TEXT("Diagonal fan follows the same clear lanes"), Diagonal.Wires[Pair.Key].Points == Pair.Value.Points);
            CheckClear(*this, Graph, Diagonal.Wires[Pair.Key]);
        }
        for (int32 I = 0; I < Graph.Edges.Num() / 2; ++I) { Swap(Graph.Edges[I], Graph.Edges[Graph.Edges.Num() - 1 - I]); }
        FRouteSet Cold;
        TestTrue(TEXT("Cold dense routing computes"), ComputeRoutes(Graph, Cold, Reason));
        for (const auto& Pair : Routes.Wires)
        {
            TestTrue(TEXT("Dense paths retain stable enumeration-independent lanes"), Cold.Wires[Pair.Key].Points == Pair.Value.Points);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLargeFanRoutingTest, "GlooPrint.Routing.LargeFanFrontiers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLargeFanRoutingTest::RunTest(const FString& Parameters)
{
    constexpr int32 FanCount = 300;
    constexpr int32 ColumnX = 100 + 48 + FanCount / 2 * 12;
    for (bool bFanIn : {false, true})
    {
        FLayoutGraph Graph; RouteNode(Graph, {0, 0}, {100, 80});
        for (int32 I = 0; I < FanCount; ++I)
        {
            const int32 Row = I % 2 ? (I + 1) / 2 : -I / 2;
            RouteNode(Graph, {ColumnX, Row * 128}, {100, 80});
            RouteLink(Graph, bFanIn ? I + 1 : 0, bFanIn ? 0 : I + 1);
            if (!bFanIn)
            {
                auto& Edge = Graph.Edges.Last(); Edge.Kind = ELinkKind::Data;
                Graph.Pins[Edge.From].Kind = ELinkKind::Data; Graph.Pins[Edge.To].Kind = ELinkKind::Data;
            }
        }
        if (bFanIn)
        {
            for (auto& Node : Graph.Nodes) { Node.Geometry.Position = {ColumnX - Node.Geometry.Position.X, -Node.Geometry.Position.Y}; }
        }
        FRouteSet Reference;
        for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes; FString Reason;
            if (!TestTrue(TEXT("Large fan computes"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); return false; }
            TestEqual(TEXT("Every large fan connection remains available"), Routes.Wires.Num(), FanCount);
            TestEqual(TEXT("Free fan corridors avoid native fallback"), Routes.FallbackCount, 0);
            TArray<float> Upper, Lower;
            for (const auto& Edge : Graph.Edges)
            {
                const auto& From = Graph.Pins[Edge.From]; const auto& To = Graph.Pins[Edge.To];
                const FRouteKey Key{Graph.Nodes[From.Node].Geometry.Id, From.Id, Graph.Nodes[To.Node].Geometry.Id, To.Id};
                const auto& Route = Routes.Wires[Key];
                if ((Route.Points.Num() != 2 && Route.Points.Num() != 4) || Route.Search.ExpandedStates != 0)
                {
                    FString Path;
                    for (const auto& Point : Route.Points) { Path += FString::Printf(TEXT(" (%.1f,%.1f)"), Point.X, Point.Y); }
                    AddInfo(FString::Printf(TEXT("%s %d -> %d, style %d, expanded %d:%s"),
                        bFanIn ? TEXT("Fan-in") : TEXT("Fan-out"), From.Node, To.Node, int32(Style), Route.Search.ExpandedStates, *Path));
                }
                if (!TestTrue(TEXT("An open fan uses a straight wire or two bends"), Route.Points.Num() == 2 || Route.Points.Num() == 4)) { continue; }
                TestEqual(TEXT("Large fan retains its original source attachment"), Route.Points[0], FVector2f(Graph.Nodes[From.Node].Geometry.Position) + From.Offset.GetValue());
                TestEqual(TEXT("Large fan retains its original target attachment"), Route.Points.Last(), FVector2f(Graph.Nodes[To.Node].Geometry.Position) + To.Offset.GetValue());
                TestEqual(TEXT("Open fan lanes need no obstacle-search expansions"), Route.Search.ExpandedStates, 0);
                if (Route.Points.Num() == 4)
                {
                    const float X = Route.Points[1].X;
                    TestTrue(TEXT("Vertical bundle clears both measured node columns"), X >= 124 && X <= ColumnX - 24);
                    const float OtherY = bFanIn ? Route.Points[0].Y : Route.Points.Last().Y;
                    (OtherY < 40 ? Upper : Lower).Add(X);
                }
                if (!Reference.Wires.IsEmpty()) { TestTrue(TEXT("Style and shuffled enumeration preserve the complete fan path"), Reference.Wires[Key].Points == Route.Points); }
            }
            for (auto* Lanes : {&Upper, &Lower})
            {
                Lanes->Sort();
                for (int32 I = 1; I < Lanes->Num(); ++I) { TestTrue(TEXT("Overlapping fan lanes stay separately traceable"), (*Lanes)[I] - (*Lanes)[I - 1] >= 12); }
            }
            FLayoutGraph Shifted = Graph;
            const FIntPoint Shift(-257, -255);
            for (auto& Node : Shifted.Nodes) { Node.Geometry.Position += Shift; }
            FRouteSet ShiftedRoutes;
            if (TestTrue(TEXT("Translated fan computes across spatial bucket seams"), ComputeRoutes(Shifted, ShiftedRoutes, Reason, Style)))
            {
                TestEqual(TEXT("Translated fan retains every connection"), ShiftedRoutes.Wires.Num(), Routes.Wires.Num());
                TestEqual(TEXT("Bucket seams introduce no native fallbacks"), ShiftedRoutes.FallbackCount, 0);
                for (const auto& Pair : Routes.Wires)
                {
                    TArray<FVector2f> Expected = Pair.Value.Points;
                    for (auto& Point : Expected) { Point += FVector2f(Shift); }
                    const auto* Wire = ShiftedRoutes.Wires.Find(Pair.Key);
                    TestTrue(TEXT("Translation preserves exact fan paths across bucket seams"), Wire && Wire->Points == Expected);
                }
            }
            if (Reference.Wires.IsEmpty())
            {
                Reference = MoveTemp(Routes);
                for (int32 I = 0; I < Graph.Edges.Num() / 2; ++I) { Swap(Graph.Edges[I], Graph.Edges[Graph.Edges.Num() - 1 - I]); }
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingContinuationTest, "GlooPrint.Routing.ContinuationOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingContinuationTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph; RouteNode(Graph, {0, 0}, {100, 80});
    for (int32 I = 0; I < 40; ++I)
    {
        RouteNode(Graph, {2000, (I - 20) * 128}, {100, 80}); RouteLink(Graph, 0, I + 1);
    }
    RouteNode(Graph, {950, -100}, {100, 300});
    for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        FRouteSet Expected; FString Reason;
        if (!TestTrue(TEXT("Reference routing completes"), ComputeRoutes(Graph, Expected, Reason, Style))) { AddError(Reason); return false; }
        FLayoutGraph Input = Graph;
        FRoutingJob Job(Input, Style);
        Input.Nodes.Reset(); Input.Pins.Reset(); Input.Edges.Reset();
        FRouteSet Actual;
        TestFalse(TEXT("An unfinished job cannot expose partial routes"), Job.TakeResult(Actual, Reason));
        TestTrue(TEXT("An unavailable result is empty"), Actual.Wires.IsEmpty());
        int32 Steps = 0, PreviousLinks = 0;
        while (!Job.Advance(0) && ++Steps < 1000)
        {
            const int32 Links = Job.GetCompletedLinks();
            TestTrue(TEXT("Expired budget advances at most one whole link"), Links >= PreviousLinks && Links <= PreviousLinks + 1);
            PreviousLinks = Links;
            TestFalse(TEXT("Intermediate results stay private"), Job.TakeResult(Actual, Reason));
            TestTrue(TEXT("No intermediate connections are published"), Actual.Wires.IsEmpty());
        }
        TestTrue(TEXT("Continuation terminates within bounded work units"), Steps < 1000);
        TestTrue(TEXT("The owned snapshot survives destruction of the caller's input"), Job.TakeResult(Actual, Reason));
        TestEqual(TEXT("Continuation retains every route"), Actual.Wires.Num(), Expected.Wires.Num());
        TestEqual(TEXT("Continuation retains fallback decisions"), Actual.FallbackCount, Expected.FallbackCount);
        for (const auto& Pair : Expected.Wires)
        {
            const auto* Wire = Actual.Wires.Find(Pair.Key);
            if (!TestTrue(TEXT("Continuation retains original pin pairs"), Wire != nullptr)) { continue; }
            TestTrue(TEXT("Yield boundaries preserve exact route points"), Wire->Points == Pair.Value.Points);
            TestEqual(TEXT("Yield boundaries preserve route method"), int32(Wire->Method), int32(Pair.Value.Method));
            TestEqual(TEXT("Yield boundaries preserve search work"), Wire->Search.ExpandedStates, Pair.Value.Search.ExpandedStates);
            TestEqual(TEXT("Yield boundaries preserve curve length"), Wire->Length, Pair.Value.Length);
            CheckClear(*this, Graph, *Wire);
        }
        TestFalse(TEXT("A completed result can only be taken once"), Job.TakeResult(Actual, Reason));
    }
    FLayoutGraph Invalid = Graph; Invalid.Edges[0].To = Invalid.Pins.Num();
    FRoutingJob Failed(MoveTemp(Invalid));
    Failed.Advance(TNumericLimits<double>::Max());
    FRouteSet Result; FString Reason;
    TestFalse(TEXT("Invalid staged input fails before publication"), Failed.TakeResult(Result, Reason));
    TestTrue(TEXT("Invalid staged input exposes no partial routes"), Result.Wires.IsEmpty() && !Reason.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLongRouteQueryTest, "GlooPrint.Routing.LongQueriesAndSpills",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLongRouteQueryTest::RunTest(const FString& Parameters)
{
    for (bool bVertical : {false, true})
    {
        FLayoutGraph Graph;
        TArray<FRouteKey> Keys;
        for (int32 I = 0; I < 2; ++I)
        {
            const int32 From = RouteNode(Graph, {0, (bVertical ? -100000 : 0) + I * 128}, {100, 80});
            const int32 To = RouteNode(Graph, {bVertical ? 2000 : 200000, (bVertical ? 100000 : 0) + I * 128}, {100, 80});
            Keys.Add(RouteLink(Graph, From, To));
        }
        RouteNode(Graph, bVertical ? FIntPoint(900, -40000) : FIntPoint(60000, -40000),
            bVertical ? FVector2f(200, 80000) : FVector2f(80000, 80000));
        FRouteSet Reference;
        for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
        {
            FRouteSet Routes; FString Reason;
            if (!TestTrue(TEXT("Long queries around oversized obstacles compute"), ComputeRoutes(Graph, Routes, Reason, Style))) { AddError(Reason); return false; }
            TestEqual(TEXT("Both long connections remain available"), Routes.Wires.Num(), 2);
            TestEqual(TEXT("Oversized obstacles leave clear custom routes"), Routes.FallbackCount, 0);
            for (const auto& Key : Keys)
            {
                const auto* Wire = Routes.Wires.Find(Key);
                if (!TestTrue(TEXT("Long wire has its complete route"), Wire && Wire->Points.Num() >= 4)) { continue; }
                CheckClear(*this, Graph, *Wire);
                if (!Reference.Wires.IsEmpty()) { TestTrue(TEXT("Both styles retain identical long corridors"), Reference.Wires[Key].Points == Wire->Points); }
            }
            const auto& A = Routes.Wires[Keys[0]].Points;
            const auto& B = Routes.Wires[Keys[1]].Points;
            for (int32 I = 1; I < A.Num(); ++I)
            {
                for (int32 J = 1; J < B.Num(); ++J)
                {
                    if (A[I].X == A[I - 1].X && B[J].X == B[J - 1].X &&
                        FMath::Min(A[I].Y, A[I - 1].Y) < FMath::Max(B[J].Y, B[J - 1].Y) &&
                        FMath::Max(A[I].Y, A[I - 1].Y) > FMath::Min(B[J].Y, B[J - 1].Y))
                    {
                        TestTrue(TEXT("Long vertical reservations retain lane clearance"), FMath::Abs(A[I].X - B[J].X) >= 12);
                    }
                    if (A[I].Y == A[I - 1].Y && B[J].Y == B[J - 1].Y &&
                        FMath::Min(A[I].X, A[I - 1].X) < FMath::Max(B[J].X, B[J - 1].X) &&
                        FMath::Max(A[I].X, A[I - 1].X) > FMath::Min(B[J].X, B[J - 1].X))
                    {
                        TestTrue(TEXT("Long horizontal reservations retain lane clearance"), FMath::Abs(A[I].Y - B[J].Y) >= 12);
                    }
                }
            }
            if (Reference.Wires.IsEmpty()) { Reference = MoveTemp(Routes); Swap(Graph.Edges[0], Graph.Edges[1]); }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoutingSearchTest, "GlooPrint.Routing.SparseSearchAndLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoutingSearchTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Maze;
    RouteNode(Maze, {0, 0}, {100, 80}); RouteNode(Maze, {2050, 0}, {100, 80});
    const auto Key = RouteLink(Maze, 0, 1);
    RouteNode(Maze, {-200, -300}, {2500, 100});
    RouteNode(Maze, {-200, 400}, {2500, 100});
    RouteNode(Maze, {600, -220}, {160, 400});
    RouteNode(Maze, {1050, 80}, {160, 360});
    RouteNode(Maze, {1500, -220}, {160, 400});
    FRouteSet Routes; FString Reason;
    if (!TestTrue(TEXT("Alternating obstacle maze computes"), ComputeRoutes(Maze, Routes, Reason))) { AddError(Reason); return false; }
    const auto& Route = Routes.Wires[Key];
    if (!TestTrue(TEXT("A* finds the maze route after simple channels fail"), Route.Method == ERouteMethod::Search)) { return false; }
    TestEqual(TEXT("Maze has no native fallback"), Routes.FallbackCount, 0);
    TestTrue(TEXT("Maze requires successive direction changes"), Route.Points.Num() >= 8);
    TestEqual(TEXT("Search retains the original source attachment"), Route.Points[0], FVector2f(100, 40));
    TestEqual(TEXT("Search retains the original destination attachment"), Route.Points.Last(), FVector2f(2050, 40));
    CheckClear(*this, Maze, Route);
    FRouteSet DiagonalMaze;
    TestTrue(TEXT("Searched maze supports diagonal styling"), ComputeRoutes(Maze, DiagonalMaze, Reason, EGlooPrintWireStyle::Diagonal45));
    TestTrue(TEXT("Changing style keeps the searched corridor"), DiagonalMaze.Wires[Key].Points == Route.Points);
    CheckClear(*this, Maze, DiagonalMaze.Wires[Key]);
    TestTrue(TEXT("Maze search remains bounded"), Route.Search.ExpandedStates > 0 && Route.Search.ExpandedStates <= 2048);
    AddInfo(FString::Printf(TEXT("Maze: %d expanded states, %d segment checks, %d x %d channels, %d bends."),
        Route.Search.ExpandedStates, Route.Search.SegmentChecks, Route.Search.XChannels, Route.Search.YChannels, Route.Points.Num() - 2));

    Swap(Maze.Nodes[2], Maze.Nodes[6]);
    for (int32 N : {2, 6})
    {
        for (int32 P = Maze.Nodes[N].FirstPin; P < Maze.Nodes[N].FirstPin + Maze.Nodes[N].PinCount; ++P) { Maze.Pins[P].Node = N; }
    }
    FRouteSet Cold;
    TestTrue(TEXT("Cold shuffled maze computes"), ComputeRoutes(Maze, Cold, Reason));
    TestTrue(TEXT("Shuffled obstacle enumeration keeps the same searched route"), Cold.Wires[Key].Points == Route.Points);
    TestEqual(TEXT("Shuffled search has identical expansion count"), Cold.Wires[Key].Search.ExpandedStates, Route.Search.ExpandedStates);

    FLayoutGraph Sealed;
    RouteNode(Sealed, {0, 0}, {100, 80}); RouteNode(Sealed, {1600, 1600}, {100, 80});
    const auto SealedKey = RouteLink(Sealed, 0, 1);
    RouteNode(Sealed, {1450, 1450}, {20, 400}); RouteNode(Sealed, {1800, 1450}, {20, 400});
    RouteNode(Sealed, {1450, 1450}, {370, 20}); RouteNode(Sealed, {1450, 1830}, {370, 20});
    for (int32 I = 0; I < 80; ++I) { RouteNode(Sealed, {200 + I * 13, 240 + I * 11}, {4, 4}); }
    const int32 A = RouteNode(Sealed, {0, -600}, {100, 80}), B = RouteNode(Sealed, {800, -600}, {100, 80});
    const auto Unaffected = RouteLink(Sealed, A, B);
    TestTrue(TEXT("Sealed target preserves other routes"), ComputeRoutes(Sealed, Routes, Reason));
    const auto& Failed = Routes.Wires[SealedKey];
    TestTrue(TEXT("Real unreachable search reaches its expansion budget"), Failed.Search.bStateLimitReached);
    TestEqual(TEXT("Search expands no more than the fixed budget"), Failed.Search.ExpandedStates, 2048);
    TestTrue(TEXT("Sparse states are bounded by generated neighbors"), Failed.Search.CreatedStates <= 8193);
    TestTrue(TEXT("Collision queries are bounded by expanded neighbors"), Failed.Search.SegmentChecks <= 8192);
    TestTrue(TEXT("State exhaustion has an explicit native fallback reason"), Failed.Fallback == ERouteFallback::SearchLimit);
    TestTrue(TEXT("Exhaustion never publishes a partial path"), Failed.Points.IsEmpty() && Failed.Curves.IsEmpty());
    TestTrue(TEXT("Unrelated direct connection is still routed"), Routes.Wires[Unaffected].Method == ERouteMethod::Simple);
    TestEqual(TEXT("Every connection remains present after search exhaustion"), Routes.Wires.Num(), 2);
    AddInfo(FString::Printf(TEXT("Sealed target: %d expanded / %d created states; %d segment checks."),
        Failed.Search.ExpandedStates, Failed.Search.CreatedStates, Failed.Search.SegmentChecks));

    FLayoutGraph Outer;
    RouteNode(Outer, {0, 0}, {100, 80}); RouteNode(Outer, {4000, 0}, {100, 80});
    const auto OuterKey = RouteLink(Outer, 0, 1);
    RouteNode(Outer, {1800, -4000}, {100, 8000});
    for (int32 I = 0; I < 160; ++I) { RouteNode(Outer, {2500 + I * 3, -500 + I * 6}, {1, 1}); }
    TestTrue(TEXT("Limited channel graph retains outer fallback"), ComputeRoutes(Outer, Routes, Reason));
    const auto& OuterRoute = Routes.Wires[OuterKey];
    TestTrue(TEXT("Channel truncation is recorded"), OuterRoute.Search.bChannelsTruncated);
    TestTrue(TEXT("Per-axis channels stay capped plus the two endpoints"), OuterRoute.Search.XChannels <= 130 && OuterRoute.Search.YChannels <= 130);
    TestTrue(TEXT("Outer lanes are tried after the sparse search"), OuterRoute.Search.ExpandedStates > 0 && OuterRoute.Method == ERouteMethod::OuterLane);
    TestEqual(TEXT("A clear outer lane avoids native fallback"), Routes.FallbackCount, 0);
    CheckClear(*this, Outer, OuterRoute);
    TestTrue(TEXT("Cold outer routing computes"), ComputeRoutes(Outer, Cold, Reason));
    TestTrue(TEXT("Outer fallback is deterministic"), Cold.Wires[OuterKey].Points == OuterRoute.Points);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagonalRoutingTest, "GlooPrint.Routing.DiagonalClearance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDiagonalRoutingTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    RouteNode(Graph, {0, 0}, {100, 80}); RouteNode(Graph, {700, 200}, {100, 80});
    const auto Key = RouteLink(Graph, 0, 1);
    RouteNode(Graph, {368, 52}, {20, 60});
    FRouteSet Rounded, Diagonal; FString Reason;
    if (!TestTrue(TEXT("Rounded tight corner computes"), ComputeRoutes(Graph, Rounded, Reason)) ||
        !TestTrue(TEXT("Diagonal tight corner computes"), ComputeRoutes(Graph, Diagonal, Reason, EGlooPrintWireStyle::Diagonal45))) { return false; }
    TestEqual(TEXT("Constrained styling preserves the connection"), Diagonal.FallbackCount, 0);
    const auto& Route = Diagonal.Wires[Key];
    TestTrue(TEXT("Both styles retain the same orthogonal corridor"), Route.Points == Rounded.Wires[Key].Points);
    CheckClear(*this, Graph, Route);
    bool bDiagonal = false, bSquareCorner = false;
    for (const auto& C : Route.Curves)
    {
        const auto Delta = C.End - C.Start;
        TestEqual(TEXT("45-degree pieces are straight Hermite segments"), C.StartTangent, Delta);
        TestEqual(TEXT("45-degree end tangent follows the segment"), C.EndTangent, Delta);
        TestTrue(TEXT("Every segment is horizontal, vertical or exactly 45 degrees"),
            Delta.X == 0 || Delta.Y == 0 || FMath::IsNearlyEqual(FMath::Abs(Delta.X), FMath::Abs(Delta.Y), 0.001f));
        bDiagonal |= Delta.X != 0 && Delta.Y != 0;
        bSquareCorner |= C.Start == FVector2f(400, 40) || C.End == FVector2f(400, 40);
        for (int32 Sample = 0; Sample <= 32; ++Sample)
        {
            const auto P = FMath::Lerp(C.Start, C.End, Sample / 32.f);
            TestFalse(TEXT("Diagonal never cuts through the inflated inside obstacle"), Inside(P, {356, 40}, {400, 124}));
        }
    }
    TestTrue(TEXT("An unconstrained corner has a real diagonal cut"), bDiagonal);
    TestTrue(TEXT("The blocked cut retains its safe square corner"), bSquareCorner);
    TestEqual(TEXT("Diagonal source remains attached"), Route.Curves[0].Start, FVector2f(100, 40));
    TestEqual(TEXT("Diagonal destination remains attached"), Route.Curves.Last().End, FVector2f(700, 240));
    TestEqual(TEXT("Diagonal pin exit stays horizontal"), Route.Curves[0].Start.Y, Route.Curves[0].End.Y);
    TestEqual(TEXT("Diagonal pin entry stays horizontal"), Route.Curves.Last().Start.Y, Route.Curves.Last().End.Y);
    FRouteSet Cold;
    TestTrue(TEXT("Cold diagonal reroute succeeds"), ComputeRoutes(Graph, Cold, Reason, EGlooPrintWireStyle::Diagonal45));
    TestEqual(TEXT("Cold diagonal rendering is deterministic"), Cold.Wires[Key].Length, Route.Length);
    for (int32 I = 0; I < Route.Curves.Num() && I < Cold.Wires[Key].Curves.Num(); ++I)
    {
        TestEqual(TEXT("Cold diagonal endpoints match"), Cold.Wires[Key].Curves[I].End, Route.Curves[I].End);
    }
    Graph.Nodes[1].Geometry.Position.Y = 0;
    TestTrue(TEXT("Aligned diagonal style computes"), ComputeRoutes(Graph, Diagonal, Reason, EGlooPrintWireStyle::Diagonal45));
    TestEqual(TEXT("Aligned diagonal style stays one straight wire"), Diagonal.Wires[Key].Curves.Num(), 1);
    return true;
}
}
#endif
