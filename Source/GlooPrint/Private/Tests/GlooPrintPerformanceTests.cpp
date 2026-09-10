// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintLayout.h"
#include "GlooPrintRouting.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GlooPrint::Tests
{
namespace
{
FLayoutGraph BenchmarkGraph(int32 Count, const FString& Family)
{
    FLayoutGraph Graph;
    const bool bDense = Family == TEXT("Dense"), bHeavy = Family == TEXT("PinHeavy"), bFan = Family == TEXT("FanOut");
    const int32 PinsPerNode = bHeavy ? 64 : bDense ? 5 : 2;
    Graph.Nodes.Reserve(Count); Graph.Pins.Reserve(Count * PinsPerNode); Graph.Edges.Reserve(Count * 4);
    for (int32 I = 0; I < Count; ++I)
    {
        auto& Node = Graph.Nodes.AddDefaulted_GetRef();
        Node.Geometry.Id = FGuid(0, 0, 0, I + 1);
        Node.Geometry.Position = {((I * 37) % 31) * 240, ((I * 19) % 29) * 160};
        Node.Geometry.BodySize = {160, bHeavy ? 840.f : bDense ? 160.f : 84.f};
        Node.Geometry.VisualBounds = {FVector2f::ZeroVector, Node.Geometry.BodySize};
        Node.FirstPin = Graph.Pins.Num(); Node.PinCount = PinsPerNode;
        Node.bEntry = !bFan && (bDense ? I < 8 : I == 0);
        Node.Incoming.Reserve(4); Node.Outgoing.Reserve(bFan && I == 0 ? Count - 1 : 4);
        for (int32 P = 0; P < PinsPerNode; ++P)
        {
            const bool bOutput = bDense ? P > 0 : P % 2 != 0;
            const ELinkKind Kind = bFan || (bHeavy && P >= 2) ? ELinkKind::Data : ELinkKind::Execution;
            const float Y = bDense ? 40 + FMath::Max(0, P - 1) * 28 : 40 + (P / 2) * 24;
            Graph.Pins.Add({FGuid(0, 0, I + 1, P + 1), I, P, bOutput, Kind, FVector2f(bOutput ? 160 : 0, Y)});
        }
    }
    const auto Link = [&](int32 From, int32 To, int32 OutPin, int32 InPin)
    {
        const int32 A = Graph.Nodes[From].FirstPin + OutPin, B = Graph.Nodes[To].FirstPin + InPin;
        const int32 E = Graph.Edges.Add({A, B, Graph.Pins[A].Kind});
        Graph.Nodes[From].Outgoing.Add(E); Graph.Nodes[To].Incoming.Add(E);
    };
    for (int32 I = 0; I < Count; ++I)
    {
        if (bFan) { if (I) { Link(0, I, 1, 0); } }
        else if (bDense)
        {
            const int32 Next = (I / 8 + 1) * 8;
            for (int32 P = 0; P < 4; ++P)
            {
                const int32 Target = Next + ((I + P * 2) % 8);
                if (Target < Count) { Link(I, Target, P + 1, 0); }
            }
        }
        else
        {
            if (I + 1 < Count) { Link(I, I + 1, 1, 0); }
            if (bHeavy)
            {
                for (int32 P = 0; P < 3; ++P)
                {
                    const int32 Target = I + 1 + P * 3;
                    if (Target < Count) { Link(I, Target, 3 + P * 2, 2 + P * 2); }
                }
            }
        }
    }
    Graph.Anchor = 0;
    return Graph;
}

class FPipelineBenchmark final : public IAutomationLatentCommand
{
public:
    FPipelineBenchmark(FAutomationTestBase& InTest, int32 Count, FString InFamily)
        : Test(InTest), Family(MoveTemp(InFamily)), Graph(BenchmarkGraph(Count, Family))
    {
        FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted);
        SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100);
    }
    virtual bool Update() override
    {
        FLayoutResult Layout; FRouteSet Routes; FString Reason;
        const double Start = FPlatformTime::Seconds();
        const bool bLayout = ComputeLayout(Graph, {}, Layout, Reason);
        const double LaidOut = FPlatformTime::Seconds();
        const bool bRouting = bLayout && ComputeLayoutRoutes(Graph, Layout, Routes, Reason);
        const double Routed = FPlatformTime::Seconds();
        if (!Test.TestTrue(TEXT("Benchmark computes complete layout and routing"), bRouting)) { Test.AddError(Reason); return true; }
        Test.TestEqual(TEXT("Benchmark retains every original node"), Layout.Positions.Num(), Graph.Nodes.Num());
        Test.TestEqual(TEXT("Benchmark retains every original link including explicit fallbacks"), Routes.Wires.Num(), Graph.Edges.Num());
        const double LayoutMs = (LaidOut - Start) * 1000, RouteMs = (Routed - LaidOut) * 1000;
        int32 Curves = 0, Bends = 0, Expanded = 0;
        uint64 RouteBytes = Routes.Wires.GetAllocatedSize();
        for (const auto& Pair : Routes.Wires)
        {
            Curves += Pair.Value.Curves.Num(); Bends += FMath::Max(0, Pair.Value.Points.Num() - 2);
            Expanded += Pair.Value.Search.ExpandedStates;
            RouteBytes += Pair.Value.Points.GetAllocatedSize() + Pair.Value.Curves.GetAllocatedSize();
        }
        Csv += FString::Printf(TEXT("%s,%d,%d,%d,%d,%.6f,%.6f,%.6f,%d,%d,%d,%d,%llu\n"),
            *Family, Graph.Nodes.Num(), Graph.Pins.Num(), Graph.Edges.Num(), Sample,
            LayoutMs, RouteMs, LayoutMs + RouteMs, Routes.FallbackCount, Bends, Curves, Expanded, RouteBytes);
        if (Sample == 0 && FParse::Param(FCommandLine::Get(), TEXT("GlooPrintBenchmarkRouteDetails"))) { SaveRouteDetails(Layout, Routes); }
        if (Sample > 0)
        {
            LayoutTimes.Add(LayoutMs); RouteTimes.Add(RouteMs); TotalTimes.Add(LayoutMs + RouteMs);
        }
        if (++Sample <= SamplesWanted && LayoutMs + RouteMs < 2000) { return false; }
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks");
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Path = Directory / FString::Printf(TEXT("%d-%s.csv"), Graph.Nodes.Num(), *Family);
        Test.TestTrue(TEXT("Save all benchmark samples"), FFileHelper::SaveStringToFile(Csv, *Path));
        if (TotalTimes.Num() != SamplesWanted)
        {
            Test.AddError(FString::Printf(TEXT("%s/%d single cached-geometry job took %.1f ms; stopped repeated sampling. No p95 or responsiveness claim. Raw sample: %s"),
                *Family, Graph.Nodes.Num(), LayoutMs + RouteMs, *Path));
            return true;
        }
        if (SamplesWanted < 20)
        {
            Test.AddInfo(FString::Printf(TEXT("%s/%d diagnostic capture only: %d samples after warmup, final layout %.3f ms, routing %.3f ms. No p95/target claim. CSV: %s"),
                *Family, Graph.Nodes.Num(), SamplesWanted, LayoutMs, RouteMs, *Path));
            return true;
        }
        const auto P95 = [](TArray<double>& Values) { Values.Sort(); return Values[FMath::CeilToInt(Values.Num() * 0.95) - 1]; };
        const double Layout95 = P95(LayoutTimes), Route95 = P95(RouteTimes), Total95 = P95(TotalTimes);
        const double Target = Graph.Nodes.Num() <= 100 ? 20 : Graph.Nodes.Num() <= 1000 ? 150 : 1000;
        Test.AddInfo(FString::Printf(TEXT("%s: %dn/%dp/%de, %d samples after warmup; p95 layout %.3f ms, routing %.3f ms, combined %.3f ms; %d fallbacks, %d bends, %d curves, %d search expansions, %llu retained route bytes. CSV: %s"),
            *Family, Graph.Nodes.Num(), Graph.Pins.Num(), Graph.Edges.Num(), SamplesWanted, Layout95, Route95, Total95,
            Routes.FallbackCount, Bends, Curves, Expanded, RouteBytes, *Path));
        Test.TestTrue(FString::Printf(TEXT("Cached-geometry p95 meets the %.0f ms initial target"), Target), Total95 < Target);
        return true;
    }
private:
    void SaveRouteDetails(const FLayoutResult& Layout, const FRouteSet& Routes)
    {
        FString Nodes = TEXT("node,x,y,width,height\n");
        for (int32 I = 0; I < Graph.Nodes.Num(); ++I)
        {
            const auto& Size = Graph.Nodes[I].Geometry.BodySize;
            Nodes += FString::Printf(TEXT("%d,%d,%d,%.3f,%.3f\n"), I, Layout.Positions[I].X, Layout.Positions[I].Y, Size.X, Size.Y);
        }
        FString Details = TEXT("edge,from_node,to_node,from_pin,to_pin,start_x,start_y,end_x,end_y,method,fallback,expansions,segment_checks,x_channels,y_channels,length,bends,points\n");
        for (int32 I = 0; I < Graph.Edges.Num(); ++I)
        {
            const auto& Edge = Graph.Edges[I]; const auto& From = Graph.Pins[Edge.From]; const auto& To = Graph.Pins[Edge.To];
            const auto& Route = Routes.Wires.FindChecked({Graph.Nodes[From.Node].Geometry.Id, From.Id, Graph.Nodes[To.Node].Geometry.Id, To.Id});
            const FVector2f Start = FVector2f(Layout.Positions[From.Node]) + From.Offset.GetValue();
            const FVector2f End = FVector2f(Layout.Positions[To.Node]) + To.Offset.GetValue();
            FString Points;
            for (auto Point : Route.Points) { Points += FString::Printf(TEXT("%.3f:%.3f;"), Point.X, Point.Y); }
            Details += FString::Printf(TEXT("%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%.3f,%d,%s\n"),
                I, From.Node, To.Node, From.Ordinal, To.Ordinal, Start.X, Start.Y, End.X, End.Y, int32(Route.Method), int32(Route.Fallback),
                Route.Search.ExpandedStates, Route.Search.SegmentChecks, Route.Search.XChannels, Route.Search.YChannels,
                Route.Length, FMath::Max(0, Route.Points.Num() - 2), *Points);
        }
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks");
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Prefix = Directory / FString::Printf(TEXT("%d-%s"), Graph.Nodes.Num(), *Family);
        Test.TestTrue(TEXT("Save diagnostic node positions"), FFileHelper::SaveStringToFile(Nodes, *(Prefix + TEXT("-nodes.csv"))));
        Test.TestTrue(TEXT("Save diagnostic route paths"), FFileHelper::SaveStringToFile(Details, *(Prefix + TEXT("-routes.csv"))));
    }
    FAutomationTestBase& Test;
    FString Family;
    FLayoutGraph Graph;
    int32 Sample = 0, SamplesWanted = 30;
    TArray<double> LayoutTimes, RouteTimes, TotalTimes;
    FString Csv = TEXT("family,nodes,pins,links,sample,layout_ms,routing_ms,total_ms,fallbacks,bends,curves,search_expansions,retained_route_bytes\n");
};

