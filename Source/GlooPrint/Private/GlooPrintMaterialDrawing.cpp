// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintMaterialDrawing.h"

#include "MaterialEditor/SGraphSubstrateMaterial.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialGraph/MaterialShaderValueTypeObject.h"
#include "MaterialGraphNode_Knot.h"
#include "SGraphPanel.h"
#include "Widgets/SToolTip.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace GlooPrint
{
FMaterialDrawingPolicy::FMaterialDrawingPolicy(int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
    FSlateWindowElementList& Elements, UEdGraph* Graph)
    : FConnectionDrawingPolicy(BackLayer, FrontLayer, Zoom, Clip, Elements), MaterialGraph(CastChecked<UMaterialGraph>(Graph))
{
    ArrowImage = nullptr; ArrowRadius = FVector2f::ZeroVector;
    HoverDeemphasisDarkFraction = 0.4f;
}

void FMaterialDrawingPolicy::Draw(TMap<TSharedRef<SWidget>, FArrangedWidget>& Geometries, FArrangedChildren& Nodes)
{
    ReversedKnots.Reset();
    FConnectionDrawingPolicy::Draw(Geometries, Nodes);
}

bool FMaterialDrawingPolicy::PinCenter(UEdGraphPin* Pin, FVector2f& Position) const
{
    if (const auto* Widget = PinToPinWidgetMap.Find(Pin); Widget && PinGeometries)
    {
        if (const auto* Geometry = PinGeometries->Find(Widget->ToSharedRef()))
        {
            Position = FGeometryHelper::CenterOf(Geometry->Geometry); return true;
        }
    }
    return false;
}

bool FMaterialDrawingPolicy::ReverseKnot(UMaterialGraphNode_Knot& Knot)
{
    if (const bool* Reversed = ReversedKnots.Find(&Knot)) { return *Reversed; }
    const auto Average = [this](UEdGraphPin* Pin, FVector2f& Position)
    {
        Position = FVector2f::ZeroVector; int32 Count = 0;
        if (!Pin) { return false; }
        for (auto* Linked : Pin->LinkedTo)
        {
            FVector2f Center;
            if (PinCenter(Linked, Center)) { Position += Center; ++Count; }
        }
        if (Count) { Position /= float(Count); }
        return Count > 0;
    };
    FVector2f Left, Right, Center;
    const bool bLeft = Average(Knot.GetInputPin(), Left), bRight = Average(Knot.GetOutputPin(), Right);
    const bool bCenter = PinCenter(Knot.GetOutputPin(), Center);
    const bool bReverse = bLeft && bRight ? Right.X < Left.X : bCenter && (bLeft ? Center.X < Left.X : bRight && Right.X < Center.X);
    ReversedKnots.Add(&Knot, bReverse); return bReverse;
}

void FMaterialDrawingPolicy::DetermineWiringStyle(UEdGraphPin* Output, UEdGraphPin* Input, FConnectionParams& Params)
{
    Params.AssociatedPin1 = Output; Params.AssociatedPin2 = Input;
    Params.WireColor = UMaterialGraphSchema::ActivePinColor;
    if (Substrate::IsSubstrateEnabled() && (FSubstrateWidget::HasOutputSubstrateType(Output) ||
        FSubstrateWidget::HasInputSubstrateType(Input) || FSubstrateWidget::HasInputSubstrateType(Output)))
    {
        Params.WireColor = FSubstrateWidget::GetConnectionColor();
    }
    bool bInactive = false, bExecution = false;
    for (auto* Pin : {Output, Input})
    {
        if (!Pin) { continue; }
        bInactive |= !MaterialGraph->IsInputActive(Pin);
        bExecution |= Pin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec;
        auto* Node = Pin->GetOwningNode();
        if (auto* Knot = Cast<UMaterialGraphNode_Knot>(Node))
        {
            if (ReverseKnot(*Knot))
            {
                if (Pin == Output) { Params.StartDirection = EGPD_Input; }
                else { Params.EndDirection = EGPD_Output; }
            }
        }
        else { bInactive |= !Node->IsNodeEnabled() || Node->IsDisplayAsDisabledForced() || Node->IsNodeUnrelated(); }
    }
    if (bInactive) { Params.WireColor = UMaterialGraphSchema::InactivePinColor; }
    else if (bExecution)
    {
        Params.WireColor = Settings->ExecutionPinTypeColor; Params.WireThickness = Settings->DefaultExecutionWireThickness;
    }
    else if (Input)
    {
        if (const auto* Type = Cast<UMaterialShaderValueTypeObject>(Input->PinType.PinSubCategoryObject.Get()))
        {
            Params.WireColor = UMaterialGraphSchema::GetColorForConnectionType(Type->ValueType);
        }
    }
    if (!HoveredPins.IsEmpty()) { ApplyHoverDeemphasis(Output, Input, Params.WireThickness, Params.WireColor); }
}

TSharedPtr<IToolTip> FMaterialDrawingPolicy::GetConnectionToolTip(const SGraphPanel& Panel, const FGraphSplineOverlapResult& Overlap) const
{
    TSharedPtr<SGraphPin> A, B; Overlap.GetPinWidgets(Panel, A, B);
    if (!A || !B) { return FConnectionDrawingPolicy::GetConnectionToolTip(Panel, Overlap); }
    const auto Describe = [](const TSharedPtr<SGraphPin>& Widget)
    {
        const auto* Pin = Widget->GetPinObj(); const auto* Node = Pin->GetOwningNode();
        FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString(); Title.RemoveFromStart(TEXT("Material Expression "));
        if (Node->GetCanRenameNode()) { Title += TEXT(" (") + Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString() + TEXT(")"); }
        return FText::Format(NSLOCTEXT("GlooPrint", "MaterialWirePin", "{0}\n{1}"), FText::FromString(Title),
            Pin->GetDisplayName().IsEmptyOrWhitespace() ? FText::FromName(Pin->PinName) : Pin->GetDisplayName());
    };
    return SNew(SToolTip)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 5)
        [SNew(STextBlock).Text(FText::Format(NSLOCTEXT("GlooPrint", "MaterialWireSource", "<< {0}"), Describe(A)))]
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 5, 0, 0)
        [SNew(STextBlock).Text(FText::Format(NSLOCTEXT("GlooPrint", "MaterialWireTarget", "{0} >>"), Describe(B)))]];
}
}
