// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintVoxelDrawing.h"

#include "EdGraph/EdGraph.h"
#include "NodeFactory.h"

namespace GlooPrint
{
namespace
{
bool bCreatingNative = false;
}

bool FVoxelDrawingPolicy::IsCreatingNative() { return bCreatingNative; }

FVoxelDrawingPolicy::FVoxelDrawingPolicy(int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
    FSlateWindowElementList& Elements, UEdGraph* Graph)
    : FConnectionDrawingPolicy(BackLayer, FrontLayer, Zoom, Clip, Elements)
{
    // As the native policy: voxel wires carry no arrow.
    ArrowImage = nullptr; ArrowRadius = FVector2f::ZeroVector;
    const TGuardValue<bool> Guard(bCreatingNative, true);
    Native.Reset(FNodeFactory::CreateConnectionPolicy(Graph->GetSchema(), BackLayer, FrontLayer, Zoom, Clip, Elements, Graph));
}

void FVoxelDrawingPolicy::Draw(TMap<TSharedRef<SWidget>, FArrangedWidget>& Geometries, FArrangedChildren& Nodes)
{
    // The panel hands its hover state to this policy only.
    Native->SetHoveredPins(HoveredPins, {}, LastHoverTimeEvent);
    FConnectionDrawingPolicy::Draw(Geometries, Nodes);
}

void FVoxelDrawingPolicy::DetermineWiringStyle(UEdGraphPin* Output, UEdGraphPin* Input, FConnectionParams& Params)
{
    Native->DetermineWiringStyle(Output, Input, Params);
}
}
