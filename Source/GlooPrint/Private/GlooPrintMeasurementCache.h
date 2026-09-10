// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintMeasurement.h"
#include "Types/ISlateMetaData.h"

class UEdGraphNode;
struct FEdGraphEditAction;

namespace GlooPrint
{
struct FLayoutGraph;
struct FLayoutResult;

class FMeasurementCache final : public ISlateMetaData, public TSharedFromThis<FMeasurementCache>
{
public:
    SLATE_METADATA_TYPE(FMeasurementCache, ISlateMetaData)
    virtual ~FMeasurementCache();

    void Begin(UEdGraph* Graph, float LayoutScale, SGraphEditor::EPinVisibility PinVisibility);
    void Invalidate(bool bContextChanged = true);
    const FMeasuredNode* Find(const UEdGraphNode& Node, const TArray<uint8>& State);
    void Store(const UEdGraphNode& Node, TArray<uint8> State, const FMeasuredNode& Measurement);
    int32 GetEntryCount() const { return Entries.Num(); }
    int32 GetHits() const { return Hits; }
    int32 GetMisses() const { return Misses; }
    uint64 GetRevision() const { return Revision; }

    struct FLayoutReuse
    {
        struct FNode
        {
            TWeakObjectPtr<UEdGraphNode> Node;
            TArray<uint8> InvariantState;
            FMeasuredNode Geometry;
        };
        TWeakObjectPtr<UEdGraph> Graph;
        TArray<FNode> Nodes;
        uint64 ContextRevision = 0;
        int32 NodeCount = 0;
    };
    FLayoutReuse PrepareLayoutReuse(const FLayoutGraph& Snapshot, const FLayoutResult& Layout) const;
    void RestoreLayoutReuse(FLayoutReuse Reuse);

private:
    struct FEntry
    {
        TWeakObjectPtr<UEdGraphNode> Node;
        TArray<uint8> State;
        FMeasuredNode Geometry;
    };
    void OnGraphChanged(const FEdGraphEditAction& Action);

    TMap<FGuid, FEntry> Entries;
    TWeakObjectPtr<UEdGraph> Graph;
    FDelegateHandle GraphChangedHandle;
    float Scale = 0;
    SGraphEditor::EPinVisibility Visibility = SGraphEditor::Pin_Show;
    uint16 TextRevision = 0;
    const void* StyleIdentity = nullptr;
    int32 Hits = 0;
    int32 Misses = 0;
    uint64 Revision = 0;
    uint64 ContextRevision = 0;
};

TArray<uint8> CaptureMeasurementState(UEdGraphNode& Node);
}