class FLayoutSliceBenchmark final : public IAutomationLatentCommand
{
public:
    FLayoutSliceBenchmark(FAutomationTestBase& InTest, FString InFamily) : Test(InTest), Family(MoveTemp(InFamily)) {}
    virtual bool Update() override
    {
        if (!Job)
        {
            FLayoutGraph Graph = BenchmarkGraph(5000, Family);
            AnchorPosition = Graph.Nodes[Graph.Anchor].Geometry.Position;
            const double Start = FPlatformTime::Seconds();
            Job = MakeUnique<FLayoutJob>(MoveTemp(Graph), FLayoutSettings());
            SetupMs = (FPlatformTime::Seconds() - Start) * 1000;
            return false;
        }
        const double Start = FPlatformTime::Seconds();
        const bool bDone = Job->Advance(0);
        const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000;
        TotalMs += Milliseconds; MaxMs = FMath::Max(MaxMs, Milliseconds); ++Slices;
        Csv += FString::Printf(TEXT("%d,%.6f,%d\n"), Slices, Milliseconds, bDone ? 1 : 0);
        if (!bDone && Slices < 4) { return false; }
        Test.TestTrue(TEXT("All bounded layout candidates finish"), bDone);
        FLayoutResult Layout; FString Reason;
        if (Test.TestTrue(TEXT("Layout slices return a complete valid result"), Job->TakeResult(Layout, Reason)))
        {
            Test.TestEqual(TEXT("Layout slices retain every node"), Layout.Positions.Num(), 5000);
            Test.TestEqual(TEXT("Layout slices preserve the chosen anchor"), Layout.Positions[0], AnchorPosition);
        }
        else { Test.AddError(Reason); }
        Test.AddInfo(FString::Printf(TEXT("Layout %s/5000: %d candidates; setup %.3fms, max %.3fms, active layout %.3fms. Diagnostic atomic-unit timing, not p95 or a whole-F responsiveness pass."),
            *Family, Slices, SetupMs, MaxMs, TotalMs));
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks");
        IFileManager::Get().MakeDirectory(*Directory, true);
        Test.TestTrue(TEXT("Save each atomic layout candidate timing"), FFileHelper::SaveStringToFile(Csv,
            *(Directory / (TEXT("5000-") + Family + TEXT("-LayoutSlices.csv")))));
        return true;
    }
private:
    FAutomationTestBase& Test;
    FString Family;
    TUniquePtr<FLayoutJob> Job;
    FIntPoint AnchorPosition;
    int32 Slices = 0;
    double SetupMs = 0, MaxMs = 0, TotalMs = 0;
    FString Csv = TEXT("candidate,elapsed_ms,finished\n");
};

