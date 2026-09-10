// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NodeFactory.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWirePaintBenchmark final : public IAutomationLatentCommand
{
public:
    explicit FWirePaintBenchmark(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FWirePaintBenchmark() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native wire-paint benchmark exceeded 180 seconds.")); return Finish(); }
        if (++Frames < 12) { return false; }
        if (Phase == 3)
        {
            Test.TestFalse(TEXT("Closing the 1000-wire graph releases its route cache"), ClosedCache.IsValid());
            RecordMemory(TEXT("Closed"), 0);
            Test.TestTrue(TEXT("Paint benchmark preserves every graph value"), Before == SerializeTransactionValues(*Fixture->Graph));
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        if (Phase == 0)
        {
            Before = SerializeTransactionValues(*Fixture->Graph);
            Test.TestFalse(TEXT("Native baseline has no custom route cache"), Panel->GetMetaData<FRouteCache>().IsValid());
            RecordMemory(TEXT("Native"), 0); SetStyle(EGlooPrintWireStyle::Rounded90);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            if (!Test.TestEqual(TEXT("Every visible connection has a cached route"), Cache->GetRoutes().Wires.Num(), LinkCount) ||
                !Test.TestEqual(TEXT("Paint benchmark has no native fallback routes"), Cache->GetRoutes().FallbackCount, 0)) { return Finish(); }
            ExpectedPieces = 0; RouteBytes = Cache->GetRoutes().Wires.GetAllocatedSize();
            for (const auto& Pair : Cache->GetRoutes().Wires)
            {
                ExpectedPieces += Pair.Value.Curves.Num();
                RouteBytes += Pair.Value.Points.GetAllocatedSize() + Pair.Value.Curves.GetAllocatedSize();
            }
            if (!Test.TestTrue(TEXT("Benchmark includes curved routes as well as straight wires"), ExpectedPieces > LinkCount * 2)) { return Finish(); }
            Builds = Cache->GetBuildCount(); RecordMemory(StyleName(), RouteBytes);
            NativeTimes.Reset(); CustomTimes.Reset(); ExtraTimes.Reset(); Sample = -3;
            Phase = 2; Frames = 0; return false;
        }
        Test.TestEqual(TEXT("Idle paint does not rebuild routes"), Cache->GetBuildCount(), Builds);
        Test.TestFalse(TEXT("Idle paint has no pending capture or routing job"), Cache->HasPendingRouting());
        if (!Measure()) { return Finish(); }
        if (++Sample < SamplesWanted) { return false; }
        Summarize(); Capture();
        if (Style == EGlooPrintWireStyle::Rounded90)
        {
            SetStyle(EGlooPrintWireStyle::Diagonal45); Phase = 1; Frames = 0; return false;
        }
        ClosedCache = Cache; Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
        Phase = 3; Frames = 0; return false;
    }
private:
    bool Start()
    {
        auto& Slate = FSlateApplication::Get();
        OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle; OriginalCursor = Slate.GetCursorPos(); bRestore = true;
        FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted);
        SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100);
        Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks"); IFileManager::Get().MakeDirectory(*Directory, true);
        SetStyle(EGlooPrintWireStyle::Native);
        Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
        for (int32 I = 0; I < LinkCount; ++I)
        {
            const FVector2f Position(float((I % 25) * 320), float((I / 25) * 128));
            auto* From = Fixture->Add<UK2Node_Knot>(Position);
            auto* To = Fixture->Add<UK2Node_Knot>(Position + FVector2f(200, I % 2 ? 48 : 0));
            if (!Fixture->Graph->GetSchema()->TryCreateConnection(From->GetOutputPin(), To->GetInputPin()))
            {
                Test.AddError(TEXT("Native schema refused the reroute-pair fixture.")); return Finish();
            }
        }
        for (UEdGraphNode* Node : Fixture->Graph->Nodes) for (UEdGraphPin* Pin : Node->Pins)
        {
            FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip);
        }
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint 1000 visible wire benchmark")))
            .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
        Slate.AddWindow(Window.ToSharedRef()); Editor->ZoomToFit(false); Slate.SetCursorPos(FVector2D::ZeroVector);
        Deadline = FPlatformTime::Seconds() + 180; return false;
    }
    bool Measure()
    {
        auto* Panel = Editor->GetGraphPanel();
        FArrangedChildren Nodes(EVisibility::Visible);
        TMap<TSharedRef<SWidget>, FArrangedWidget> Pins; Pins.Reserve(Fixture->Graph->Nodes.Num() * 2);
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Native benchmark node has painted geometry"), Widget.IsValid())) { return false; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                const auto PinWidget = Widget->FindWidgetForPin(Pin);
                if (!Test.TestTrue(TEXT("Native benchmark pin has painted geometry"), PinWidget.IsValid())) { return false; }
                Pins.Add(PinWidget.ToSharedRef(), FArrangedWidget(PinWidget.ToSharedRef(), PinWidget->GetCachedGeometry()));
            }
        }
        const auto& Geometry = Panel->GetCachedGeometry();
        const FVector2f Min = Geometry.GetAbsolutePosition(), Max = Geometry.LocalToAbsolute(Geometry.GetLocalSize());
        const FSlateRect Clip(Min.X, Min.Y, Max.X, Max.Y);
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const auto Draw = [&](bool bNative, int32& OutPieces)
        {
            FSlateWindowElementList Elements(Window);
            const double StartTime = FPlatformTime::Seconds();
            {
                TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BenchmarkWirePolicy);
                TUniquePtr<FConnectionDrawingPolicy> Policy(bNative ? new FKismetConnectionDrawingPolicy(0, 1, Scale, Clip, Elements, Fixture->Graph) :
                    FNodeFactory::CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, Scale, Clip, Elements, Fixture->Graph));
                if (!Policy) { OutPieces = 0; return 0.0; }
                Policy->SetAbsoluteMousePosition(FVector2f(-100000)); Policy->Draw(Pins, Nodes);
            }
            const double Milliseconds = (FPlatformTime::Seconds() - StartTime) * 1000;
            OutPieces = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>().Num();
            return Milliseconds;
        };
        double NativeMs = 0, CustomMs = 0; int32 NativePieces = 0, CustomPieces = 0;
        const bool bNativeFirst = (Sample & 1) == 0;
        if (bNativeFirst) { NativeMs = Draw(true, NativePieces); CustomMs = Draw(false, CustomPieces); }
        else { CustomMs = Draw(false, CustomPieces); NativeMs = Draw(true, NativePieces); }
        Csv += FString::Printf(TEXT("%s,%d,%d,%.6f,%.6f,%.6f,%d,%d,%llu\n"), StyleName(), Sample, int32(bNativeFirst),
            NativeMs, CustomMs, CustomMs - NativeMs, NativePieces, CustomPieces, RouteBytes);
        if (!Test.TestEqual(TEXT("Native policy paints all 1000 visible connections"), NativePieces, LinkCount) ||
            !Test.TestEqual(TEXT("Custom policy paints every cached piece without culling or fallback"), CustomPieces, ExpectedPieces)) { return false; }
        if (NativeMs > 100 || CustomMs > 100) { Test.AddError(TEXT("Wire paint exceeded 100 ms; stopped repeated sampling, retaining the diagnostic.")); return false; }
        if (Sample >= 0) { NativeTimes.Add(NativeMs); CustomTimes.Add(CustomMs); ExtraTimes.Add(CustomMs - NativeMs); }
        return true;
    }
    void Summarize()
    {
        if (SamplesWanted < 20)
        {
            Test.AddInfo(FString::Printf(TEXT("%s diagnostic: %d samples, final native %.3fms/custom %.3fms for 1000 wires/%d custom pieces. No p95 or target acceptance."),
                StyleName(), SamplesWanted, NativeTimes.Last(), CustomTimes.Last(), ExpectedPieces));
            return;
        }
        const auto P95 = [](TArray<double>& Times) { Times.Sort(); return Times[FMath::CeilToInt(Times.Num() * 0.95) - 1]; };
        const double Native95 = P95(NativeTimes), Custom95 = P95(CustomTimes), Extra95 = P95(ExtraTimes);
        Test.AddInfo(FString::Printf(TEXT("%s: %d samples after 3 warmups; 1000 native splines/%d custom pieces; p95 policy CPU native %.3fms, custom %.3fms, paired extra %.3fms; %llu retained route-array/map bytes. Node painting, batching and GPU excluded."),
            StyleName(), SamplesWanted, ExpectedPieces, Native95, Custom95, Extra95, RouteBytes));
        Test.TestTrue(TEXT("Additional p95 wire-policy CPU is under 1 ms for 1000 visible connections"), Extra95 < 1.0);
    }
    void RecordMemory(const TCHAR* Stage, uint64 RetainedRoutes)
    {
        const auto Memory = FPlatformMemory::GetStats();
        MemoryCsv += FString::Printf(TEXT("%s,%llu,%llu,%llu\n"), Stage, Memory.UsedPhysical, Memory.PeakUsedPhysical, RetainedRoutes);
    }
    void Capture()
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture actual 1000-wire viewport"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        Test.TestTrue(TEXT("Save wire benchmark screenshot"), FFileHelper::SaveArrayToFile(Png, *(Directory / (FString(TEXT("1000-WirePaint-")) + StyleName() + TEXT(".png")))));
    }
    const TCHAR* StyleName() const { return Style == EGlooPrintWireStyle::Rounded90 ? TEXT("Rounded") : TEXT("Diagonal"); }
    void SetStyle(EGlooPrintWireStyle Value)
    {
        Style = Value; auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = Value; Settings->NotifyChanged();
    }
    bool Finish()
    {
        if (!Directory.IsEmpty())
        {
            Test.TestTrue(TEXT("Save raw wire paint samples"), FFileHelper::SaveStringToFile(Csv, *(Directory / TEXT("1000-WirePaint.csv"))));
            Test.TestTrue(TEXT("Save process memory observations"), FFileHelper::SaveStringToFile(MemoryCsv, *(Directory / TEXT("1000-WirePaint-Memory.csv"))));
        }
        Restore(); return true;
    }
    void Restore()
    {
        if (Window) { Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset(); }
        if (bRestore) { bRestore = false; SetStyle(OriginalStyle); FSlateApplication::Get().SetCursorPos(OriginalCursor); }
    }
    static constexpr int32 LinkCount = 1000;
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> ClosedCache;
    TArray<uint8> Before;
    TArray<double> NativeTimes, CustomTimes, ExtraTimes;
    EGlooPrintWireStyle Style = EGlooPrintWireStyle::Native, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FVector2D OriginalCursor;
    FString Directory;
    FString Csv = TEXT("style,sample,native_first,native_ms,custom_ms,extra_ms,native_splines,custom_pieces,retained_route_bytes\n");
    FString MemoryCsv = TEXT("stage,process_resident_bytes,process_peak_resident_bytes,retained_route_bytes\n");
    uint64 RouteBytes = 0;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Sample = 0, SamplesWanted = 30, ExpectedPieces = 0, Builds = 0;
    bool bRestore = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWirePaintPerformanceTest, "GlooPrint.Performance.WirePaint.1000",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
bool FWirePaintPerformanceTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWirePaintBenchmark(*this)); return true;
}
}
#endif
