// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintGraphAdapter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialGraphNode_Knot.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"

namespace GlooPrint
{
namespace
{
// The Voxel Plugin graph editor's classes are private to its module, so they are matched by name, which also
// keeps GlooPrint free of a Voxel dependency.
const FName VoxelEditorPackage(TEXT("/Script/VoxelGraphEditor"));
const FName VoxelSchema(TEXT("VoxelGraphSchema"));
const FName VoxelKnot(TEXT("VoxelGraphNode_Knot"));
const FName VoxelFunctionOutput(TEXT("VoxelGraphNode_FunctionOutput"));

bool IsVoxelEditorClass(const UClass* Class, FName Name)
{
    return Class->GetFName() == Name && Class->GetOuter()->GetFName() == VoxelEditorPackage;
}
}

EGraphFamily GetGraphFamily(const UEdGraph* Graph)
{
    if (!IsValid(Graph) || !Graph->GetSchema()) { return EGraphFamily::Unsupported; }
    if (Graph->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass()) { return EGraphFamily::Blueprint; }
    if (Graph->GetSchema()->GetClass() == UMaterialGraphSchema::StaticClass() && Graph->IsA<UMaterialGraph>())
    {
        return EGraphFamily::Material;
    }
    if (IsVoxelEditorClass(Graph->GetSchema()->GetClass(), VoxelSchema)) { return EGraphFamily::Voxel; }
    return EGraphFamily::Unsupported;
}

bool IsDataflowFamily(EGraphFamily Family)
{
    return Family == EGraphFamily::Material || Family == EGraphFamily::Voxel;
}

bool ValidateGraphOwner(const UEdGraph* Graph, FString& Reason)
{
    switch (GetGraphFamily(Graph))
    {
    case EGraphFamily::Blueprint:
        if (const auto* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
            IsValid(Blueprint) && !Blueprint->bBeingCompiled && !Blueprint->bIsRegeneratingOnLoad) { return true; }
        Reason = TEXT("The Blueprint is unavailable or is being compiled/reconstructed."); return false;
    case EGraphFamily::Material:
    {
        const auto* MaterialGraph = CastChecked<UMaterialGraph>(Graph);
        if (IsValid(MaterialGraph->Material) &&
            (!MaterialGraph->MaterialFunction || IsValid(MaterialGraph->MaterialFunction))) { return true; }
        Reason = TEXT("The material graph owner is unavailable or is being reconstructed."); return false;
    }
    case EGraphFamily::Voxel:
        // The outer is the graph's terminal graph (main graph or function) inside the voxel graph asset.
        if (IsValid(Graph->GetOuter())) { return true; }
        Reason = TEXT("The voxel graph owner is unavailable."); return false;
    default:
        Reason = TEXT("Format Graph supports ordinary Blueprint, Material and Voxel editor graphs."); return false;
    }
}

bool IsGraphReadOnly(const UEdGraph* Graph)
{
    return !Graph || !Graph->bEditable ||
        (GetGraphFamily(Graph) == EGraphFamily::Blueprint && FBlueprintEditorUtils::IsGraphReadOnly(const_cast<UEdGraph*>(Graph)));
}

ELinkKind GetLinkKind(const UEdGraphPin& Pin)
{
    const EGraphFamily Family = GetGraphFamily(Pin.GetOwningNode()->GetGraph());
    if (Family == EGraphFamily::Material)
    {
        return Pin.PinType.PinCategory == UMaterialGraphSchema::PC_Exec ? ELinkKind::Execution : ELinkKind::Data;
    }
    // Voxel graphs have no execution or delegate pins; their pin categories are voxel types.
    if (Family == EGraphFamily::Voxel) { return ELinkKind::Data; }
    return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? ELinkKind::Execution :
        (Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate || Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate
            ? ELinkKind::Delegate : ELinkKind::Data);
}

bool IsRerouteNode(const UEdGraphNode& Node)
{
    return Node.IsA<UK2Node_Knot>() || Node.IsA<UMaterialGraphNode_Knot>() || IsVoxelEditorClass(Node.GetClass(), VoxelKnot);
}

bool IsMaterialPinHidden(const UEdGraphPin& Pin, SGraphEditor::EPinVisibility Visibility)
{
    const auto* Node = Pin.GetOwningNode();
    const auto* Graph = CastChecked<UMaterialGraph>(Node->GetGraph());
    const bool bUnconnectedHidden = Visibility == SGraphEditor::Pin_HideNoConnection && Pin.LinkedTo.IsEmpty();
    // Native material widgets use SGraphPin::IsPinVisibleAsAdvanced. Connected
    // advanced pins remain visible even while the node's advanced section is closed.
    const bool bAdvancedHidden = Pin.bAdvancedView && Pin.LinkedTo.IsEmpty() && Node->AdvancedPinDisplay == ENodeAdvancedPins::Hidden;
    const bool bRoot = Node->IsA<UMaterialGraphNode_Root>();
    bool bAttributes = false;
    if (const auto* ExpressionNode = Cast<UMaterialGraphNode>(Node); ExpressionNode && ExpressionNode->MaterialExpression)
    {
        bAttributes = !Graph->MaterialFunction &&
            ((ExpressionNode->MaterialExpression->IsA<UMaterialExpressionMakeMaterialAttributes>() && Pin.Direction == EGPD_Input) ||
             (ExpressionNode->MaterialExpression->IsA<UMaterialExpressionBreakMaterialAttributes>() && Pin.Direction == EGPD_Output));
    }
    if ((bRoot || bAttributes) && Pin.PinType.PinCategory != UMaterialGraphSchema::PC_Exec &&
        Graph->MaterialInputs.IsValidIndex(Pin.SourceIndex) &&
        !Graph->MaterialInputs[Pin.SourceIndex].IsVisiblePin(Graph->Material, bAttributes)) { return true; }
    return bUnconnectedHidden || bAdvancedHidden || (!bRoot && Pin.bHidden);
}

TOptional<int32> GetDefaultAnchorPriority(const UEdGraphNode& Node)
{
    if (GetGraphFamily(Node.GetGraph()) == EGraphFamily::Voxel)
    {
        // A main graph's output node is the one node Voxel refuses to delete (FVoxelOutputNode::CanBeDeleted);
        // a function has output nodes instead.
        if (!Node.CanUserDeleteNode()) { return MIN_int32; }
        if (IsVoxelEditorClass(Node.GetClass(), VoxelFunctionOutput)) { return 0; }
        return {};
    }
    if (Node.IsA<UMaterialGraphNode_Root>()) { return MIN_int32; }
    if (const auto* ExpressionNode = Cast<UMaterialGraphNode>(&Node))
    {
        if (const auto* Output = Cast<UMaterialExpressionFunctionOutput>(ExpressionNode->MaterialExpression)) { return Output->SortPriority; }
    }
    return {};
}

UObject* GetLayoutBackingObject(const UEdGraphNode& Node)
{
    if (const auto* Expression = Cast<UMaterialGraphNode>(&Node)) { return Expression->MaterialExpression; }
    if (const auto* Comment = Cast<UMaterialGraphNode_Comment>(&Node)) { return Comment->MaterialExpressionComment; }
    if (const auto* Root = Cast<UMaterialGraphNode_Root>(&Node)) { return Root->Material; }
    return nullptr;
}

bool ValidateLayoutBackingObject(const UEdGraphNode& Node, FString& Reason)
{
    if (GetGraphFamily(Node.GetGraph()) != EGraphFamily::Material) { return true; }
    const auto* Graph = CastChecked<UMaterialGraph>(Node.GetGraph());
    const UObject* Backing = GetLayoutBackingObject(Node);
    if (!IsValid(Backing) || !Backing->HasAnyFlags(RF_Transactional))
    {
        Reason = TEXT("A material node has no valid transactional expression or layout owner."); return false;
    }
    if (const auto* Expression = Cast<UMaterialExpression>(Backing); Expression && Expression->GraphNode != &Node)
    {
        Reason = TEXT("A material expression was replaced or detached from its graph node."); return false;
    }
    bool bUsesMaterialInputs = Node.IsA<UMaterialGraphNode_Root>();
    if (const auto* Expression = Cast<UMaterialExpression>(Backing); Expression && !Graph->MaterialFunction && !Graph->MaterialInputs.IsEmpty())
    {
        bUsesMaterialInputs |= Expression->IsA<UMaterialExpressionMakeMaterialAttributes>() || Expression->IsA<UMaterialExpressionBreakMaterialAttributes>();
    }
    if (bUsesMaterialInputs)
    {
        for (const UEdGraphPin* Pin : Node.Pins)
        {
            if (Pin && Pin->PinType.PinCategory != UMaterialGraphSchema::PC_Exec &&
                ((Node.IsA<UMaterialGraphNode_Root>() && Pin->Direction == EGPD_Input) ||
                 (Backing->IsA<UMaterialExpressionMakeMaterialAttributes>() && Pin->Direction == EGPD_Input) ||
                 (Backing->IsA<UMaterialExpressionBreakMaterialAttributes>() && Pin->Direction == EGPD_Output)) &&
                !Graph->MaterialInputs.IsValidIndex(Pin->SourceIndex))
            {
                Reason = TEXT("A material property pin is being reconstructed or has an invalid source index."); return false;
            }
        }
    }
    return true;
}

void ApplyBackingLayout(UEdGraphNode& Node)
{
    UObject* Backing = GetLayoutBackingObject(Node);
    if (!Backing) { return; }
    // UMaterialExpression::Modify requests preview regeneration. Layout changes
    // need only UObject's transaction/package bookkeeping, without that effect.
    Backing->UObject::Modify();
    if (auto* Expression = Cast<UMaterialExpression>(Backing))
    {
        Expression->MaterialExpressionEditorX = Node.NodePosX;
        Expression->MaterialExpressionEditorY = Node.NodePosY;
        if (auto* Comment = Cast<UMaterialExpressionComment>(Expression))
        {
            Comment->SizeX = Node.NodeWidth; Comment->SizeY = Node.NodeHeight;
        }
    }
    else if (auto* Material = Cast<UMaterial>(Backing))
    {
        Material->EditorX = Node.NodePosX; Material->EditorY = Node.NodePosY;
    }
}

void NotifyLayoutApplied(UEdGraph& Graph)
{
    if (auto* MaterialGraph = Cast<UMaterialGraph>(&Graph))
    {
        // Matches native movement: dirty the preview editor, without relinking or compiling shaders.
        MaterialGraph->MaterialDirtyDelegate.ExecuteIfBound();
    }
    Graph.NotifyGraphChanged();
}

bool IsObjectRelevantToGraph(const UObject* Object, const UEdGraph* Graph)
{
    if (!Object || !Graph) { return false; }
    if (Object == Graph || Object->IsIn(Graph) || Graph->IsIn(Object)) { return true; }
    if (const auto* Expression = Cast<UMaterialExpression>(Object);
        Expression && IsValid(Expression->GraphNode) && Expression->GraphNode->GetGraph() == Graph) { return true; }
    const auto* MaterialGraph = Cast<UMaterialGraph>(Graph);
    if (!MaterialGraph) { return false; }
    if (Object == MaterialGraph->Material || Object == MaterialGraph->MaterialFunction) { return true; }
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node)) { continue; }
        if (const auto* Backing = GetLayoutBackingObject(*Node); Backing && (Object == Backing || Object->IsIn(Backing))) { return true; }
        if (const auto* ExpressionNode = Cast<UMaterialGraphNode>(Node))
        {
            if (const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(ExpressionNode->MaterialExpression);
                Call && Call->MaterialFunction && (Object == Call->MaterialFunction || Object->IsIn(Call->MaterialFunction))) { return true; }
        }
    }
    return false;
}

