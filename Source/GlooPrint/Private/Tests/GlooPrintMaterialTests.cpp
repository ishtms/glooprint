// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintMaterialTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "ConnectionDrawingPolicy.h"
#include "NodeFactory.h"
#include "Rendering/DrawElementTypes.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "SGraphPanel.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraphNode_Knot.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/UnrealType.h"

namespace GlooPrint::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAdvancedPinsTest, "GlooPrint.Materials.AdvancedPinVisibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialAdvancedPinsTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    auto* Position = Fixture.Expression<UMaterialExpressionWorldPosition>(500, 700);
    Fixture.Rebuild();
    auto* Node = Position->GraphNode.Get(); Node->AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
    const auto Cache = MakeShared<FMeasurementCache>(); FMeasurementOptions Options; Options.Cache = &Cache.Get();
    FLayoutGraph Snapshot; FString Reason;
    if (!TestTrue(TEXT("World Position with collapsed advanced controls measures"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options)))
    { AddError(Reason); return false; }
    int32 Advanced = 0;
    for (const auto* Pin : Node->Pins) if (Pin->bAdvancedView)
    {
        ++Advanced;
        TestTrue(TEXT("Unconnected advanced material pin is hidden"), IsMaterialPinHidden(*Pin, SGraphEditor::Pin_Show));
        const auto* Measured = Snapshot.Pins.FindByPredicate([&](const auto& P) { return P.Id == Pin->PinId; });
        TestTrue(TEXT("Collapsed advanced pin has no visual attachment"), Measured && !Measured->Offset.IsSet());
    }
    TestTrue(TEXT("World Position supplies a real advanced pin fixture"), Advanced > 0);
    Node->AdvancedPinDisplay = ENodeAdvancedPins::Shown;
    if (!TestTrue(TEXT("Expanded advanced material controls measure"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options)))
    { AddError(Reason); return false; }
    TestEqual(TEXT("Expanding advanced controls invalidates only that widget"), Cache->GetMisses(), 1);
    for (const auto* Pin : Node->Pins) if (Pin->bAdvancedView)
    {
        const auto* Measured = Snapshot.Pins.FindByPredicate([&](const auto& P) { return P.Id == Pin->PinId; });
        TestTrue(TEXT("Expanded advanced material pin has measured geometry"), Measured && Measured->Offset.IsSet());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFactoryOwnershipTest, "GlooPrint.Materials.FactoryOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialFactoryOwnershipTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    const auto Panel = SNew(SGraphPanel).GraphObj(Fixture.Graph).IsEditable(true);
    const auto Window = SNew(SWindow).ClientSize(FVector2f(800, 600))[Panel];
    FSlateApplication::Get().AddWindow(Window);
    const auto Owner = MakeShared<FWireDrawing>();
    Owner->InitializeMaterialPanels();
    const auto Foreign = MakeShared<FGraphNodeFactory>();
    Panel->SetNodeFactory(Foreign);
    const int32 References = Foreign.GetSharedReferenceCount();
    Owner->Shutdown();
    TestEqual(TEXT("Shutdown retains a subsequently installed external factory"), Foreign.GetSharedReferenceCount(), References);
    FSlateApplication::Get().RequestDestroyWindow(Window);
    return true;
}

