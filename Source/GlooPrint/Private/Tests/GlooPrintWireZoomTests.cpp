// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "Rendering/DrawElementTypes.h"
#include "SGraphPanel.h"
#include "Types/PaintArgs.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWireZoomCheck final : public IAutomationLatentCommand
{
public:
    FWireZoomCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle) : Test(InTest), Style(InStyle) {}
    virtual ~FWireZoomCheck() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalStyle = Settings->WireStyle; bRestore = true;
            Settings->WireStyle = Style; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>();
            Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then)->LinkedTo[0]->GetOwningNode()->SetPosition({700, 280});
            Fixture->Print->SetPosition({320, 30});
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint zoom regression")))
                .ClientSize(FVector2f(1200, 850))[Editor.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -200), 1.f);
            Deadline = FPlatformTime::Seconds() + 60;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Zoom fixture timed out.")); return Finish(); }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady() || ++Frames < 8) { return false; }
        if (Step == INDEX_NONE)
        {
            Builds = Cache->GetBuildCount();
            Before = SerializeTransactionValues(*Fixture->Graph);
            Step = 0;
        }
        else
        {
            CheckPaint(*Panel, *Cache);
            Test.TestEqual(TEXT("Zooming does not reroute the graph"), Cache->GetBuildCount(), Builds);
            Test.TestTrue(TEXT("Zooming does not change nodes or connections"), Before == SerializeTransactionValues(*Fixture->Graph));
            if (++Step == UE_ARRAY_COUNT(Zooms)) { return Finish(); }
        }
        // Traverse every native zoom/LOD level, then return to the starting view.
        Editor->SetViewLocation(FVector2f(-100.f + Step * 3.f, -200.f + Step * 2.f), Zooms[Step]);
        Frames = 0;
        return false;
    }
private:
    void CheckPaint(SGraphPanel& Panel, const FRouteCache& Cache)
    {
        FHittestGrid Grid;
        FSlateWindowElementList Elements(Window);
        auto& Slate = FSlateApplication::Get();
        FPaintArgs Args(&Panel, Grid, FVector2f::ZeroVector, Slate.GetCurrentTime(), 0);
        const FGeometry& Geometry = Panel.GetCachedGeometry();
        Panel.OnPaint(Args, Geometry, FSlateRect(-100000, -100000, 100000, 100000), Elements, 0, FWidgetStyle(), true);
        const auto& Pieces = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
        int32 ExpectedPieces = 0;
        for (const auto& Pair : Cache.GetRoutes().Wires) { ExpectedPieces += Pair.Value.Curves.Num(); }
        const FString Context = FString::Printf(TEXT("Zoom %.3f"), Panel.GetZoomAmount());
        Test.TestTrue(Context + TEXT(" keeps routed pieces, including any extra pin attachments"), Pieces.Num() >= ExpectedPieces);
        const auto Transform = [&](FVector2f Point)
        {
            return FVector2f(Geometry.LocalToAbsolute((Point - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount()));
        };
        for (const auto& Pair : Cache.GetRoutes().Wires)
        {
            const auto& Curves = Pair.Value.Curves;
            for (int32 Index = 1; Index + 1 < Curves.Num(); ++Index)
            {
                const auto& Curve = Curves[Index];
                const bool bFound = Pieces.ContainsByPredicate([&](const FSlateSplineElement& Piece)
                {
                    return Piece.P0.Equals(Transform(Curve.Start), 0.1f) && Piece.P3.Equals(Transform(Curve.End), 0.1f);
                });
                Test.TestTrue(Context + TEXT(" keeps each interior wire segment in graph coordinates"), bFound);
            }
        }
    }
    void Restore()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset();
        if (bRestore)
        {
            bRestore = false;
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Settings->WireStyle = OriginalStyle; Settings->NotifyChanged();
        }
    }
    bool Finish() { Restore(); return true; }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TArray<uint8> Before;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    double Deadline = 0;
    int32 Step = INDEX_NONE, Frames = 0, Builds = 0;
    bool bRestore = false;
    const float Zooms[21] = {2, 1.875f, 1.75f, 1.675f, 1.5f, 1.375f, 1.25f, 1, 0.875f, 0.75f,
        0.675f, 0.5f, 0.375f, 0.25f, 0.225f, 0.2f, 0.175f, 0.15f, 0.125f, 0.1f, 1};
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoundedWireZoomTest, "GlooPrint.Editor.WireZoom.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoundedWireZoomTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireZoomCheck(*this, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagonalWireZoomTest, "GlooPrint.Editor.WireZoom.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDiagonalWireZoomTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireZoomCheck(*this, EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