void AppendMaterialMeasurementState(UEdGraphNode& Node, FArchive& Archive)
{
    if (GetGraphFamily(Node.GetGraph()) != EGraphFamily::Material) { return; }
    if (auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(Node))) { Expression->Serialize(Archive); }
    if (auto* ExpressionNode = Cast<UMaterialGraphNode>(&Node))
    {
        Archive << ExpressionNode->bIsErrorExpression << ExpressionNode->bIsPreviewExpression;
        if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(ExpressionNode->MaterialExpression))
        {
            if (Call->MaterialFunction) { Archive << Call->MaterialFunction->StateId; }
            for (const auto& Input : Call->FunctionInputs)
            {
                if (IsValid(Input.ExpressionInput)) { Input.ExpressionInput->Serialize(Archive); }
            }
            for (const auto& Output : Call->FunctionOutputs)
            {
                if (IsValid(Output.ExpressionOutput)) { Output.ExpressionOutput->Serialize(Archive); }
            }
        }
    }
    const auto* Graph = CastChecked<UMaterialGraph>(Node.GetGraph());
    const auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(Node));
    // Only property-pin widgets consult the material's input visibility. Doing
    // this for every arithmetic/function node repeats the same expensive material
    // property queries thousands of times without adding any geometry information.
    const bool bPropertyPins = Node.IsA<UMaterialGraphNode_Root>() || (Expression &&
        (Expression->IsA<UMaterialExpressionMakeMaterialAttributes>() || Expression->IsA<UMaterialExpressionBreakMaterialAttributes>()));
    if (bPropertyPins) for (const FMaterialInputInfo& Input : Graph->MaterialInputs)
    {
        bool bRootVisible = Input.IsVisiblePin(Graph->Material);
        bool bAttributesVisible = Input.IsVisiblePin(Graph->Material, true);
        Archive << bRootVisible << bAttributesVisible;
    }
}
}