class FRoutingSliceBenchmark final : public IAutomationLatentCommand
{
public:
    FRoutingSliceBenchmark(FAutomationTestBase& InTest, FString InFamily) : Test(InTest), Family(MoveTemp(InFamily)) {}
    virtual bool Update() override
    {
        if (!Job)
        {
            FLayoutGraph Graph = BenchmarkGraph(5000, Family);
            FLayoutResult Layout; FString Reason;
            if (!Test.TestTrue(TEXT("Prepare sliced-routing geometry"), ComputeLayout(Graph, {}, Layout, Reason))) { Test.AddError(Reason); return true; }
            for (int32 I = 0; I < Graph.Nodes.Num(); ++I) { Graph.Nodes[I].Geometry.Position = Layout.Positions[I]; }
            Links = Graph.Edges.Num();
            const double Start = FPlatformTime::Seconds();
            Job = MakeUnique<FRoutingJob>(MoveTemp(Graph));
            SetupMs = (FPlatformTime::Seconds() - Start) * 1000;
            return false;
        }
        const double Start = FPlatformTime::Seconds();
        const bool bDone = Job->Advance(Start + 0.004);
        const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000;
        TotalMs += Milliseconds; MaxMs = FMath::Max(MaxMs, Milliseconds); ++Slices;
        Csv += FString::Printf(TEXT("%d,%.6f,%d,%d\n"), Slices, Milliseconds, Job->GetCompletedLinks(), bDone ? 1 : 0);
        if (!bDone && Slices < 10000 && Milliseconds < 200) { return false; }
        Test.TestTrue(TEXT("Sliced routing terminates without an oversized blocking unit"), bDone);
        Test.TestTrue(TEXT("Large routing job yields to more than one editor frame"), Slices > 1);
        Test.TestTrue(TEXT("Routing units stay below 50ms on the reference machine"), MaxMs < 50 && SetupMs < 50);
        if (bDone)
        {
            FRouteSet Routes; FString Reason;
            Test.TestTrue(TEXT("Finished sliced job publishes a complete result"), Job->TakeResult(Routes, Reason));
            Test.TestEqual(TEXT("Sliced routing retains all original links"), Routes.Wires.Num(), Links);
            Test.AddInfo(FString::Printf(TEXT("Sliced %s/5000: %d slices; setup %.3fms, max %.3fms, active routing %.3fms; %d fallbacks. Native capture, layout and painting excluded."),
                *Family, Slices, SetupMs, MaxMs, TotalMs, Routes.FallbackCount));
        }
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks");
        IFileManager::Get().MakeDirectory(*Directory, true);
        Test.TestTrue(TEXT("Save individual routing chunk timings"), FFileHelper::SaveStringToFile(Csv,
            *(Directory / (TEXT("5000-") + Family + TEXT("-Slices.csv")))));
        return true;
    }
private:
    FAutomationTestBase& Test;
    FString Family;
    TUniquePtr<FRoutingJob> Job;
    int32 Slices = 0, Links = 0;
    double SetupMs = 0, MaxMs = 0, TotalMs = 0;
    FString Csv = TEXT("slice,elapsed_ms,completed_links,finished\n");
};

}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FCachedGeometryBenchmark, "GlooPrint.Performance.CachedGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FCachedGeometryBenchmark::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int32 Count : {100, 1000, 5000})
    {
        for (const TCHAR* Family : {TEXT("Chain"), TEXT("Dense"), TEXT("FanOut"), TEXT("PinHeavy")})
        {
            const FString Name = FString::Printf(TEXT("%d.%s"), Count, Family);
            Names.Add(Name); Commands.Add(Name);
        }
    }
}
bool FCachedGeometryBenchmark::RunTest(const FString& Parameters)
{
    FString Count, Family;
    if (!Parameters.Split(TEXT("."), &Count, &Family)) { AddError(TEXT("Expected fixture size and family.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FPipelineBenchmark(*this, FCString::Atoi(*Count), Family)); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FRoutingSlicesTest, "GlooPrint.Performance.RoutingSlices.5000",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FRoutingSlicesTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Family : {TEXT("FanOut"), TEXT("PinHeavy")}) { Names.Add(Family); Commands.Add(Family); }
}
bool FRoutingSlicesTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FRoutingSliceBenchmark(*this, Parameters)); return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FLayoutSlicesTest, "GlooPrint.Performance.LayoutSlices.5000",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FLayoutSlicesTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Family : {TEXT("Dense"), TEXT("PinHeavy")}) { Names.Add(Family); Commands.Add(Family); }
}
bool FLayoutSlicesTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FLayoutSliceBenchmark(*this, Parameters)); return true;
}

}
#endif
