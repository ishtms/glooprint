// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintGraph.h"

class FArchive;
class UEdGraphNode;
class UEdGraphPin;

namespace GlooPrint
{
// Voxel is the Voxel Plugin 2 graph editor (voxel graphs and function libraries).
enum class EGraphFamily : uint8 { Unsupported, Blueprint, Material, Voxel };

EGraphFamily GetGraphFamily(const UEdGraph* Graph);
// Material and Voxel graphs: links may end at pins their widgets hide, and an output node anchors the layout.
bool IsDataflowFamily(EGraphFamily Family);
bool ValidateGraphOwner(const UEdGraph* Graph, FString& Reason);
bool IsGraphReadOnly(const UEdGraph* Graph);
ELinkKind GetLinkKind(const UEdGraphPin& Pin);
bool IsRerouteNode(const UEdGraphNode& Node);
bool IsMaterialPinHidden(const UEdGraphPin& Pin, SGraphEditor::EPinVisibility Visibility);
TOptional<int32> GetDefaultAnchorPriority(const UEdGraphNode& Node);
bool IsObjectRelevantToGraph(const UObject* Object, const UEdGraph* Graph);

// The persistent object represented by a material editor node (not necessarily its outer).
UObject* GetLayoutBackingObject(const UEdGraphNode& Node);
bool ValidateLayoutBackingObject(const UEdGraphNode& Node, FString& Reason);
void ApplyBackingLayout(UEdGraphNode& Node);
void NotifyLayoutApplied(UEdGraph& Graph);
void AppendMaterialMeasurementState(UEdGraphNode& Node, FArchive& Archive);
}
