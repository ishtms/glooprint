// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "GlooPrintMeasurement.h"
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Settings/EditorStyleSettings.h"

#include "Algo/Reverse.h"
#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "ImageUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetNodes/SGraphNodeK2Default.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "Widgets/IToolTip.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "SGraphPin.h"
#include "Serialization/ObjectWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace GlooPrint::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FColdMeasurementTest, "GlooPrint.Measurement.ColdAndReadOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FColdMeasurementTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    const TArray<uint8> Before = SerializeNodes(*Fixture.Graph);
    const bool DirtyBefore = Fixture.Graph->GetOutermost()->IsDirty();
    FGraphMeasurement First;
    FString Reason;
    if (!TestTrue(TEXT("Fresh native widgets measure"), MeasureGraph(Fixture.Graph, 1.0f, First, Reason)))
    {
        AddError(Reason);
        return false;
    }
    TestEqual(TEXT("Every node, including disconnected reroute and comment"), First.Nodes.Num(), Fixture.Graph->Nodes.Num());
    TestTrue(TEXT("Comment bubble extends above the body"), Find(First, Fixture.Branch->NodeGuid).VisualBounds.Min.Y < 0);
    TestTrue(TEXT("Measurement preserves serialized node and pin properties"), Before == SerializeNodes(*Fixture.Graph));
    TestEqual(TEXT("No dirty-state change"), Fixture.Graph->GetOutermost()->IsDirty(), DirtyBefore);

    Algo::Reverse(Fixture.Graph->Nodes);
    FGraphMeasurement Second;
    if (TestTrue(TEXT("Cold measurement after shuffled enumeration"), MeasureGraph(Fixture.Graph, 1.0f, Second, Reason)))
    {
        Compare(*this, First, Second);
    }
    else
    {
        AddError(Reason);
    }

    const float ExpandedHeight = Find(First, Fixture.Print->NodeGuid).BodySize.Y;
    Fixture.Print->AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
    FGraphMeasurement Collapsed;
    if (TestTrue(TEXT("Collapsed advanced pins remain measurable"), MeasureGraph(Fixture.Graph, 1.0f, Collapsed, Reason)))
    {
        TestTrue(TEXT("Advanced pin expansion changes measured body height"), Find(Collapsed, Fixture.Print->NodeGuid).BodySize.Y < ExpandedHeight);
    }
    else
    {
        AddError(Reason);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasurementFailureTest, "GlooPrint.Measurement.FailWithoutPartialResults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeasurementFailureTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    FGraphMeasurement Result;
    FString Reason;
    TestFalse(TEXT("Null graph rejected"), MeasureGraph(nullptr, 1.0f, Result, Reason));
    TestFalse(TEXT("Failure explains why"), Reason.IsEmpty());
    TestFalse(TEXT("Unavailable display scale rejected"), MeasureGraph(Fixture.Graph, 0.0f, Result, Reason));
    Fixture.Blueprint->bBeingCompiled = true;
    TestFalse(TEXT("Compilation rejected"), MeasureGraph(Fixture.Graph, 1.0f, Result, Reason));
    Fixture.Blueprint->bBeingCompiled = false;
    Fixture.Branch->Pins.Add(nullptr);
    TestFalse(TEXT("Missing pin rejected before native widget construction"), MeasureGraph(Fixture.Graph, 1.0f, Result, Reason));
    TestTrue(TEXT("Missing pin exposes no partial snapshot"), Result.Nodes.IsEmpty());
    Fixture.Branch->Pins.Pop();
    auto* TargetNode = Fixture.Add<UK2Node_ExecutionSequence>(FVector2f(1600, 0));
    auto* SourcePin = Fixture.Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else);
    auto* TargetPin = TargetNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
    SourcePin->LinkedTo.Add(TargetPin);
    TestTrue(TEXT("One-sided link to an unconnected live target is safe to inspect"), ValidateMeasurementGraph(Fixture.Graph, Reason));
    const int32 TargetIndex = TargetNode->Pins.IndexOfByKey(TargetPin);
    TargetNode->Pins.RemoveAt(TargetIndex);
    TestFalse(TEXT("Referenced pin removed from its node is rejected before dereference"), ValidateMeasurementGraph(Fixture.Graph, Reason));
    TargetNode->Pins.Insert(TargetPin, TargetIndex);
    SourcePin->LinkedTo.Add(nullptr);
    TestFalse(TEXT("Null referenced endpoint is rejected"), ValidateMeasurementGraph(Fixture.Graph, Reason));
    SourcePin->LinkedTo.Pop();
    SourcePin->LinkedTo.RemoveSingle(TargetPin);
    const FGuid TargetId = TargetPin->PinId;
    TargetPin->PinId = SourcePin->PinId;
    TestTrue(TEXT("Pin identity scratch resets between original owning nodes"), ValidateMeasurementGraph(Fixture.Graph, Reason));
    TargetPin->PinId = TargetNode->Pins.Last()->PinId;
    TestFalse(TEXT("Duplicate pin identities within one node are rejected"), ValidateMeasurementGraph(Fixture.Graph, Reason));
    TargetPin->PinId = TargetId;
    Fixture.Graph->Nodes.Last()->NodeGuid = Fixture.Graph->Nodes[0]->NodeGuid;
    const TArray<uint8> Before = SerializeNodes(*Fixture.Graph);
    TestFalse(TEXT("Duplicate node identities rejected"), MeasureGraph(Fixture.Graph, 1.0f, Result, Reason));
    TestTrue(TEXT("Failed measurement exposes no partial snapshot"), Result.Nodes.IsEmpty());
    TestTrue(TEXT("Failure leaves nodes unchanged"), Before == SerializeNodes(*Fixture.Graph));
    Fixture.Graph->Nodes.Reset();
    TestTrue(TEXT("Empty graph succeeds"), MeasureGraph(Fixture.Graph, 1.0f, Result, Reason));
    TestTrue(TEXT("Empty graph has no nodes"), Result.Nodes.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasurementCacheTest, "GlooPrint.Measurement.CacheInvalidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeasurementCacheTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    TSharedPtr<FMeasurementCache> Cache = MakeShared<FMeasurementCache>();
    FMeasurementOptions Options;
    Options.Cache = Cache.Get();
    FGraphMeasurement First, Cached, Cold;
    FString Reason;
    const auto Before = SerializeNodes(*Fixture.Graph);
    if (!TestTrue(TEXT("Cold cache measures"), MeasureGraph(Fixture.Graph, 1, First, Reason, Options)) ||
        !TestTrue(TEXT("Warm cache measures"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options)))
    {
        AddError(Reason); return false;
    }
    TestEqual(TEXT("All unchanged nodes reuse geometry"), Cache->GetHits(), Fixture.Graph->Nodes.Num());
    TestEqual(TEXT("Warm cache constructs no node widgets"), Cache->GetMisses(), 0);
    Compare(*this, First, Cached);
    TestTrue(TEXT("Cache preserves serialized node/pin state"), Before == SerializeNodes(*Fixture.Graph));

    Fixture.Print->FindPinChecked(TEXT("InString"))->DefaultValue = FString::ChrN(100, TEXT('W'));
    if (!TestTrue(TEXT("Direct default edit measures"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestEqual(TEXT("Unnotified edit invalidates only changed node"), Cache->GetMisses(), 1);
    TestTrue(TEXT("Changed default width remeasured"), Find(Cached, Fixture.Print->NodeGuid).BodySize.X > Find(First, Fixture.Print->NodeGuid).BodySize.X);
    if (TestTrue(TEXT("Independent cold measurement succeeds"), MeasureGraph(Fixture.Graph, 1, Cold, Reason))) { Compare(*this, Cold, Cached); }

    Fixture.Print->AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
    if (!TestTrue(TEXT("Unnotified advanced-pin collapse measures"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestEqual(TEXT("Advanced-pin edit invalidates changed node"), Cache->GetMisses(), 1);
    TestTrue(TEXT("Collapsed node gets its new height"), Find(Cached, Fixture.Print->NodeGuid).BodySize.Y < Find(Cold, Fixture.Print->NodeGuid).BodySize.Y);

    Fixture.Graph->NotifyNodeChanged(Fixture.Print);
    TestEqual(TEXT("Node notification evicts affected geometry"), Cache->GetEntryCount(), Fixture.Graph->Nodes.Num() - 1);
    if (!TestTrue(TEXT("Remeasure notified node"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestEqual(TEXT("Other node geometry survives targeted notification"), Cache->GetHits(), Fixture.Graph->Nodes.Num() - 1);
    Fixture.Print->ReconstructNode();
    if (!TestTrue(TEXT("Reconstructed node measures"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestTrue(TEXT("Reconstructed pin identities do not use old geometry"), Cache->GetMisses() > 0);

    if (!TestTrue(TEXT("New DPI measures"), MeasureGraph(Fixture.Graph, 2, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestEqual(TEXT("DPI change invalidates every node"), Cache->GetMisses(), Fixture.Graph->Nodes.Num());
    Fixture.Graph->NotifyGraphChanged();
    TestEqual(TEXT("Graph reconstruction invalidates all geometry"), Cache->GetEntryCount(), 0);
    Cache->Invalidate();
    if (!TestTrue(TEXT("Cache eviction permits cold measurement"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options)) ||
        !TestTrue(TEXT("Cold result after edits measures"), MeasureGraph(Fixture.Graph, 1, Cold, Reason)))
    {
        AddError(Reason); return false;
    }
    Compare(*this, Cold, Cached);
    UEdGraphNode_Comment* Comment = CastChecked<UEdGraphNode_Comment>(Fixture.Graph->Nodes.Last());
    const float PreviousCommentWidth = Find(Cached, Comment->NodeGuid).BodySize.X;
    Comment->NodeWidth += 1600;
    Comment->NodeComment = TEXT("Edited comment title\nwith a second line");
    if (!TestTrue(TEXT("Unnotified comment edit measures"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options))) { AddError(Reason); return false; }
    TestEqual(TEXT("Comment edit invalidates affected geometry"), Cache->GetMisses(), 1);
    TestTrue(TEXT("Edited comment width remeasured"), Find(Cached, Comment->NodeGuid).BodySize.X > PreviousCommentWidth);
    if (TestTrue(TEXT("Cold edited comment measures"), MeasureGraph(Fixture.Graph, 1, Cold, Reason))) { Compare(*this, Cold, Cached); }
    Fixture.Branch->Pins.Add(nullptr);
    TestFalse(TEXT("Warm cache cannot hide an invalid pin"), MeasureGraph(Fixture.Graph, 1, Cached, Reason, Options));
    TestTrue(TEXT("Invalid graph returns no cached partial result"), Cached.Nodes.IsEmpty());
    Fixture.Branch->Pins.Pop();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasurementContinuationTest, "GlooPrint.Measurement.Continuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeasurementContinuationTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    FString Reason;
    const auto Before = SerializeNodes(*Fixture.Graph);
    for (const auto Visibility : {SGraphEditor::Pin_Show, SGraphEditor::Pin_HideNoConnection, SGraphEditor::Pin_HideNoConnectionNoDefault})
    {
        FMeasurementOptions Options; Options.PinVisibility = Visibility;
        FGraphMeasurement Expected, Actual;
        if (!TestTrue(TEXT("Native synchronous reference measures"), MeasureGraph(Fixture.Graph, 1, Expected, Reason, Options))) { AddError(Reason); return false; }
        FMeasurementJob Job(Fixture.Graph, 1, Options);
        TestFalse(TEXT("An expired budget yields after one native node"), Job.Advance(0));
        TestFalse(TEXT("Pending measurement exposes no partial result"), Job.TakeResult(Actual, Reason));
        TestTrue(TEXT("Pending output is empty"), Actual.Nodes.IsEmpty());
        bool bDone = false;
        for (int32 I = 0; I <= Fixture.Graph->Nodes.Num() && !bDone; ++I) { bDone = Job.Advance(0); }
        TestTrue(TEXT("A bounded number of continuations completes measurement"), bDone);
        if (TestTrue(TEXT("Completed native measurement is available"), Job.TakeResult(Actual, Reason))) { Compare(*this, Expected, Actual); }
        else { AddError(Reason); }
        TestFalse(TEXT("Completed measurement can only be taken once"), Job.TakeResult(Actual, Reason));
    }
    TestTrue(TEXT("Every measurement mode preserves graph state"), Before == SerializeNodes(*Fixture.Graph));
    FGraphCaptureJob Stale(Fixture.Graph, 1, {});
    TestFalse(TEXT("Capture yields with original geometry retained"), Stale.Advance(0));
    Fixture.Graph->Nodes[0]->NodePosX += 16;
    bool bDone = false;
    for (int32 I = 0; I <= Fixture.Graph->Nodes.Num() && !bDone; ++I) { bDone = Stale.Advance(0); }
    FLayoutGraph Snapshot;
    TestTrue(TEXT("Unnotified edit terminates capture"), bDone);
    TestFalse(TEXT("Already measured node edit cannot publish mixed geometry"), Stale.TakeResult(Snapshot, Reason));
    TestTrue(TEXT("Stale capture has no partial nodes or links"), Snapshot.Nodes.IsEmpty() && Snapshot.Edges.IsEmpty());
    Fixture.Graph->Nodes[0]->NodePosX -= 16;
    FGraphCaptureJob Reconstructed(Fixture.Graph, 1, {});
    TestFalse(TEXT("Another capture yields"), Reconstructed.Advance(0));
    Fixture.Branch->ReconstructNode();
    bDone = false;
    for (int32 I = 0; I <= Fixture.Graph->Nodes.Num() && !bDone; ++I) { bDone = Reconstructed.Advance(0); }
    TestFalse(TEXT("Reconstructed pins cannot publish old identities"), Reconstructed.TakeResult(Snapshot, Reason));
    TSharedPtr<FMeasurementCache> Cache = MakeShared<FMeasurementCache>();
    FMeasurementOptions Options; Options.Cache = Cache.Get();
    FMeasurementJob ClosedCache(Fixture.Graph, 1, Options);
    TestFalse(TEXT("Cached measurement starts cooperatively"), ClosedCache.Advance(0));
    Cache.Reset();
    TestTrue(TEXT("Closed cache terminates pending measurement"), ClosedCache.Advance(0));
    FGraphMeasurement Measured;
    TestFalse(TEXT("A job does not retain closed cache ownership"), ClosedCache.TakeResult(Measured, Reason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMixedCacheContinuationTest, "GlooPrint.Measurement.MixedCacheContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMixedCacheContinuationTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    const TSharedPtr<FMeasurementCache> Cache = MakeShared<FMeasurementCache>();
    FMeasurementOptions Options; Options.Cache = Cache.Get();
    FGraphMeasurement Expected, Actual;
    FString Reason;
    if (!TestTrue(TEXT("Warm native geometry is available"), MeasureGraph(Fixture.Graph, 1, Expected, Reason, Options))) { AddError(Reason); return false; }
    FMeasurementJob Warm(Fixture.Graph, 1, Options);
    TestFalse(TEXT("Expired warm capture yields before final state validation"), Warm.Advance(0));
    TestTrue(TEXT("Fully cached capture needs only final validation on resume"), Warm.Advance(0));
    if (TestTrue(TEXT("Fully cached result is complete"), Warm.TakeResult(Actual, Reason))) { Compare(*this, Expected, Actual); }
    TestEqual(TEXT("All unchanged nodes reuse native geometry"), Cache->GetHits(), Fixture.Graph->Nodes.Num());
    TestEqual(TEXT("Warm capture has no misses"), Cache->GetMisses(), 0);

    Fixture.Branch->NodeComment += TEXT(" changed branch");
    Fixture.Print->NodeComment += TEXT(" changed print");
    const auto Before = SerializeNodes(*Fixture.Graph);
    FMeasurementJob Mixed(Fixture.Graph, 1, Options);
    TestFalse(TEXT("Mixed capture yields between fresh native measurements"), Mixed.Advance(0));
    TestFalse(TEXT("Mixed pending capture cannot publish partial geometry"), Mixed.TakeResult(Actual, Reason));
    TestTrue(TEXT("Pending mixed output is empty"), Actual.Nodes.IsEmpty());
    TestTrue(TEXT("Remaining native measurement completes"), Mixed.Advance(TNumericLimits<double>::Max()));
    if (!TestTrue(TEXT("Mixed cached/native result is complete"), Mixed.TakeResult(Actual, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Only directly edited nodes miss"), Cache->GetMisses(), 2);
    TestEqual(TEXT("Other nodes reuse measurements"), Cache->GetHits(), Fixture.Graph->Nodes.Num() - 2);
    if (TestTrue(TEXT("Independent cold result measures"), MeasureGraph(Fixture.Graph, 1, Expected, Reason))) { Compare(*this, Expected, Actual); }
    TestTrue(TEXT("Mixed capture preserves all graph values"), Before == SerializeNodes(*Fixture.Graph));

    Fixture.Print->NodeComment += TEXT(" another edit");
    FMeasurementJob Stale(Fixture.Graph, 1, Options);
    TestFalse(TEXT("One cold node leaves final validation pending"), Stale.Advance(0));
    Fixture.Branch->NodePosX += 16;
    TestTrue(TEXT("Edit to an already copied warm node terminates capture"), Stale.Advance(TNumericLimits<double>::Max()));
    TestFalse(TEXT("Final validation rejects stale cached geometry"), Stale.TakeResult(Actual, Reason));
    TestTrue(TEXT("Rejected mixed snapshot exposes no nodes"), Actual.Nodes.IsEmpty());
    TestTrue(TEXT("Failure identifies the unnotified state change"), Reason.Contains(TEXT("changed during capture")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutMeasurementReuseTest, "GlooPrint.Measurement.LayoutReuse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLayoutMeasurementReuseTest::RunTest(const FString& Parameters)
{
    enum class ECallback { None, PinEdit, Reconstruct, ContextChange };
    for (const auto Callback : {ECallback::None, ECallback::PinEdit, ECallback::Reconstruct, ECallback::ContextChange})
    {
        FFixture Fixture;
        const TSharedPtr<FMeasurementCache> Cache = MakeShared<FMeasurementCache>();
        FMeasurementOptions Options; Options.Cache = Cache.Get();
        const auto Before = SerializeNodes(*Fixture.Graph);
        const int32 QueueBefore = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
        bool bNotified = false;
        const auto Handle = Fixture.Graph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateLambda(
            [&](const FEdGraphEditAction&)
            {
                if (bNotified) { return; }
                bNotified = true;
                if (Callback == ECallback::PinEdit)
                {
                    Fixture.Print->FindPinChecked(TEXT("InString"))->DefaultValue += TEXT(" updated by graph observer with a longer default");
                }
                else if (Callback == ECallback::Reconstruct) { Fixture.Print->ReconstructNode(); }
                else if (Callback == ECallback::ContextChange) { Cache->Invalidate(); }
            }));
        FString Reason;
        int32 Changed = 0;
        const bool bFormatted = FormatGraph(Fixture.Graph, 1, {Fixture.Branch->NodeGuid}, Changed, Reason, Options);
        Fixture.Graph->RemoveOnGraphChangedHandler(Handle);
        if (!TestTrue(TEXT("Native format with reusable measurements succeeds"), bFormatted)) { AddError(Reason); return false; }
        TestTrue(TEXT("Format moved nodes and retained the normal graph notification"), Changed > 0 && bNotified);
        if (Callback == ECallback::None)
        {
            TestEqual(TEXT("Only ordinary native measurements survive the layout boundary"), Cache->GetEntryCount(), Fixture.Graph->Nodes.Num() - 1);
            TestEqual(TEXT("Reuse does not add a transaction"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
        }
        else if (Callback == ECallback::ContextChange)
        {
            TestEqual(TEXT("Explicit context invalidation prevents every staged restoration"), Cache->GetEntryCount(), 0);
        }
        else
        {
            TestFalse(TEXT("Callback-edited or reconstructed node cannot regain old geometry"),
                Cache->Find(*Fixture.Print, CaptureMeasurementState(*Fixture.Print)) != nullptr);
        }
        FGraphMeasurement Reused, Cold;
        if (!TestTrue(TEXT("Live post-notification geometry measures"), MeasureGraph(Fixture.Graph, 1, Reused, Reason, Options))) { AddError(Reason); return false; }
        if (Callback == ECallback::None)
        {
            TestEqual(TEXT("Only the resized comment needs native remeasurement"), Cache->GetMisses(), 1);
        }
        if (TestTrue(TEXT("Independent cold post-notification geometry measures"), MeasureGraph(Fixture.Graph, 1, Cold, Reason))) { Compare(*this, Cold, Reused); }
        else { AddError(Reason); return false; }
        if (Callback == ECallback::None)
        {
            const auto After = SerializeNodes(*Fixture.Graph);
            TestTrue(TEXT("Layout reuse preserves native undo"), GEditor->UndoTransaction());
            TestTrue(TEXT("One undo restores exact original values"), Before == SerializeNodes(*Fixture.Graph));
            TestTrue(TEXT("Layout reuse preserves native redo"), GEditor->RedoTransaction());
            TestTrue(TEXT("Redo restores exact formatted values"), After == SerializeNodes(*Fixture.Graph));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphSnapshotTest, "GlooPrint.Editor.AnchorRulesAndConnectionValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphSnapshotTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    UK2Node_CustomEvent* Entry = Fixture.Add<UK2Node_CustomEvent>(FVector2f(1500, 0));
    Entry->CustomFunctionName = TEXT("FixtureEntry");
    Entry->NodeGuid = FGuid(0, 0, 0, 1);
    UK2Node_CustomEvent* SecondEntry = Fixture.Add<UK2Node_CustomEvent>(FVector2f(1500, 500));
    SecondEntry->CustomFunctionName = TEXT("SecondEntry");
    SecondEntry->NodeGuid = FGuid(0, 0, 0, 2);
    FLayoutGraph Snapshot;
    FString Reason;
    auto CheckAnchor = [&](const TSet<FGuid>& Selection, const FGuid& Expected)
    {
        if (TestTrue(TEXT("Anchor selection captures"), CaptureGraph(Fixture.Graph, 1, Selection, Snapshot, Reason)))
        {
            TestEqual(TEXT("Correct deterministic anchor"), Snapshot.Nodes[Snapshot.Anchor].Geometry.Id, Expected);
        }
        else { AddError(Reason); }
    };
    CheckAnchor({Fixture.Branch->NodeGuid, SecondEntry->NodeGuid}, SecondEntry->NodeGuid);
    CheckAnchor({Entry->NodeGuid, SecondEntry->NodeGuid}, Entry->NodeGuid);
    CheckAnchor({}, Entry->NodeGuid);
    for (UEdGraphNode* Node : Fixture.Graph->Nodes)
    {
        if (Node->IsA<UEdGraphNode_Comment>()) { CheckAnchor({Node->NodeGuid}, Entry->NodeGuid); }
    }
    const FGuid Regular = Fixture.Branch->NodeGuid < Fixture.Print->NodeGuid ? Fixture.Branch->NodeGuid : Fixture.Print->NodeGuid;
    CheckAnchor({Fixture.Branch->NodeGuid, Fixture.Print->NodeGuid}, Regular);
    UEdGraphPin* Source = Fixture.Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
    UEdGraphPin* Target = Source->LinkedTo[0];
    Target->LinkedTo.Remove(Source);
    const auto Before = SerializeNodes(*Fixture.Graph);
    TestFalse(TEXT("One-sided connection rejected"), CaptureGraph(Fixture.Graph, 1, {}, Snapshot, Reason));
    TestTrue(TEXT("Invalid connection exposes no partial snapshot"), Snapshot.Nodes.IsEmpty() && Snapshot.Edges.IsEmpty());
    TestTrue(TEXT("Failed capture preserves malformed graph for the user to repair"), Before == SerializeNodes(*Fixture.Graph));
    Target->LinkedTo.Add(Source);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatTransactionTest, "GlooPrint.Editor.TransactionAndColdNoOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormatTransactionTest::RunTest(const FString& Parameters)
{
    FFixture Fixture;
    const TArray<uint8> Before = SerializeNodes(*Fixture.Graph);
    const auto Status = Fixture.Blueprint->Status;
    const TSet<FGuid> Selection{Fixture.Branch->NodeGuid};
    FLayoutGraph Snapshot;
    FLayoutResult Layout;
    FString Reason;
    int32 Changed = 0;
    if (!TestTrue(TEXT("Capture complete graph"), CaptureGraph(Fixture.Graph, 1, Selection, Snapshot, Reason)) ||
        !TestTrue(TEXT("Compute fixture layout"), ComputeLayout(Snapshot, {}, Layout, Reason)))
    {
        AddError(Reason); return false;
    }
    TestEqual(TEXT("Selection does not exclude other nodes"), Snapshot.Nodes.Num(), Fixture.Graph->Nodes.Num());
    TestEqual(TEXT("Selection chooses anchor"), Snapshot.Nodes[Snapshot.Anchor].Geometry.Id, Fixture.Branch->NodeGuid);
    ++Fixture.Branch->NodePosX;
    TestFalse(TEXT("Stale position rejected before transaction"), ApplyLayout(Fixture.Graph, Snapshot, Layout, Changed, Reason));
    --Fixture.Branch->NodePosX;
    TestTrue(TEXT("Rejected application preserves original graph"), Before == SerializeNodes(*Fixture.Graph));
    const int32 QueueBefore = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
    if (!TestTrue(TEXT("Apply fixture in editor transaction"), ApplyLayout(Fixture.Graph, Snapshot, Layout, Changed, Reason)))
    {
        AddError(Reason); return false;
    }
    TestTrue(TEXT("Fixture actually changed"), Changed > 0);
    TestEqual(TEXT("Exactly one undo entry"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
    TestEqual(TEXT("Anchor X unchanged"), Fixture.Branch->NodePosX, 0);
    TestEqual(TEXT("Anchor Y unchanged"), Fixture.Branch->NodePosY, 0);
    TestEqual(TEXT("Position changes do not invalidate compilation status"), Fixture.Blueprint->Status, Status);
    const TArray<uint8> After = SerializeNodes(*Fixture.Graph);
    const bool DirtyBeforeNoOp = Fixture.Graph->GetOutermost()->IsDirty();
    if (!TestTrue(TEXT("Cold second format succeeds"), FormatGraph(Fixture.Graph, 1, Selection, Changed, Reason)))
    {
        AddError(Reason); return false;
    }
    TestEqual(TEXT("Second format changes no objects"), Changed, 0);
    TestEqual(TEXT("No-op adds no undo entry"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
    TestEqual(TEXT("No-op preserves dirty state"), Fixture.Graph->GetOutermost()->IsDirty(), DirtyBeforeNoOp);
    TestTrue(TEXT("No-op preserves serialized graph"), After == SerializeNodes(*Fixture.Graph));
    Algo::Reverse(Fixture.Graph->Nodes);
    FLayoutGraph Shuffled;
    FLayoutResult ShuffledLayout;
    if (TestTrue(TEXT("Shuffled node enumeration captures"), CaptureGraph(Fixture.Graph, 1, Selection, Shuffled, Reason)) &&
        TestTrue(TEXT("Shuffled node enumeration lays out"), ComputeLayout(Shuffled, {}, ShuffledLayout, Reason)))
    {
        TestTrue(TEXT("Input order does not alter layout"), ShuffledLayout.Positions == Layout.Positions);
    }
    Algo::Reverse(Fixture.Graph->Nodes);
    TestTrue(TEXT("One-step undo succeeds"), GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores all serialized node and pin state"), Before == SerializeNodes(*Fixture.Graph));
    TestTrue(TEXT("One-step redo succeeds"), GEditor->RedoTransaction());
    TestTrue(TEXT("Redo restores exact formatted state"), After == SerializeNodes(*Fixture.Graph));
    Fixture.Graph->bEditable = false;
    TestFalse(TEXT("Read-only graph rejects formatting"), FormatGraph(Fixture.Graph, 1, Selection, Changed, Reason));
    TestTrue(TEXT("Read-only refusal preserves graph"), After == SerializeNodes(*Fixture.Graph));
    return true;
}

class FFormatFocusCheck final : public IAutomationLatentCommand
{
public:
    explicit FFormatFocusCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FFormatFocusCheck()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        if (Package.IsValid()) { Package->SetDirtyFlag(false); }
        if (bRestoreStyle)
        {
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
        }
    }
    virtual bool Update() override
    {
        FSlateApplication& Slate = FSlateApplication::Get();
        if ((Phase >= 3 && Phase <= 8) || Phase == 13)
        {
            if (++Frames < 10) { return false; }
            if (Phase == 3)
            {
                Test.TestFalse(TEXT("Closing graph panel releases measurement cache"), ClosedCache.IsValid());
                Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
                Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint reopened graph"))).ClientSize(FVector2f(1000, 700))[Editor.ToSharedRef()];
                Slate.AddWindow(Window.ToSharedRef());
                Editor->SetNodeSelection(Fixture->Branch, true);
                Phase = 4; Frames = 0; return false;
            }
            if (Phase == 4)
            {
                CheckStableRoutes();
                const auto ReopenedCache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
                if (Test.TestTrue(TEXT("Reopened routing creates fresh shared measurements without F"), ReopenedCache.IsValid()))
                {
                    Test.TestEqual(TEXT("Reopen measures every node anew for routing"), ReopenedCache->GetMisses(), Fixture->Graph->Nodes.Num());
                    Test.TestEqual(TEXT("Background routing retains every measured node"), ReopenedCache->GetEntryCount(), Fixture->Graph->Nodes.Num());
                    ReopenedCache->Invalidate();
                    Test.TestEqual(TEXT("Reopened F starts with explicitly cold geometry"), ReopenedCache->GetEntryCount(), 0);
                }
                EvaluateNativePinTooltip();
                if (After != SerializeNodes(*Fixture->Graph))
                {
                    Test.AddInfo(TEXT("Serialized difference already exists before reopened F:"));
                    ReportNodeDifferences(Test, AfterProperties, *Fixture->Graph, &AfterNodeBytes);
                }
                NoOpQueue = GEditor->Trans->GetQueueLength();
                Package->SetDirtyFlag(false);
                Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
                Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
                Phase = 13; Frames = 0; return false;
            }
            if (Phase == 13)
            {
                const auto PendingCache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
                if (PendingCache && PendingCache->GetEntryCount() != Fixture->Graph->Nodes.Num() && Frames < 120) { return false; }
                const int32 Queue = NoOpQueue;
                if (!Test.TestTrue(TEXT("Reopened graph formats as a cold no-op"), After == SerializeNodes(*Fixture->Graph)))
                {
                    ReportNodeDifferences(Test, AfterProperties, *Fixture->Graph, &AfterNodeBytes);
                }
                Test.TestEqual(TEXT("Reopen no-op creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
                Test.TestFalse(TEXT("Reopened no-op leaves the owned package clean"), Package->IsDirty());
                const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
                if (Test.TestTrue(TEXT("Reopened panel has a fresh cache"), Cache.IsValid()))
                {
                    Test.TestEqual(TEXT("Reopen measures every node without a disk cache"), Cache->GetMisses(), Fixture->Graph->Nodes.Num());
                    ClosedCache = Cache;
                    Cache->Invalidate();
                    Test.TestEqual(TEXT("Explicit eviction removes every cached measurement"), Cache->GetEntryCount(), 0);
                }
                const auto Routes = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
                if (Test.TestTrue(TEXT("Reopened graph has live custom routes"), Routes && Routes->IsReady()))
                {
                    RouteBuilds = Routes->GetBuildCount(); Routes->Invalidate();
                    Test.TestFalse(TEXT("Explicit eviction discards cached routes"), Routes->IsReady());
                }
                Test.TestTrue(TEXT("F handles the graph after both caches are evicted"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
                Test.TestTrue(TEXT("F after eviction preserves every formatted node and pin value"), After == SerializeNodes(*Fixture->Graph));
                Test.TestEqual(TEXT("F after eviction creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
                Test.TestFalse(TEXT("F after eviction leaves the package clean"), Package->IsDirty());
                Phase = 5; Frames = 0; return false;
            }
            if (Phase == 5)
            {
                CheckStableRoutes();
                const auto Routes = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
                if (Routes) { Test.TestEqual(TEXT("Evicted routes rebuild once without a layout edit"), Routes->GetBuildCount(), RouteBuilds + 1); }
                Test.TestTrue(TEXT("Cold route rebuilding preserves formatted graph data"), After == SerializeNodes(*Fixture->Graph));
                Test.TestFalse(TEXT("Cold route rebuilding leaves the package clean"), Package->IsDirty());
                Window->RequestDestroyWindow(); Editor.Reset(); Window.Reset();
                Phase = 6; Frames = 0; return false;
            }
            if (Phase == 6)
            {
                Test.TestFalse(TEXT("Reopened panel also releases its cache"), ClosedCache.IsValid());
                Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false, Package.Get());
                Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
                Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint empty graph"))).ClientSize(FVector2f(1000, 700))[Editor.ToSharedRef()];
                Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(123, -456), 0.5f);
                Phase = 7; Frames = 0; return false;
            }
            if (Phase == 7)
            {
                Package->SetDirtyFlag(false); NoOpQueue = GEditor->Trans->GetQueueLength();
                const auto Status = Fixture->Blueprint->Status;
                Editor->GetViewLocation(View, Zoom);
                int32 Changed = INDEX_NONE; FString Reason; bool bRetry = true;
                const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
                Test.TestTrue(TEXT("Empty graph formatting succeeds through the transaction entry point"), FormatGraph(Fixture->Graph, Scale, {}, Changed, Reason, {}, &bRetry));
                Test.TestEqual(TEXT("Empty graph formatting changes no objects"), Changed, 0);
                Test.TestTrue(TEXT("Empty graph needs no error message or deferred retry"), Reason.IsEmpty() && !bRetry);
                Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
                for (int32 Press = 0; Press < 3; ++Press)
                {
                    if (Press == 2)
                    {
                        const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
                        if (Test.TestTrue(TEXT("Empty graph owns the normal measurement cache"), Cache.IsValid())) { Cache->Invalidate(); }
                    }
                    Test.TestTrue(TEXT("Actual F handles an empty focused graph with cold or warm cache"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
                }
                Test.TestEqual(TEXT("Empty no-op preserves Blueprint compile status"), Fixture->Blueprint->Status, Status);
                Phase = 8; Frames = 0; return false;
            }
            Test.TestTrue(TEXT("Repeated empty F never creates a node or selection"), Fixture->Graph->Nodes.IsEmpty() && Editor->GetSelectedNodes().IsEmpty());
            Test.TestEqual(TEXT("Repeated empty F opens no transaction, including on later frames"), GEditor->Trans->GetQueueLength(), NoOpQueue);
            Test.TestFalse(TEXT("Repeated empty F leaves the package clean on later frames"), Package->IsDirty());
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Empty F preserves camera position"), AfterView, View);
            Test.TestEqual(TEXT("Empty F preserves zoom"), AfterZoom, Zoom);
            Window->RequestDestroyWindow(); Editor.Reset(); Window.Reset(); Fixture.Reset(); return true;
        }
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle; bRestoreStyle = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintNoOp_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), true, Package.Get());
            Fixture->Print->SetPosition(FVector2f(350, 0));
            Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then)->LinkedTo[0]->GetOwningNode()->SetPosition(FVector2f(640, 260));
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                if (Node->IsA<UEdGraphNode_Comment>()) { Node->NodeComment = TEXT("GlooPrint format fixture — native Blueprint widgets"); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable_Lambda([this] { return bPanelEditable; });
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint format fixture"))).ClientSize(FVector2f(1500, 950))
                [ SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()[ SAssignNew(Text, SEditableTextBox) ]
                    + SVerticalBox::Slot().FillHeight(1)[ Editor.ToSharedRef() ] ];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-160, -550), 0.75f);
            Editor->SetNodeSelection(Fixture->Branch, true);
            return false;
        }
        if (++Frames < 10) { return false; }
        if (Phase == 0)
        {
            EvaluateNativePinTooltip();
            Package->SetDirtyFlag(false);
            Before = SerializeNodes(*Fixture->Graph);
            BeforeProperties = DescribeNodes(*Fixture->Graph);
            bPanelEditable = false;
            Editor->GetGraphPanel()->UpdateAllAttributes();
            Test.TestFalse(TEXT("Native panel exposes its read-only state"), Editor->GetGraphPanel()->IsGraphEditable());
            CheckRefusedF(TEXT("Read-only panel"));
            bPanelEditable = true;
            Editor->GetGraphPanel()->UpdateAllAttributes();
            Fixture->Graph->bEditable = false;
            CheckRefusedF(TEXT("Read-only graph"), TEXT("read-only"));
            Fixture->Graph->bEditable = true;
            {
                FScopedTransaction Transaction(FText::FromString(TEXT("GlooPrint safety fixture operation")));
                Test.TestTrue(TEXT("A real editor transaction is active"), GEditor->IsTransactionActive());
                CheckRefusedF(TEXT("Active editor transaction"), TEXT("current editor operation"));
                Transaction.Cancel();
            }
            Capture(TEXT("GlooPrint-Before.png"));
            Slate.SetKeyboardFocus(Text, EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Test.TestTrue(TEXT("F in a text field leaves graph unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestFalse(TEXT("F in a text field leaves the owned package clean"), Package->IsDirty());
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            Test.TestTrue(TEXT("Refused F never formats later after its restriction ends"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestFalse(TEXT("Refused F leaves the owned package clean on later frames"), Package->IsDirty());
            Editor->GetViewLocation(View, Zoom);
            FormatQueue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Native graph panel receives keyboard focus"), Slate.GetKeyboardFocusedWidget().Get() == Editor->GetGraphPanel());
            Test.TestTrue(TEXT("Registered F command handles native key event"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 10; Frames = 0; return false;
        }
        if (Phase == 10)
        {
            if (GEditor->Trans->GetQueueLength() == FormatQueue)
            {
                if (Frames < 120) { return false; }
                Test.AddError(TEXT("Focused F did not apply within 120 frames."));
                Window->RequestDestroyWindow(); return true;
            }
            After = SerializeNodes(*Fixture->Graph, &AfterNodeBytes);
            AfterProperties = DescribeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("F actually formats the fixture"), Before != After);
            Test.TestEqual(TEXT("F creates one undo entry"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            Test.TestTrue(TEXT("An actual layout edit marks the owned package dirty"), Package->IsDirty());
            Package->SetDirtyFlag(false);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, true, 0, 0));
            Test.TestEqual(TEXT("Key repeat creates no undo entry"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 11; Frames = 0; return false;
        }
        if (Phase == 11)
        {
            Test.TestTrue(TEXT("Second actual F is a cold no-op"), After == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("No-op F creates no undo entry"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 12; Frames = 0; return false;
        }
        if (Phase == 12)
        {
            Test.TestFalse(TEXT("Repeated no-op F does not dirty a clean package"), Package->IsDirty());
            const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
            if (Test.TestTrue(TEXT("Open graph owns its geometry cache"), Cache.IsValid()))
            {
                Test.TestEqual(TEXT("Repeated unchanged F reuses every measurement"), Cache->GetHits(), Fixture->Graph->Nodes.Num());
                Slate.InvalidateAllWidgets(false);
                Test.TestEqual(TEXT("Slate/style invalidation clears cached geometry"), Cache->GetEntryCount(), 0);
                ClosedCache = Cache;
            }
            Phase = 2; Frames = 0; return false;
        }
        FVector2f AfterView;
        float AfterZoom;
        Editor->GetViewLocation(AfterView, AfterZoom);
        Test.TestEqual(TEXT("Formatting preserves viewport"), View, AfterView);
        Test.TestEqual(TEXT("Formatting preserves zoom"), Zoom, AfterZoom);
        Test.TestTrue(TEXT("Formatting preserves selected anchor"), Editor->GetSelectedNodes().Contains(Fixture->Branch));
        Test.TestEqual(TEXT("Formatting preserves selection count"), Editor->GetSelectedNodes().Num(), 1);
        Capture(TEXT("GlooPrint-After.png"));
        const auto Routes = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
        if (Test.TestTrue(TEXT("Formatted graph has settled live custom routes"), Routes && Routes->IsReady()))
        {
            StableRoutes = Routes->GetRoutes();
            Test.TestEqual(TEXT("No-op fixture retains both original wires"), StableRoutes.Wires.Num(), 2);
            Test.TestEqual(TEXT("No-op fixture has no native fallback"), StableRoutes.FallbackCount, 0);
        }
        const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
        FMeasurementOptions Options;
        Options.Cache = Cache.Get();
        FGraphMeasurement Geometry, Cold;
        FString Reason;
        Test.TestTrue(TEXT("Warm cache before undo"), MeasureGraph(Fixture->Graph, 1, Geometry, Reason, Options));
        const uint64 BeforeUndoRevision = Cache->GetRevision();
        Test.TestTrue(TEXT("Visible editor undo succeeds"), GEditor->UndoTransaction());
        if (!Test.TestTrue(TEXT("Visible editor undo restores serialized nodes/pins"), Before == SerializeNodes(*Fixture->Graph)))
        {
            ReportNodeDifferences(Test, BeforeProperties, *Fixture->Graph);
        }
        Test.TestTrue(TEXT("Native undo invalidates cache revision"), Cache->GetRevision() != BeforeUndoRevision);
        Test.TestTrue(TEXT("Cache measures restored undo state"), MeasureGraph(Fixture->Graph, 1, Geometry, Reason, Options));
        Test.TestTrue(TEXT("Cold undo measurement succeeds"), MeasureGraph(Fixture->Graph, 1, Cold, Reason));
        Compare(Test, Cold, Geometry);
        const uint64 BeforeRedoRevision = Cache->GetRevision();
        Test.TestTrue(TEXT("Visible editor redo succeeds"), GEditor->RedoTransaction());
        if (!Test.TestTrue(TEXT("Visible editor redo restores formatted state"), After == SerializeNodes(*Fixture->Graph)))
        {
            ReportNodeDifferences(Test, AfterProperties, *Fixture->Graph);
        }
        Test.TestTrue(TEXT("Native redo invalidates cache revision"), Cache->GetRevision() != BeforeRedoRevision);
        Test.TestTrue(TEXT("Cache measures restored redo state"), MeasureGraph(Fixture->Graph, 1, Geometry, Reason, Options));
        Test.TestTrue(TEXT("Cold redo measurement succeeds"), MeasureGraph(Fixture->Graph, 1, Cold, Reason));
        Compare(Test, Cold, Geometry);
        Window->RequestDestroyWindow();
        Text.Reset(); Editor.Reset(); Window.Reset();
        Phase = 3; Frames = 0; return false;
    }
private:
    void CheckRefusedF(const FString& Context, const TCHAR* ExpectedReason = nullptr)
    {
        const auto State = SerializeNodes(*Fixture->Graph);
        const int32 Queue = GEditor->Trans->GetQueueLength();
        const auto Status = Fixture->Blueprint->Status;
        const bool bDirty = Package->IsDirty();
        auto& Slate = FSlateApplication::Get();
        FVector2f BeforeView; float BeforeZoom; Editor->GetViewLocation(BeforeView, BeforeZoom);
        if (ExpectedReason)
        {
            FString Reason; int32 Changed = INDEX_NONE; bool bRetry = true;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            Test.TestFalse(Context + TEXT(" rejects the direct format entry point"), FormatGraph(Fixture->Graph, Scale, {}, Changed, Reason, {}, &bRetry));
            Test.TestTrue(Context + TEXT(" explains refusal"), Reason.Contains(ExpectedReason));
            Test.TestTrue(Context + TEXT(" changes no nodes and schedules no retry"), Changed == 0 && !bRetry);
        }
        Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        Test.TestTrue(Context + TEXT(" handles the actual F command"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
        Test.TestTrue(Context + TEXT(" preserves every node and pin value"), State == SerializeNodes(*Fixture->Graph));
        Test.TestEqual(Context + TEXT(" adds no transaction"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestEqual(Context + TEXT(" preserves package dirty state"), Package->IsDirty(), bDirty);
        Test.TestEqual(Context + TEXT(" preserves compile status"), Fixture->Blueprint->Status, Status);
        FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
        Test.TestTrue(Context + TEXT(" preserves view and selected anchor"), BeforeView == AfterView && BeforeZoom == AfterZoom &&
            Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Fixture->Branch));
    }
    void CheckStableRoutes()
    {
        const auto Cache = Editor->GetGraphPanel()->GetMetaData<FRouteCache>();
        if (!Test.TestTrue(TEXT("Cold graph rebuild has settled live routes"), Cache && Cache->IsReady())) { return; }
        const auto& Current = Cache->GetRoutes();
        Test.TestEqual(TEXT("Cold no-op retains every original wire"), Current.Wires.Num(), StableRoutes.Wires.Num());
        Test.TestEqual(TEXT("Cold no-op retains fallback decisions"), Current.FallbackCount, StableRoutes.FallbackCount);
        for (const auto& Pair : StableRoutes.Wires)
        {
            const auto* Wire = Current.Wires.Find(Pair.Key);
            if (!Test.TestTrue(TEXT("Cold route retains its original node and pin identities"), Wire != nullptr)) { continue; }
            const auto& Old = Pair.Value;
            Test.TestTrue(TEXT("Cold no-op preserves route points, bounds and attachment regions exactly"),
                Wire->Points == Old.Points && Wire->Bounds == Old.Bounds && Wire->StartRegion == Old.StartRegion && Wire->EndRegion == Old.EndRegion && Wire->Fallback == Old.Fallback && Wire->Length == Old.Length);
            if (!Test.TestEqual(TEXT("Cold no-op preserves the number of curve pieces"), Wire->Curves.Num(), Old.Curves.Num())) { continue; }
            for (int32 I = 0; I < Old.Curves.Num(); ++I)
            {
                const auto& A = Old.Curves[I]; const auto& B = Wire->Curves[I];
                Test.TestTrue(TEXT("Cold no-op preserves exact curve geometry and length"), A.Start == B.Start && A.End == B.End && A.StartTangent == B.StartTangent && A.EndTangent == B.EndTangent && A.Length == B.Length);
                for (int32 S = 0; S < 17; ++S) { Test.TestTrue(TEXT("Cold no-op preserves exact pulse-distance samples"), A.Distances[S] == B.Distances[S]); }
            }
        }
    }
    void EvaluateNativePinTooltip()
    {
        const auto Node = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Fixture->Print->NodeGuid);
        const auto Pin = Node ? Node->FindWidgetForPin(Fixture->Print->FindPinChecked(TEXT("InString"))) : nullptr;
        const auto Tooltip = Pin ? Pin->GetToolTip() : nullptr;
        Test.TestTrue(TEXT("Native function pin has a nonempty tooltip"), Tooltip && !Tooltip->IsEmpty());
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;
        FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture actual editor fixture"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        Test.TestTrue(TEXT("Save editor fixture image"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
    }
    FAutomationTestBase& Test;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SEditableTextBox> Text;
    TSharedPtr<SWindow> Window;
    TArray<uint8> Before, After;
    TMap<FString, FString> BeforeProperties, AfterProperties;
    TMap<FGuid, TArray<uint8>> AfterNodeBytes;
    TWeakPtr<FMeasurementCache> ClosedCache;
    FRouteSet StableRoutes;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    bool bRestoreStyle = false, bPanelEditable = true;
    FVector2f View;
    float Zoom = 1;
    int32 Frames = 0, Phase = 0, RouteBuilds = 0, NoOpQueue = 0, FormatQueue = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatFocusTest, "GlooPrint.Editor.FocusedShortcut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormatFocusTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFormatFocusCheck(*this));
    return true;
}

enum class EDeferredCase { Ready, Unavailable, DirectEdit, Reconstruction, Deletion, Closed, Escape, Focus, Selection, Display, Notification, Settings, Grid, Disabled };

class FDeferredFormatCheck final : public IAutomationLatentCommand
{
public:
    FDeferredFormatCheck(FAutomationTestBase& InTest, EDeferredCase InCase) : Test(InTest), Case(InCase) {}
    virtual ~FDeferredFormatCheck()
    {
        if (Factory) { FEdGraphUtilities::UnregisterVisualNodeFactory(Factory); }
        RestoreSettings();
    }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            Fixture = MakeUnique<FFixture>();
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint deferred measurement"))).ClientSize(FVector2f(1000, 700))
                [SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()[SAssignNew(Text, SEditableTextBox)]
                    + SVerticalBox::Slot().FillHeight(1)[Editor.ToSharedRef()]];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetNodeSelection(Fixture->Branch, true);
            return false;
        }
        if (++Frames < 10) { return false; }
        if (Phase == 0)
        {
            Factory = MakeShared<FDelayedNodeFactory>();
            Factory->Target = Fixture->Branch;
            FEdGraphUtilities::RegisterVisualNodeFactory(Factory);
            EvaluateNativePinTooltip();
            if (const auto Cache = Editor->GetGraphPanel()->GetMetaData<FMeasurementCache>()) { Cache->Invalidate(); }
            Before = SerializeNodes(*Fixture->Graph);
            Queue = GEditor->Trans->GetQueueLength();
            Dirty = Fixture->Graph->GetOutermost()->IsDirty();
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("F accepts deferred geometry request"), PressF());
            Test.TestTrue(TEXT("Unavailable geometry leaves graph untouched"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Pending request creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
            Test.TestEqual(TEXT("One initial native measurement attempt"), Factory->Readiness->Creations, 1);
            PressF();
            Test.TestEqual(TEXT("Duplicate pending F is coalesced"), Factory->Readiness->Creations, 1);
            switch (Case)
            {
            case EDeferredCase::Ready:
                Factory->Readiness->ReadyFrame = GFrameCounter + 2; break;
            case EDeferredCase::Unavailable:
                break;
            case EDeferredCase::DirectEdit:
                Fixture->Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("User edit during pending format"); break;
            case EDeferredCase::Reconstruction:
                Fixture->Branch->ReconstructNode(); break;
            case EDeferredCase::Deletion:
                for (UEdGraphNode* Node : Fixture->Graph->Nodes)
                {
                    if (Node->IsA<UK2Node_Knot>()) { Fixture->Graph->RemoveNode(Node); break; }
                }
                break;
            case EDeferredCase::Closed:
                Window->RequestDestroyWindow(); Text.Reset(); Editor.Reset(); Window.Reset(); break;
            case EDeferredCase::Escape:
                Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0)); break;
            case EDeferredCase::Focus:
                Slate.SetKeyboardFocus(Text, EFocusCause::SetDirectly); break;
            case EDeferredCase::Selection:
                Editor->SetNodeSelection(Fixture->Print, true); break;
            case EDeferredCase::Display:
                Editor->SetPinVisibility(SGraphEditor::Pin_HideNoConnection); break;
            case EDeferredCase::Notification:
                Fixture->Graph->NotifyNodeChanged(Fixture->Branch); break;
            case EDeferredCase::Settings:
                OriginalSpacing = GetDefault<UGlooPrintSettings>()->HorizontalSpacing;
                GetMutableDefault<UGlooPrintSettings>()->HorizontalSpacing += 32;
                GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); break;
            case EDeferredCase::Disabled:
                OriginalEnabled = GetDefault<UGlooPrintSettings>()->bFormattingEnabled;
                GetMutableDefault<UGlooPrintSettings>()->bFormattingEnabled = false;
                GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); break;
            case EDeferredCase::Grid:
                OriginalGrid = GetDefault<UEditorStyleSettings>()->GridSnapSize;
                GetMutableDefault<UEditorStyleSettings>()->GridSnapSize = OriginalGrid.GetValue() == 32 ? 16 : 32;
                break;
            }
            if (Case != EDeferredCase::Ready && Case != EDeferredCase::Unavailable)
            {
                Factory->Readiness->ReadyFrame = GFrameCounter + 1;
            }
            Expected = SerializeNodes(*Fixture->Graph);
            ExpectedProperties = DescribeNodes(*Fixture->Graph);
            Dirty = Fixture->Graph->GetOutermost()->IsDirty();
            Phase = 1; Frames = 0; return false;
        }
        const FString Label = FString::Printf(TEXT("Deferred case %d"), int32(Case));
        if (Case == EDeferredCase::Ready)
        {
            const auto After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Ready geometry completes deferred formatting"), After != Before);
            Test.TestEqual(TEXT("Deferred format creates exactly one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Deferred undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Deferred undo restores exact original data"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Deferred redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Deferred redo restores exact formatted data"), After == SerializeNodes(*Fixture->Graph));
        }
        else
        {
            if (Case == EDeferredCase::Focus)
            {
                EvaluateNativePinTooltip();
            }
            if (!Test.TestTrue(Label + TEXT(" preserves graph/user edits"), Expected == SerializeNodes(*Fixture->Graph)))
            {
                ReportNodeDifferences(Test, ExpectedProperties, *Fixture->Graph);
            }
            Test.TestEqual(Label + TEXT(" creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
            Test.TestEqual(Label + TEXT(" preserves dirty state"), Fixture->Graph->GetOutermost()->IsDirty(), Dirty);
            if (Case == EDeferredCase::Unavailable)
            {
                Test.TestEqual(TEXT("Unavailable geometry stops after initial plus three deferred attempts"), Factory->Readiness->Creations, 4);
                if (Phase == 1)
                {
                    Factory->Readiness->ReadyFrame = GFrameCounter;
                    Phase = 2; Frames = 0; return false;
                }
                Test.TestTrue(TEXT("Exhausted request never resumes after readiness changes"), Before == SerializeNodes(*Fixture->Graph));
            }
        }
        FEdGraphUtilities::UnregisterVisualNodeFactory(Factory); Factory.Reset();
        RestoreSettings();
        if (Window) { Window->RequestDestroyWindow(); }
        Text.Reset(); Editor.Reset(); Window.Reset();
        return true;
    }
private:
    void EvaluateNativePinTooltip()
    {
        const auto Node = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Fixture->Print->NodeGuid);
        const auto Pin = Node ? Node->FindWidgetForPin(Fixture->Print->FindPinChecked(TEXT("InString"))) : nullptr;
        const auto Tooltip = Pin ? Pin->GetToolTip() : nullptr;
        Test.TestTrue(TEXT("Deferred fixture has a native pin tooltip"), Tooltip && !Tooltip->IsEmpty());
    }
    void RestoreSettings()
    {
        if (!OriginalSpacing.IsSet() && !OriginalEnabled.IsSet() && !OriginalGrid.IsSet()) { return; }
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        if (OriginalSpacing.IsSet()) { Settings->HorizontalSpacing = OriginalSpacing.GetValue(); OriginalSpacing.Reset(); }
        if (OriginalEnabled.IsSet()) { Settings->bFormattingEnabled = OriginalEnabled.GetValue(); OriginalEnabled.Reset(); }
        if (OriginalGrid.IsSet()) { GetMutableDefault<UEditorStyleSettings>()->GridSnapSize = OriginalGrid.GetValue(); OriginalGrid.Reset(); }
        Settings->NotifyChanged();
    }
    bool PressF() { return FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)); }
    FAutomationTestBase& Test;
    EDeferredCase Case;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SEditableTextBox> Text;
    TSharedPtr<FDelayedNodeFactory> Factory;
    TArray<uint8> Before, Expected;
    TMap<FString, FString> ExpectedProperties;
    int32 Frames = 0, Phase = 0, Queue = 0;
    bool Dirty = false;
    TOptional<float> OriginalSpacing;
    TOptional<bool> OriginalEnabled;
    TOptional<uint32> OriginalGrid;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeferredFormatTest, "GlooPrint.Editor.DeferredMeasurement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeferredFormatTest::RunTest(const FString& Parameters)
{
    for (const auto Case : {EDeferredCase::Ready, EDeferredCase::Unavailable, EDeferredCase::DirectEdit,
        EDeferredCase::Reconstruction, EDeferredCase::Deletion, EDeferredCase::Closed, EDeferredCase::Escape,
        EDeferredCase::Focus, EDeferredCase::Selection, EDeferredCase::Display, EDeferredCase::Notification,
        EDeferredCase::Settings, EDeferredCase::Grid, EDeferredCase::Disabled})
    {
        ADD_LATENT_AUTOMATION_COMMAND(FDeferredFormatCheck(*this, Case));
    }
    return true;
}

class FBlueprintMenuCheck final : public IAutomationLatentCommand
{
public:
    explicit FBlueprintMenuCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        FSlateApplication& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            Fixture = MakeUnique<FFixture>();
            BlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Fixture->Blueprint.Get(), true);
            if (!Test.TestTrue(TEXT("Native Blueprint editor opens for disposable fixture"), BlueprintEditor.IsValid())) { return true; }
            Editor = BlueprintEditor->OpenGraphAndBringToFront(Fixture->Graph);
            if (!Test.TestTrue(TEXT("Native Blueprint editor opens target graph"), Editor.IsValid())) { return Finish(); }
            return false;
        }
        if (++Frames < 20) { return false; }
        if (!bSummoned)
        {
            SGraphPanel* Panel = Editor->GetGraphPanel();
            Editor->SetNodeSelection(Fixture->Branch, true);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Panel->SummonContextMenu(Panel->GetCachedGeometry().LocalToAbsolute(FVector2f(300, 200)),
                FVector2f::ZeroVector, Fixture->Branch, nullptr, {});
            bSummoned = true; Frames = 0; return false;
        }
        const auto MenuWindow = Slate.GetVisibleMenuWindow();
        bool bFoundFormat = false;
        TArray<FString> Labels;
        if (Test.TestTrue(TEXT("Native node context menu opens"), MenuWindow.IsValid()))
        {
            TArray<TSharedRef<SWidget>> Widgets;
            TArray<TSharedRef<SWindow>> Windows;
            Slate.GetAllVisibleWindowsOrdered(Windows);
            for (const auto& VisibleWindow : Windows) { Widgets.Add(VisibleWindow); }
            for (int32 I = 0; I < Widgets.Num() && I < 8192; ++I)
            {
                const TSharedRef<SWidget> Widget = Widgets[I];
                if (Widget->GetType() == TEXT("STextBlock"))
                {
                    const FString Label = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
                    Labels.Add(Label);
                    bFoundFormat |= Label == TEXT("Format Graph");
                }
                FChildren* Children = Widget->GetChildren();
                for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
            }
        }
        if (!Test.TestTrue(TEXT("Actual graph context menu includes Format Graph"), bFoundFormat))
        {
            Test.AddInfo(FString::Join(Labels, TEXT(" | ")).Left(2000));
        }
        return Finish();
    }
private:
    bool Finish()
    {
        if (Editor) { Editor->GetGraphPanel()->DismissContextMenu(); }
        if (BlueprintEditor) { BlueprintEditor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); }
        Editor.Reset(); BlueprintEditor.Reset(); Fixture.Reset();
        return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<IBlueprintEditor> BlueprintEditor;
    TSharedPtr<SGraphEditor> Editor;
    int32 Frames = 0;
    bool bSummoned = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintMenuTest, "GlooPrint.Editor.NativeBlueprintContextMenu",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintMenuTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FBlueprintMenuCheck(*this));
    return true;
}

class FVisibleGeometryCheck final : public IAutomationLatentCommand
{
public:
    explicit FVisibleGeometryCheck(FAutomationTestBase& InTest, SGraphEditor::EPinVisibility Visibility = SGraphEditor::Pin_Show, float InLayoutScale = 0)
        : Test(InTest), RequestedLayoutScale(InLayoutScale)
    {
        Options.PinVisibility = Visibility;
    }

    virtual ~FVisibleGeometryCheck() { RestoreScale(); }

    virtual bool Update() override
    {
        if (!Fixture)
        {
            Fixture = MakeUnique<FFixture>();
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Editor->SetPinVisibility(Options.PinVisibility);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint geometry fixture")))
                .ClientSize(FVector2f(1280, 850)).FocusWhenFirstShown(false)
                [ Editor.ToSharedRef() ];
            auto& Slate = FSlateApplication::Get();
            Slate.AddWindow(Window.ToSharedRef());
            if (RequestedLayoutScale > 0)
            {
                OriginalApplicationScale = Slate.GetApplicationScale(); bRestoreScale = true;
                Slate.SetApplicationScale(RequestedLayoutScale / Window->GetDPIScaleFactor());
                Test.AddInfo(FString::Printf(TEXT("Painted display-scale fixture: requested %.2f, native DPI %.2f, application scale %.3f, pin mode %d."),
                    double(RequestedLayoutScale), double(Window->GetDPIScaleFactor()), double(Slate.GetApplicationScale()), int32(Options.PinVisibility)));
            }
            Editor->SetViewLocation(FVector2f(100000, 100000), 0.25f);
            Editor->SetNodeSelection(Fixture->Branch, true);
            return false;
        }
        if (++Frames < 10)
        {
            return false;
        }
        if (FormatPhase != 0)
        {
            if (FormatPhase == 1)
            {
                if (BeforeFormat == SerializeNodes(*Fixture->Graph))
                {
                    if (Frames < 120) { return false; }
                    Test.AddError(TEXT("Hidden-pin F did not finish within120 frames.")); return Finish();
                }
                Formatted = SerializeNodes(*Fixture->Graph);
                Test.TestTrue(TEXT("F formats graph in its hidden-pin mode"), BeforeFormat != Formatted);
                Test.TestEqual(TEXT("Hidden-pin format creates one undo entry"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
                FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
                FormatPhase = 2; Frames = 0; return false;
            }
            Test.TestTrue(TEXT("Hidden-pin second format is unchanged"), Formatted == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Hidden-pin no-op creates no undo entry"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            Test.TestTrue(TEXT("Hidden-pin layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Hidden-pin undo restores exact original graph"), BeforeFormat == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Hidden-pin layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Hidden-pin redo restores exact formatted graph"), Formatted == SerializeNodes(*Fixture->Graph));
            return Finish();
        }
        FString Reason;
        const float LayoutScale = Editor->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale();
        if (RequestedLayoutScale > 0 && !Test.TestTrue(TEXT("Painted editor reaches the requested display scale"),
            FMath::IsNearlyEqual(LayoutScale, RequestedLayoutScale, 0.001f))) { return Finish(); }
        if (!bMeasuredOffscreen)
        {
            FVector2f Before;
            float ZoomBefore;
            Editor->GetViewLocation(Before, ZoomBefore);
            const TArray<uint8> BeforeNodes = SerializeNodes(*Fixture->Graph);
            TArray<TSharedPtr<SGraphNode>> WidgetReferences;
            for (const UEdGraphNode* Node : Fixture->Graph->Nodes) { WidgetReferences.Add(Node->DEPRECATED_NodeWidget.Pin()); }
            if (!Test.TestTrue(TEXT("Initially offscreen and zoomed-out graph measures"), MeasureGraph(Fixture->Graph, LayoutScale, Offscreen, Reason, Options)))
            {
                Test.AddError(Reason);
                return Finish();
            }
            FLayoutGraph Snapshot;
            if (!Test.TestTrue(TEXT("Offscreen graph captures for formatting"), CaptureGraph(Fixture->Graph, LayoutScale, {Fixture->Branch->NodeGuid}, Snapshot, Reason, Options)) ||
                !Test.TestTrue(TEXT("Offscreen graph computes layout"), ComputeLayout(Snapshot, {}, OffscreenLayout, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            FVector2f After;
            float ZoomAfter;
            Editor->GetViewLocation(After, ZoomAfter);
            Test.TestEqual(TEXT("Measurement preserves viewport"), Before, After);
            Test.TestEqual(TEXT("Measurement preserves zoom"), ZoomBefore, ZoomAfter);
            Test.TestTrue(TEXT("Measurement preserves selected branch"), Editor->GetSelectedNodes().Contains(Fixture->Branch));
            Test.TestEqual(TEXT("Measurement preserves selection count"), Editor->GetSelectedNodes().Num(), 1);
            Test.TestTrue(TEXT("Open graph nodes and pins unchanged"), BeforeNodes == SerializeNodes(*Fixture->Graph));
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I)
            {
                Test.TestTrue(TEXT("Measurement preserves native node-widget reference"), Fixture->Graph->Nodes[I]->DEPRECATED_NodeWidget.Pin() == WidgetReferences[I]);
            }
            Editor->SetViewLocation(FVector2f(-60, -170), 1.0f);
            bMeasuredOffscreen = true;
            Frames = 0;
            return false;
        }

        FGraphMeasurement Visible;
        if (!Test.TestTrue(TEXT("Visible graph measures"), MeasureGraph(Fixture->Graph, LayoutScale, Visible, Reason, Options)))
        {
            Test.AddError(Reason);
            return Finish();
        }
        Compare(Test, Offscreen, Visible);
        FLayoutGraph VisibleSnapshot;
        FLayoutResult VisibleLayout;
        if (Test.TestTrue(TEXT("Visible graph captures for formatting"), CaptureGraph(Fixture->Graph, LayoutScale, {Fixture->Branch->NodeGuid}, VisibleSnapshot, Reason, Options)) &&
            Test.TestTrue(TEXT("Visible graph computes layout"), ComputeLayout(VisibleSnapshot, {}, VisibleLayout, Reason)))
        {
            Test.TestTrue(TEXT("Offscreen formatting and visible formatting choose identical positions"), VisibleLayout.Positions == OffscreenLayout.Positions);
            Test.TestTrue(TEXT("Offscreen formatting and visible formatting choose identical comment bounds"), VisibleLayout.Sizes == OffscreenLayout.Sizes);
        }
        else { Test.AddError(Reason); }
        SGraphPanel* Panel = Editor->GetGraphPanel();
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const TSharedPtr<SGraphNode> Native = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Native panel has the fixture node"), Native.IsValid()))
            {
                continue;
            }
            const FMeasuredNode& Measured = Find(Visible, Node->NodeGuid);
            Test.TestTrue(*FString::Printf(TEXT("%s: body matches painted native widget (measured %s, native %s, scale %.3f)"), *Node->GetName(),
                *Measured.BodySize.ToString(), *FVector2f(Native->GetCachedGeometry().GetLocalSize()).ToString(),
                Native->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale()),
                Measured.BodySize.Equals(FVector2f(Native->GetCachedGeometry().GetLocalSize()), 0.1f));
            if (Node == Fixture->Branch)
            {
                const SNodePanel::SNode::FNodeSlot* Bubble = Native->GetSlot(ENodeZone::TopCenter);
                const FVector2f BubbleTop = Native->GetCachedGeometry().AbsoluteToLocal(
                    Bubble->GetWidget()->GetCachedGeometry().LocalToAbsolute(FVector2f::ZeroVector));
                Test.TestTrue(TEXT("Visual clearance includes painted comment bubble"),
                    Measured.VisualBounds.Min.Y <= BubbleTop.Y + 0.1f && Measured.VisualBounds.Min.Y < 0);
            }
            TArray<TSharedRef<SWidget>> Pins;
            Native->GetPins(Pins);
            TSet<FGuid> PaintedPins;
            for (const TSharedRef<SWidget>& PinWidget : Pins)
            {
                const UEdGraphPin* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj();
                const FMeasuredPin* MeasuredPin = Measured.Pins.FindByPredicate([Pin](const FMeasuredPin& P) { return P.Id == Pin->PinId; });
                if (!MeasuredPin || !MeasuredPin->AttachmentOffset.IsSet())
                {
                    continue;
                }
                const FGeometry& Geometry = PinWidget->GetCachedGeometry();
                const FVector2f Size(Geometry.GetLocalSize());
                if (Size.X <= 0 || Size.Y <= 0) { continue; }
                PaintedPins.Add(Pin->PinId);
                const FVector2f Attachment = Native->GetCachedGeometry().AbsoluteToLocal(
                    Geometry.LocalToAbsolute(FVector2f(Pin->Direction == EGPD_Output ? Size.X : 0, Size.Y * 0.5f)));
                Test.TestTrue(*FString::Printf(TEXT("%s.%s: attachment matches painted native pin (measured %s, native %s)"),
                    *Node->GetName(), *Pin->PinName.ToString(), *MeasuredPin->AttachmentOffset.GetValue().ToString(), *Attachment.ToString()),
                    MeasuredPin->AttachmentOffset.GetValue().Equals(Attachment, 0.1f));
            }
            for (const FMeasuredPin& Pin : Measured.Pins)
            {
                if (Pin.AttachmentOffset.IsSet()) { Test.TestTrue(TEXT("Every measured attachment has a painted native counterpart"), PaintedPins.Contains(Pin.Id)); }
            }
        }
        TArray<FColor> Pixels;
        FIntVector Size;
        if (FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))
        {
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            const FString Name = RequestedLayoutScale > 0 ?
                FString::Printf(TEXT("GlooPrint-Geometry-Scale%.2f-PinMode%d.png"), double(RequestedLayoutScale), int32(Options.PinVisibility)) :
                Options.PinVisibility == SGraphEditor::Pin_Show ? TEXT("GlooPrint-Geometry.png") :
                FString::Printf(TEXT("GlooPrint-Geometry-PinMode%d.png"), int32(Options.PinVisibility));
            const FString Path = FPaths::ProjectSavedDir() / Name;
            Test.TestTrue(TEXT("Save fixture screenshot"), FFileHelper::SaveArrayToFile(Png, *Path));
            Test.AddInfo(FString::Printf(TEXT("Native fixture screenshot: %s"), *Path));
        }
        else
        {
            Test.AddError(TEXT("Could not capture the painted native fixture."));
        }
        if (Options.PinVisibility != SGraphEditor::Pin_Show || RequestedLayoutScale > 0)
        {
            BeforeFormat = SerializeNodes(*Fixture->Graph);
            FormatQueue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            auto& Slate = FSlateApplication::Get();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            FormatPhase = 1; Frames = 0; return false;
        }
        return Finish();
    }

private:
    void RestoreScale()
    {
        if (bRestoreScale)
        {
            FSlateApplication::Get().SetApplicationScale(OriginalApplicationScale); bRestoreScale = false;
        }
    }
    bool Finish()
    {
        RestoreScale();
        if (RequestedLayoutScale > 0) { Test.TestEqual(TEXT("Display-scale fixture restores the original application scale"),
            FSlateApplication::Get().GetApplicationScale(), OriginalApplicationScale); }
        Window->RequestDestroyWindow();
        Editor.Reset();
        Window.Reset();
        Fixture.Reset();
        return true;
    }

    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FGraphMeasurement Offscreen;
    FLayoutResult OffscreenLayout;
    FMeasurementOptions Options;
    TArray<uint8> BeforeFormat, Formatted;
    int32 Frames = 0, FormatPhase = 0, FormatQueue = 0;
    const float RequestedLayoutScale;
    float OriginalApplicationScale = 1;
    bool bMeasuredOffscreen = false, bRestoreScale = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVisibleMeasurementTest, "GlooPrint.Measurement.OffscreenVersusPaintedNative",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVisibleMeasurementTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FVisibleGeometryCheck(*this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPaintedDisplayScalesTest, "GlooPrint.Measurement.PaintedDisplayScales",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPaintedDisplayScalesTest::RunTest(const FString& Parameters)
{
    for (float Scale : {1.f, 1.25f, 1.5f})
    for (const auto Visibility : {SGraphEditor::Pin_Show, SGraphEditor::Pin_HideNoConnection, SGraphEditor::Pin_HideNoConnectionNoDefault})
    {
        ADD_LATENT_AUTOMATION_COMMAND(FVisibleGeometryCheck(*this, Visibility, Scale));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiddenPinsMeasurementTest, "GlooPrint.Measurement.HiddenUnconnectedPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiddenPinsMeasurementTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FVisibleGeometryCheck(*this, SGraphEditor::Pin_HideNoConnection));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiddenDefaultsMeasurementTest, "GlooPrint.Measurement.HiddenPinsWithoutDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiddenDefaultsMeasurementTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FVisibleGeometryCheck(*this, SGraphEditor::Pin_HideNoConnectionNoDefault));
    return true;
}
}

#endif
