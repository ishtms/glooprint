// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "ConnectionDrawingPolicy.h"

class UMaterialGraph;
class UMaterialGraphNode_Knot;

namespace GlooPrint
{
// MaterialEditor's native policy is not exported. Keep material presentation here,
// using public APIs, and share route painting with the Blueprint policy.
class FMaterialDrawingPolicy : public FConnectionDrawingPolicy
{
public:
    FMaterialDrawingPolicy(int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
        FSlateWindowElementList& Elements, UEdGraph* Graph);
    virtual void Draw(TMap<TSharedRef<SWidget>, FArrangedWidget>& Geometries, FArrangedChildren& Nodes) override;
    virtual void DetermineWiringStyle(UEdGraphPin* Output, UEdGraphPin* Input, FConnectionParams& Params) override;
    virtual TSharedPtr<IToolTip> GetConnectionToolTip(const SGraphPanel& Panel, const FGraphSplineOverlapResult& Overlap) const override;
private:
    bool PinCenter(UEdGraphPin* Pin, FVector2f& Position) const;
    bool ReverseKnot(UMaterialGraphNode_Knot& Knot);
    UMaterialGraph* MaterialGraph;
    TMap<UMaterialGraphNode_Knot*, bool> ReversedKnots;
};
}
