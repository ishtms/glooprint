// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintLayout.h"
#include "GlooPrintRouting.h"
#include "Framework/Application/IInputProcessor.h"

class SGraphPanel;
class FUICommandList;
class FExtender;
class FMenuBuilder;
class UEdGraphNode;
class UEdGraphPin;
class FSlateFontCache;
class SNotificationItem;
struct FPropertyChangedEvent;

namespace GlooPrint
{
struct FFormatPlan
{
    FLayoutGraph Snapshot;
    FLayoutResult Layout;
    FRouteSet Routes;
    FLayoutGraph RouteSource;
    int32 LayoutAttempts = 0;
    int32 CommentMeasurements = 0;
    int32 SpacingRepairs = 0;
};

class FFormatJob final
{
public:
    FFormatJob(UEdGraph* Graph, FLayoutGraph Snapshot, float Scale, FLayoutSettings Settings, EGlooPrintWireStyle Style,
        bool bRetainRouteSource = false);
    ~FFormatJob();
    bool Advance(double Deadline);
    bool TakePlan(FFormatPlan& OutPlan, FString& OutReason, bool* OutNeedsLayoutRetry = nullptr);
private:
    struct FState;
    TUniquePtr<FState> State;
};

bool PlanFormatGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection, FFormatPlan& OutPlan,
    FString& OutReason, const FMeasurementOptions& MeasurementOptions = {}, bool* OutNeedsLayoutRetry = nullptr);

bool ApplyLayout(UEdGraph* Graph, const FLayoutGraph& Snapshot, const FLayoutResult& Layout,
    int32& ChangedNodes, FString& OutReason);
bool FormatGraph(UEdGraph* Graph, float LayoutScale, const TSet<FGuid>& Selection,
    int32& ChangedNodes, FString& OutReason, const FMeasurementOptions& MeasurementOptions = {}, bool* OutNeedsLayoutRetry = nullptr);

class FEditor final : public IInputProcessor, public TSharedFromThis<FEditor>
{
public:
    void Initialize();
    void Shutdown();
    void OnSettingsChanged();
    virtual void Tick(float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override;
    virtual const TCHAR* GetDebugName() const override { return TEXT("GlooPrint Format Graph"); }

private:
    struct FPendingNode
    {
        TWeakObjectPtr<UEdGraphNode> Node;
        TArray<uint8> State;
    };
    struct FPendingFormat
    {
        TWeakPtr<SGraphPanel> Panel;
        TWeakObjectPtr<UEdGraph> Graph;
        TWeakPtr<FMeasurementCache> Cache;
        TArray<FPendingNode> Nodes;
        TSet<FGuid> Selection;
        FLayoutSettings Settings;
        EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90;
        TUniquePtr<FGraphCaptureJob> Capture;
        TUniquePtr<FFormatJob> Job;
        SGraphEditor::EPinVisibility Visibility = SGraphEditor::Pin_Show;
        float Scale = 0;
        uint64 Revision = 0;
        uint64 LastAttemptFrame = 0;
        uint64 Generation = 0;
        double StartedAt = 0;
        int32 AttemptsMade = 0;
    };
    void ExecuteFocused();
    void ExecutePanel(TWeakPtr<SGraphPanel> WeakPanel);
    bool IsPendingCurrent(const FPendingFormat& Request, FSlateApplication& SlateApp, bool bValidateNodes = true) const;
    void ContinueRequest(FPendingFormat Request, double Deadline);
    void CancelPending();
    void ShowProgress();
    void CloseProgress();
    void BuildMenu(FMenuBuilder& Menu);
    void InvalidateMeasurements();
    void OnInvalidateWidgets(bool bClearResources);
    void OnPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
    void OnFontResourcesReleased(const FSlateFontCache& FontCache);
    TSharedRef<FExtender> ExtendMenu(const TSharedRef<FUICommandList> Commands, const UEdGraph* Graph,
        const UEdGraphNode* Node, const UEdGraphPin* Pin, bool bReadOnly);

    TSharedPtr<FUICommandList> CommandList;
    TWeakPtr<SGraphPanel> CurrentPanel;
    FDelegateHandle MenuHandle;
    FDelegateHandle InvalidateWidgetsHandle;
    FDelegateHandle PropertyChangedHandle;
    FDelegateHandle FontResourcesHandle;
    TWeakPtr<FSlateFontCache> FontCache;
    TArray<TWeakPtr<SGraphPanel>> CachedPanels;
    TOptional<FPendingFormat> Pending;
    TWeakPtr<SNotificationItem> Progress;
    uint64 RequestGeneration = 0;
};
}
