// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "ConnectionDrawingPolicy.h"

namespace GlooPrint
{
// The Voxel plugin's wire policy is private to its module, but it keeps no painting state, so wire style (pin-type
// colors, orphaned pins, hover, the previewed pin's bubbles) is delegated to it; route painting is shared with the
// Blueprint and Material policies.
class FVoxelDrawingPolicy : public FConnectionDrawingPolicy
{
public:
    FVoxelDrawingPolicy(int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
        FSlateWindowElementList& Elements, UEdGraph* Graph);
    virtual void Draw(TMap<TSharedRef<SWidget>, FArrangedWidget>& Geometries, FArrangedChildren& Nodes) override;
    virtual void DetermineWiringStyle(UEdGraphPin* Output, UEdGraphPin* Input, FConnectionParams& Params) override;
    // True while the native policy is being created: GlooPrint's own factories must not answer then.
    static bool IsCreatingNative();
private:
    TUniquePtr<FConnectionDrawingPolicy> Native;
};
}