namespace
{
class FMaterialInvariantWriter final : public FObjectWriter
{
public:
    explicit FMaterialInvariantWriter(TArray<uint8>& Bytes, bool bInIncludeLayout = false) : FObjectWriter(Bytes), bIncludeLayout(bInIncludeLayout)
    { ArNoDelta = true; ArPortFlags |= PPF_DuplicateVerbatim; }
    virtual bool ShouldSkipProperty(const FProperty* Property) const override
    {
        const FName Name = Property->GetFName();
        // Native undo/redo invokes expression preview refresh. Compare persistent
        // state there; the direct-format invariant also checks transient flags.
        if (bIncludeLayout && Property->HasAnyPropertyFlags(CPF_Transient)) { return true; }
        if (!bIncludeLayout && Property->GetOwnerStruct() == UMaterialExpression::StaticClass() &&
            (Name == TEXT("MaterialExpressionEditorX") || Name == TEXT("MaterialExpressionEditorY"))) { return true; }
        if (!bIncludeLayout && Property->GetOwnerStruct() == UMaterialExpressionComment::StaticClass() &&
            (Name == TEXT("SizeX") || Name == TEXT("SizeY"))) { return true; }
        return FObjectWriter::ShouldSkipProperty(Property);
    }
private:
    bool bIncludeLayout;
};

TArray<uint8> ExpressionState(UMaterialGraph& Graph, bool bIncludeLayout)
{
    TArray<uint8> Bytes;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        if (auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(*Node)))
        {
            TArray<uint8> Part;
            FMaterialInvariantWriter Writer(Part, bIncludeLayout); Expression->Serialize(Writer);
            Bytes.Append(Part);
        }
    }
    return Bytes;
}
TMap<FString, FString> ExpressionProperties(UMaterialGraph& Graph)
{
    TMap<FString, FString> Values;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        if (auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(*Node)))
        {
            for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
            {
                FString Value;
                It->ExportText_InContainer(0, Value, Expression, Expression, Expression, PPF_None);
                Values.Add(Expression->GetName() + TEXT(".") + It->GetName(), MoveTemp(Value));
            }
        }
    }
    return Values;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFamilyTest, "GlooPrint.Materials.CaptureLayoutUndo.Families",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialFamilyTest::RunTest(const FString& Parameters)
{
    for (int32 Family = 0; Family < 4; ++Family)
    {
        FMaterialFixture Fixture(Family);
        if (Family)
        {
            auto* Second = Fixture.Expression<UMaterialExpressionFunctionOutput>(-1600, 100);
            Second->SortPriority = -10; Second->A.Connect(0, Fixture.Constant); Fixture.Rebuild();
        }
        FString Reason;
        FLayoutGraph Snapshot;
        if (!TestTrue(TEXT("Native material-family widgets capture"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason)))
        {
            AddError(Reason); continue;
        }
        auto* Anchor = Family ? Fixture.Graph->Nodes.FindByPredicate([](const auto& Node)
        {
            const auto Priority = GetDefaultAnchorPriority(*Node); return Priority.IsSet() && Priority.GetValue() == -10;
        }) : nullptr;
        TestEqual(TEXT("Default anchor is material result or first function output"), Snapshot.Nodes[Snapshot.Anchor].Geometry.Id,
            Family ? (*Anchor)->NodeGuid : Fixture.Graph->RootNode->NodeGuid);
        const auto Before = SerializeTransactionValues(*Fixture.Graph);
        const auto BeforeExpressions = ExpressionState(*Fixture.Graph, true);
        const auto Invariants = ExpressionState(*Fixture.Graph, false);
        FLayoutResult Layout;
        if (!TestTrue(TEXT("Pure material layout computes"), ComputeLayout(Snapshot, {}, Layout, Reason))) { AddError(Reason); continue; }
        int32 Changed = 0; const int32 UndoCount = GEditor->Trans->GetQueueLength();
        if (!TestTrue(TEXT("Native material layout applies"), ApplyLayout(Fixture.Graph, Snapshot, Layout, Changed, Reason))) { AddError(Reason); continue; }
        TestTrue(TEXT("Unformatted fixture moved"), Changed > 0);
        TestEqual(TEXT("Single transaction"), GEditor->Trans->GetQueueLength(), UndoCount + 1);
        TestTrue(TEXT("Expression semantics unchanged"), Invariants == ExpressionState(*Fixture.Graph, false));
        for (UEdGraphNode* Node : Fixture.Graph->Nodes)
        {
            if (auto* Expression = Cast<UMaterialExpression>(GetLayoutBackingObject(*Node)))
            {
                TestEqual(TEXT("Persistent expression X"), Expression->MaterialExpressionEditorX, Node->NodePosX);
                TestEqual(TEXT("Persistent expression Y"), Expression->MaterialExpressionEditorY, Node->NodePosY);
            }
        }
        const auto After = SerializeTransactionValues(*Fixture.Graph);
        const auto AfterExpressions = ExpressionState(*Fixture.Graph, true);
        const auto AfterProperties = ExpressionProperties(*Fixture.Graph);
        TestTrue(TEXT("Undo succeeds"), GEditor->UndoTransaction());
        TestTrue(TEXT("Undo restores graph"), Before == SerializeTransactionValues(*Fixture.Graph));
        TestTrue(TEXT("Undo restores backing expressions"), BeforeExpressions == ExpressionState(*Fixture.Graph, true));
        TestTrue(TEXT("Redo succeeds"), GEditor->RedoTransaction());
        TestTrue(TEXT("Redo restores graph"), After == SerializeTransactionValues(*Fixture.Graph));
        if (!TestTrue(TEXT("Redo restores backing expressions"), AfterExpressions == ExpressionState(*Fixture.Graph, true)))
        {
            for (const auto& Pair : ExpressionProperties(*Fixture.Graph))
            {
                const auto* Expected = AfterProperties.Find(Pair.Key);
                if (!Expected || *Expected != Pair.Value) { AddInfo(Pair.Key + TEXT(": expected ") + (Expected ? *Expected : TEXT("<missing>")) + TEXT("; actual ") + Pair.Value); }
            }
        }
        FLayoutGraph Again;
        if (TestTrue(TEXT("Recapture after layout"), CaptureGraph(Fixture.Graph, 1, {}, Again, Reason)) &&
            TestTrue(TEXT("Recompute after layout"), ComputeLayout(Again, {}, Layout, Reason)))
        {
            const int32 Queue = GEditor->Trans->GetQueueLength();
            TestTrue(TEXT("Repeat apply"), ApplyLayout(Fixture.Graph, Again, Layout, Changed, Reason));
            TestEqual(TEXT("Repeat is a no-op"), Changed, 0);
            TestEqual(TEXT("No-op adds no undo"), GEditor->Trans->GetQueueLength(), Queue);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialHiddenPinsTest, "GlooPrint.Materials.HiddenConnectionsAndReroutes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialHiddenPinsTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    Fixture.Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Fixture.Constant);
    Fixture.Material->bUseMaterialAttributes = true;
    auto* Attributes = Fixture.Expression<UMaterialExpressionMakeMaterialAttributes>(600, -400);
    Fixture.Material->GetExpressionInputForProperty(MP_MaterialAttributes)->Connect(0, Attributes);
    auto* Knot = Fixture.Expression<UMaterialExpressionReroute>(900, -400); Knot->Input.Connect(0, Fixture.Constant);
    auto* Declaration = Fixture.Expression<UMaterialExpressionNamedRerouteDeclaration>(300, 1200); Declaration->Input.Connect(0, Knot);
    auto* Usage = Fixture.Expression<UMaterialExpressionNamedRerouteUsage>(700, 1500); Usage->Declaration = Declaration;
    Fixture.Rebuild();
    FLayoutGraph Snapshot; FString Reason;
    if (!TestTrue(TEXT("Hidden material properties do not reject capture"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason))) { AddError(Reason); return false; }
    TestTrue(TEXT("Full topology retains invisible connections"), Snapshot.Connections.Num() > Snapshot.Edges.Num());
    TestTrue(TEXT("Native material knot recognized"), Snapshot.Nodes.ContainsByPredicate([Knot](const auto& Node)
    {
        return Node.Geometry.Id == Knot->GraphNode->NodeGuid && Node.bReroute;
    }));
    const auto Invariants = ExpressionState(*Fixture.Graph, false);
    FLayoutResult Layout; int32 Changed = 0;
    if (TestTrue(TEXT("Hidden links permit formatting"), ComputeLayout(Snapshot, {}, Layout, Reason)) &&
        TestTrue(TEXT("Hidden links pass apply validation"), ApplyLayout(Fixture.Graph, Snapshot, Layout, Changed, Reason)))
    {
        TestTrue(TEXT("Hidden links and named references stay intact"), Invariants == ExpressionState(*Fixture.Graph, false));
        TestTrue(TEXT("Named usage still references original declaration"), Usage->Declaration == Declaration);
        const int32 X = Fixture.Constant->MaterialExpressionEditorX, Y = Fixture.Constant->MaterialExpressionEditorY;
        Fixture.Rebuild();
        TestEqual(TEXT("Native rebuild preserves saved X"), Fixture.Constant->GraphNode->NodePosX, X);
        TestEqual(TEXT("Native rebuild preserves saved Y"), Fixture.Constant->GraphNode->NodePosY, Y);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialWireStyleTest, "GlooPrint.Materials.NativeWireAppearance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialWireStyleTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    auto* Settings = GetMutableDefault<UGlooPrintSettings>();
    TGuardValue<EGlooPrintWireStyle> RestoreStyle(Settings->WireStyle, EGlooPrintWireStyle::Native);
    const auto Window = SNew(SWindow);
    FSlateWindowElementList Elements(Window);
    const FSlateRect Clip(0, 0, 1200, 800);
    TUniquePtr<FConnectionDrawingPolicy> Native(FNodeFactory::CreateConnectionPolicy(Fixture.Graph->GetSchema(), 0, 1, 1, Clip, Elements, Fixture.Graph));
    auto Factory = MakeShared<FWireDrawing>();
    TestNull(TEXT("Native material style delegates to Unreal"), Factory->CreateConnectionPolicy(Fixture.Graph->GetSchema(), 0, 1, 1, Clip, Elements, Fixture.Graph));
    for (auto Style : {EGlooPrintWireStyle::Rounded90, EGlooPrintWireStyle::Diagonal45})
    {
        Settings->WireStyle = Style;
        TUniquePtr<FConnectionDrawingPolicy> Custom(Factory->CreateConnectionPolicy(Fixture.Graph->GetSchema(), 0, 1, 1, Clip, Elements, Fixture.Graph));
        if (!TestNotNull(TEXT("Material wire style has a drawing policy"), Custom.Get())) { continue; }
        for (UEdGraphNode* Node : Fixture.Graph->Nodes)
        {
            for (auto* Output : Node->Pins)
            {
                if (Output->Direction != EGPD_Output) { continue; }
                for (auto* Input : Output->LinkedTo)
                {
                    for (bool bHovered : {false, true})
                    {
                        TSet<FEdGraphPinReference> Hovered;
                        if (bHovered) { Hovered.Add(Output); }
                        Native->SetHoveredPins(Hovered, {}, 0); Custom->SetHoveredPins(Hovered, {}, 0);
                        FConnectionParams A, B; Native->DetermineWiringStyle(Output, Input, A); Custom->DetermineWiringStyle(Output, Input, B);
                        TestTrue(TEXT("Native material wire colors match"), A.WireColor.Equals(B.WireColor));
                        TestEqual(TEXT("Native material wire thickness matches"), A.WireThickness, B.WireThickness);
                        TestTrue(TEXT("Material wire identity is preserved"), B.AssociatedPin1 == Output && B.AssociatedPin2 == Input);
                        TestTrue(TEXT("Materials have no Blueprint execution bubbles"), A.bDrawBubbles == B.bDrawBubbles);
                    }
                }
            }
        }
    }
    Factory->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCompositeTest, "GlooPrint.Materials.CompositesCommentsSubstrate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialCompositeTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    auto* Composite = Fixture.Expression<UMaterialExpressionComposite>(900, -500);
    Composite->SubgraphName = TEXT("Material subgraph");
    Composite->InputExpressions = Fixture.Expression<UMaterialExpressionPinBase>(-600, 0);
    Composite->OutputExpressions = Fixture.Expression<UMaterialExpressionPinBase>(600, 0);
    Composite->InputExpressions->PinDirection = EGPD_Output;
    Composite->OutputExpressions->PinDirection = EGPD_Input;
    Composite->InputExpressions->SubgraphExpression = Composite;
    Composite->OutputExpressions->SubgraphExpression = Composite;
    auto* Inside = Fixture.Expression<UMaterialExpressionAdd>(100, 350); Inside->SubgraphExpression = Composite;
    auto* Nested = Fixture.Expression<UMaterialExpressionComposite>(400, 500); Nested->SubgraphExpression = Composite;
    Nested->SubgraphName = TEXT("Nested material subgraph");
    Nested->InputExpressions = Fixture.Expression<UMaterialExpressionPinBase>(-600, 0);
    Nested->OutputExpressions = Fixture.Expression<UMaterialExpressionPinBase>(600, 0);
    Nested->InputExpressions->PinDirection = EGPD_Output; Nested->InputExpressions->SubgraphExpression = Nested;
    Nested->OutputExpressions->PinDirection = EGPD_Input; Nested->OutputExpressions->SubgraphExpression = Nested;
    Fixture.Expression<UMaterialExpressionMultiply>(80, 240)->SubgraphExpression = Nested;
    auto* Comment = NewObject<UMaterialExpressionComment>(Fixture.Material.Get(), NAME_None, RF_Transactional);
    Comment->Text = TEXT("Native material comment with a wrapped title"); Comment->SizeX = 4200; Comment->SizeY = 3000;
    Comment->MaterialExpressionEditorX = -2000; Comment->MaterialExpressionEditorY = -1000;
    Fixture.Material->GetExpressionCollection().AddComment(Comment);
    if (Substrate::IsSubstrateEnabled())
    {
        auto* Slab = Fixture.Expression<UMaterialExpressionSubstrateSlabBSDF>(400, 800);
        Fixture.Material->GetExpressionInputForProperty(MP_FrontMaterial)->Connect(0, Slab);
    }
    Fixture.Rebuild();
    TestEqual(TEXT("Composite creates native subgraph"), Fixture.Graph->SubGraphs.Num(), 1);
    if (Fixture.Graph->SubGraphs.IsEmpty()) { return false; }
    auto* Subgraph = CastChecked<UMaterialGraph>(Fixture.Graph->SubGraphs[0]);
    TestEqual(TEXT("Nested composite creates a second graph level"), Subgraph->SubGraphs.Num(), 1);
    AddInfo(FString::Printf(TEXT("Substrate enabled: %s"), Substrate::IsSubstrateEnabled() ? TEXT("true") : TEXT("false")));
    const auto ParentBefore = SerializeTransactionValues(*Fixture.Graph);
    FFormatPlan Plan; FString Reason; int32 Changed = 0;
    if (TestTrue(TEXT("Opened composite subgraph formats"), PlanFormatGraph(Subgraph, 1, {}, Plan, Reason)))
    {
        TestTrue(TEXT("Opened subgraph layout applies"), ApplyLayout(Subgraph, Plan.Snapshot, Plan.Layout, Changed, Reason));
        TestTrue(TEXT("Formatting a subgraph leaves parent untouched"), ParentBefore == SerializeTransactionValues(*Fixture.Graph));
    }
    else { AddError(Reason); }
    if (!Subgraph->SubGraphs.IsEmpty())
    {
        const auto ChildState = SerializeTransactionValues(*Subgraph);
        auto* NestedGraph = CastChecked<UMaterialGraph>(Subgraph->SubGraphs[0]);
        if (TestTrue(TEXT("Opened nested material graph formats"), PlanFormatGraph(NestedGraph, 1, {}, Plan, Reason)))
        {
            TestTrue(TEXT("Nested layout applies"), ApplyLayout(NestedGraph, Plan.Snapshot, Plan.Layout, Changed, Reason));
            TestTrue(TEXT("Nested formatting leaves its parent untouched"), ChildState == SerializeTransactionValues(*Subgraph));
            TestTrue(TEXT("Nested formatting leaves the root untouched"), ParentBefore == SerializeTransactionValues(*Fixture.Graph));
        }
        else { AddError(Reason); }
    }
    const auto ChildBefore = SerializeTransactionValues(*Subgraph);
    if (TestTrue(TEXT("Material comments and Substrate measure and format"), PlanFormatGraph(Fixture.Graph, 1, {}, Plan, Reason)))
    {
        TestTrue(TEXT("Parent layout applies"), ApplyLayout(Fixture.Graph, Plan.Snapshot, Plan.Layout, Changed, Reason));
        TestTrue(TEXT("Parent formatting leaves child untouched"), ChildBefore == SerializeTransactionValues(*Subgraph));
        TestEqual(TEXT("Comment width persisted"), Comment->SizeX, Comment->GraphNode->NodeWidth);
        TestEqual(TEXT("Comment height persisted"), Comment->SizeY, Comment->GraphNode->NodeHeight);
    }
    else { AddError(Reason); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInvalidationTest, "GlooPrint.Materials.CachesAndStalePlans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInvalidationTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture;
    auto Cache = MakeShared<FMeasurementCache>();
    FMeasurementOptions Options; Options.Cache = &Cache.Get(); FString Reason; FLayoutGraph Snapshot;
    if (!TestTrue(TEXT("Cached material capture"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options))) { AddError(Reason); return false; }
    TestTrue(TEXT("Repeat material capture"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options));
    TestEqual(TEXT("Every unchanged material widget reused"), Cache->GetHits(), Fixture.Graph->Nodes.Num());
    FLayoutResult Layout; ComputeLayout(Snapshot, {}, Layout, Reason);
    Fixture.Constant->R = 0.75f;
    const auto Before = SerializeTransactionValues(*Fixture.Graph); int32 Changed = -1;
    TestFalse(TEXT("Expression edit invalidates a captured plan"), ApplyLayout(Fixture.Graph, Snapshot, Layout, Changed, Reason));
    TestTrue(TEXT("Stale plan has no partial changes"), Before == SerializeTransactionValues(*Fixture.Graph));
    Cache->Invalidate(false); Cache->Invalidate(false); // Coalesced edit notifications retain unchanged geometry.
    TestTrue(TEXT("Expression-backed node remains measurable"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options));
    TestEqual(TEXT("Only changed expression is remeasured"), Cache->GetMisses(), 1);
    Fixture.Graph->bEditable = false;
    TestFalse(TEXT("Read-only materials reject format"), CanFormatGraph(Fixture.Graph, Reason)); Fixture.Graph->bEditable = true;
    TestFalse(TEXT("Unrelated material is not relevant"), IsObjectRelevantToGraph(NewObject<UMaterial>(), Fixture.Graph));
    TestTrue(TEXT("Expression edits are relevant"), IsObjectRelevantToGraph(Fixture.Constant, Fixture.Graph));
    auto* Pin = Fixture.Constant->GraphNode->Pins.Last(); Pin->LinkedTo.Add(nullptr);
    TestFalse(TEXT("Malformed links fail before native measurement"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason)); Pin->LinkedTo.Pop();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInterfaceTest, "GlooPrint.Materials.FunctionInterfaceAndManyPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInterfaceTest::RunTest(const FString& Parameters)
{
    FMaterialFixture Fixture(0, 40, nullptr, true);
    const auto Cache = MakeShared<FMeasurementCache>();
    FMeasurementOptions Options; Options.Cache = &Cache.Get(); FLayoutGraph Snapshot; FString Reason;
    if (!TestTrue(TEXT("Mixed material widgets measure"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options))) { AddError(Reason); return false; }
    auto* Input = CastChecked<UMaterialExpressionFunctionInput>(Fixture.CalledFunction->GetExpressions()[0]);
    TestTrue(TEXT("Referenced function expressions affect their caller graph"), IsObjectRelevantToGraph(Input, Fixture.Graph));
    Input->InputName = TEXT("Renamed function input"); Cache->Invalidate(false);
    if (!TestTrue(TEXT("Changed function interface remeasures"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason, Options))) { AddError(Reason); return false; }
    int32 Calls = 0;
    for (UEdGraphNode* Node : Fixture.Graph->Nodes)
    {
        const auto* Expression = Cast<UMaterialGraphNode>(Node);
        Calls += Expression && Expression->MaterialExpression->IsA<UMaterialExpressionMaterialFunctionCall>();
    }
    TestEqual(TEXT("Only nodes referencing the changed interface miss the cache"), Cache->GetMisses(), Calls);
    const auto Invariants = ExpressionState(*Fixture.Graph, false);
    FFormatPlan Plan;
    if (!TestTrue(TEXT("Mixed previews, custom many-pin expressions and comments format"), PlanFormatGraph(Fixture.Graph, 1, {}, Plan, Reason, Options))) { AddError(Reason); return false; }
    int32 Changed = 0;
    TestTrue(TEXT("Mixed material layout applies"), ApplyLayout(Fixture.Graph, Plan.Snapshot, Plan.Layout, Changed, Reason));
    TestTrue(TEXT("Function references and custom expression code remain unchanged"), Invariants == ExpressionState(*Fixture.Graph, false));
    return true;
}
}
#endif
