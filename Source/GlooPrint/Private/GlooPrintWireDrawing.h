// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "EdGraphUtilities.h"
#include "Delegates/Delegate.h"
#include "GlooPrintRouting.h"
#include "Types/ISlateMetaData.h"

class SGraphPanel;
struct FEdGraphEditAction;
struct FPropertyChangedEvent;
class FTransactionObjectEvent;
class FSlateFontCache;

namespace GlooPrint
{
class FRouteCache final : public ISlateMetaData, public TSharedFromThis<FRouteCache>
{
public:
    SLATE_METADATA_TYPE(FRouteCache, ISlateMetaData)
    virtual ~FRouteCache();
    void Initialize(TSharedRef<SGraphPanel> InPanel);
    void Shutdown();
    void ObserveContext();
    void Invalidate(bool bContextChanged = true);
    void StagePlannedRoutes(FLayoutGraph Source, FRouteSet PlannedRoutes, EGlooPrintWireStyle Style);
    const FRouteSet& GetRoutes() const { return Routes; }
    bool IsReady() const { return bReady; }
    int32 GetBuildCount() const { return BuildCount; }
    int32 GetReusedPlanCount() const { return ReusedPlanCount; }
    bool HasPendingRouting() const { return Capture.IsValid() || Routing.IsValid() || Planned.IsSet(); }
    UEdGraph* GetGraph() const { return Graph.Get(); }
    TSharedPtr<SGraphPanel> GetPanel() const { return Panel.Pin(); }

private:
    struct FPlannedRoutes
    {
        FLayoutGraph Source;
        FRouteSet Routes;
    };
    TOptional<FPlannedRoutes> Planned;
    TUniquePtr<FGraphCaptureJob> Capture;
    TWeakPtr<FMeasurementCache> Measurements;
    uint64 MeasurementRevision = 0;
    void Schedule();
    void OnPostTick(float DeltaTime);
    bool Rebuild(float DeltaTime);
    void OnGraphChanged(const FEdGraphEditAction& Action);
    void OnModified(UObject* Object);
    void OnPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
    void OnTransacted(UObject* Object, const FTransactionObjectEvent& Event);
    void OnSlateInvalidated(bool bClearResources);
    void OnFontsReleased(const FSlateFontCache& Fonts);

    TWeakPtr<SGraphPanel> Panel;
    TWeakObjectPtr<UEdGraph> Graph;
    FDelegateHandle RebuildHandle;
    TWeakPtr<FSlateFontCache> FontCache;
    FDelegateHandle GraphHandle, ModifiedHandle, PropertyHandle, TransactionHandle, SlateHandle, FontHandle;
    FRouteSet Routes;
    TUniquePtr<FRoutingJob> Routing;
    uint64 RoutingRevision = 0;
    float Scale = 0;
    SGraphEditor::EPinVisibility PinVisibility = SGraphEditor::Pin_Show;
    uint64 Revision = 0;
    uint16 TextRevision = 0;
    const void* StyleIdentity = nullptr;
    EGlooPrintWireStyle WireStyle = EGlooPrintWireStyle::Rounded90;
    int32 AttemptsLeft = 3;
    int32 BuildCount = 0;
    int32 ReusedPlanCount = 0;
    bool bReady = false;
    bool bStopped = false;
};

class FWireDrawing final : public FGraphPanelPinConnectionFactory
{
public:
    FWireDrawing();
    virtual FConnectionDrawingPolicy* CreateConnectionPolicy(const UEdGraphSchema* Schema,
        int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
        FSlateWindowElementList& Elements, UEdGraph* Graph) const override;
    TSharedPtr<FRouteCache> GetCache(TSharedRef<SGraphPanel> Panel) const;
    void Shutdown();
    void OnSettingsChanged();

private:
    mutable TArray<TWeakPtr<FRouteCache>> Caches;
    EGlooPrintWireStyle WireStyle;
    bool bStopped = false;
};
}
