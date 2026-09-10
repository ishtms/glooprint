// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintMeasurement.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "KismetNodes/SGraphNodeK2Default.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

class SWindow;
class SGraphPanel;

namespace GlooPrint::Tests
{
struct FFixture
{
    TStrongObjectPtr<UBlueprint> Blueprint;
    UEdGraph* Graph = nullptr;
    UK2Node_CallFunction* Print = nullptr;
    UK2Node_IfThenElse* Branch = nullptr;

    explicit FFixture(UClass* ParentClass = UObject::StaticClass(), bool bPopulate = true, UPackage* InPackage = nullptr)
    {
        UPackage* Package = InPackage ? InPackage : GetTransientPackage();
        Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(ParentClass, Package,
            MakeUniqueObjectName(Package, UBlueprint::StaticClass(), TEXT("GlooPrintGeometry"), EUniqueObjectNameOptions::UniversallyUnique), BPTYPE_Normal));
        Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("GeometryFixture"),
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(), Graph);
        if (!bPopulate) { return; }
        Branch = Add<UK2Node_IfThenElse>(FVector2f(0, 0));
        UK2Node_ExecutionSequence* Sequence = Add<UK2Node_ExecutionSequence>(FVector2f(320, 0));
        Print = NewObject<UK2Node_CallFunction>(Graph);
        Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
        Initialize(*Print, FVector2f(640, 0));
        Print->PostPlacedNewNode();
        Print->AdvancedPinDisplay = ENodeAdvancedPins::Shown;
        Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("Measured native inline default");
        Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        Sequence->GetThenPinGivenIndex(0)->MakeLinkTo(Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        Add<UK2Node_Knot>(FVector2f(320, 350));
        UEdGraphNode_Comment* Comment = Add<UEdGraphNode_Comment>(FVector2f(-30, -90));
        Comment->NodeWidth = 1100;
        Comment->NodeHeight = 550;
        Comment->NodeComment = TEXT("GlooPrint geometry prototype — native Blueprint widgets");
        Branch->NodeComment = TEXT("A visible comment bubble above the branch");
        Branch->bCommentBubbleVisible = true;
        Branch->bCommentBubblePinned = true;
    }

    void Initialize(UEdGraphNode& Node, const FVector2f& Position)
    {
        Node.SetFlags(RF_Transactional);
        Node.CreateNewGuid();
        Node.SetPosition(Position);
        Graph->AddNode(&Node, false, false);
        Node.AllocateDefaultPins();
    }

    template<typename T>
    T* Add(const FVector2f& Position)
    {
        T* Node = NewObject<T>(Graph);
        Initialize(*Node, Position);
        return Node;
    }
};

struct FGeometryReadiness
{
    uint64 ReadyFrame = MAX_uint64;
    int32 Creations = 0;
};

class SDelayedK2Node final : public SGraphNodeK2Default
{
public:
    SLATE_BEGIN_ARGS(SDelayedK2Node) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, UK2Node* Node, TSharedRef<FGeometryReadiness> InReadiness)
    {
        Readiness = InReadiness;
        SGraphNodeK2Default::Construct(SGraphNodeK2Default::FArguments(), Node);
    }
    virtual FVector2D ComputeDesiredSize(float Scale) const override
    {
        return GFrameCounter < Readiness->ReadyFrame ? FVector2D::ZeroVector : SGraphNodeK2Default::ComputeDesiredSize(Scale);
    }
private:
    TSharedPtr<FGeometryReadiness> Readiness;
};

struct FDelayedNodeFactory final : public FGraphPanelNodeFactory
{
    TWeakObjectPtr<UK2Node> Target;
    TSharedRef<FGeometryReadiness> Readiness = MakeShared<FGeometryReadiness>();
    virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override
    {
        if (Node != Target.Get()) { return nullptr; }
        ++Readiness->Creations;
        return SNew(SDelayedK2Node, Target.Get(), Readiness);
    }
};

TArray<uint8> SerializeNodes(const UEdGraph& Graph, TMap<FGuid, TArray<uint8>>* PerNode = nullptr);
TArray<uint8> SerializeTransactionValues(const UEdGraph& Graph);
TMap<FString, FString> DescribeNodes(const UEdGraph& Graph);
void ReportNodeDifferences(FAutomationTestBase& Test, const TMap<FString, FString>& Before, const UEdGraph& Graph,
    const TMap<FGuid, TArray<uint8>>* BeforeBytes = nullptr);
const FMeasuredNode& Find(const FGraphMeasurement& Measurement, const FGuid& Id);
void Compare(FAutomationTestBase& Test, const FGraphMeasurement& A, const FGraphMeasurement& B);
void MoveMouseOverGraph(FAutomationTestBase& Test, const TSharedRef<SWindow>& Window, SGraphPanel& Panel, FVector2f ScreenPosition);
}
#endif
