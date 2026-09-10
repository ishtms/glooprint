// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/HittestGrid.h"
#include "SGraphPanel.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
DEFINE_LOG_CATEGORY_STATIC(LogGlooPrintTestActivation, Log, All);

void RequestEditorActivation()
{
#if PLATFORM_MAC
    FPlatformApplicationMisc::ActivateApplication();
    UE_LOG(LogGlooPrintTestActivation, Display, TEXT("Requested Mac editor activation; waiting for foreground confirmation."));
#else
    auto& Slate = FSlateApplication::Get();
    TSharedPtr<SWindow> Window = Slate.GetActiveTopLevelRegularWindow();
    if (!Window)
    {
        for (const auto& Candidate : Slate.GetTopLevelWindows())
        {
            if (Candidate->IsRegularWindow() && Candidate->IsVisible())
            {
                Window = Candidate;
                break;
            }
        }
    }
    if (Window)
    {
        Window->BringToFront(true);
        UE_LOG(LogGlooPrintTestActivation, Display, TEXT("Requested foreground for editor window '%s'; waiting for foreground confirmation."), *Window->GetTitle().ToString());
    }
    else
    {
        UE_LOG(LogGlooPrintTestActivation, Warning, TEXT("No regular editor window is available for foreground activation."));
    }
#endif
}

void MoveMouseOverGraph(FAutomationTestBase& Test, const TSharedRef<SWindow>& Window, SGraphPanel& Panel, FVector2f ScreenPosition)
{
    auto& Slate = FSlateApplication::Get();
    Slate.SetCursorPos(FVector2D(ScreenPosition));
    const FPointerEvent Event(0, ScreenPosition, ScreenPosition, TSet<FKey>(), EKeys::Invalid, 0, FModifierKeysState());
    Slate.ProcessMouseMoveEvent(Event, false);
    const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(ScreenPosition, 0, false, 0));
    if (!Test.TestTrue(TEXT("Painted hit grid identifies the fixture graph under the test cursor"), Path.ContainsWidget(&Panel))) { return; }
    Slate.RoutePointerMoveEvent(Path, Event, false);
    Test.TestTrue(TEXT("Slate pointer routing reaches the fixture graph"), Panel.IsHovered());
}

namespace
{
class FTransactionValueWriter final : public FObjectWriter
{
public:
    explicit FTransactionValueWriter(TArray<uint8>& Bytes) : FObjectWriter(Bytes)
    {
        ArNoDelta = true;
        ArPortFlags |= PPF_DuplicateVerbatim;
    }
    virtual FArchive& operator<<(FName& Name) override
    {
        FNameEntryId Identity = Name.GetComparisonIndex();
        int32 Number = Name.GetNumber();
        ByteOrderSerialize(&Identity, sizeof(Identity));
        ByteOrderSerialize(&Number, sizeof(Number));
        return *this;
    }
};
}

TArray<uint8> SerializeTransactionValues(const UEdGraph& Graph)
{
    TArray<uint8> Bytes;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        TArray<uint8> NodeBytes;
        FTransactionValueWriter Writer(NodeBytes);
        Node->Serialize(Writer);
        Bytes.Append(NodeBytes);
    }
    return Bytes;
}

TArray<uint8> SerializeNodes(const UEdGraph& Graph, TMap<FGuid, TArray<uint8>>* PerNode)
{
    TArray<uint8> Bytes;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        TArray<uint8> NodeBytes;
        FObjectWriter Writer(Node, NodeBytes, false, false, false, PPF_DuplicateVerbatim);
        Bytes.Append(NodeBytes);
        if (PerNode) { PerNode->Add(Node->NodeGuid, MoveTemp(NodeBytes)); }
    }
    return Bytes;
}

TMap<FString, FString> DescribeNodes(const UEdGraph& Graph)
{
    TMap<FString, FString> Values;
    for (UEdGraphNode* Node : Graph.Nodes)
    {
        for (TFieldIterator<FProperty> Property(Node->GetClass()); Property; ++Property)
        {
            FString Value;
            Property->ExportText_InContainer(0, Value, Node, nullptr, Node, PPF_DuplicateVerbatim);
            Values.Add(Node->GetName() + TEXT(".") + Property->GetName(), MoveTemp(Value));
        }
        for (int32 I = 0; I < Node->Pins.Num(); ++I)
        {
            FString Value;
            Node->Pins[I]->ExportTextItem(Value, PPF_DuplicateVerbatim);
            Values.Add(FString::Printf(TEXT("%s.Pin%d"), *Node->GetName(), I), MoveTemp(Value));
            Values.Add(FString::Printf(TEXT("%s.Pin%d.Tooltip"), *Node->GetName(), I), Node->Pins[I]->PinToolTip);
            Values.Add(FString::Printf(TEXT("%s.Pin%d.SourceIndex"), *Node->GetName(), I), LexToString(Node->Pins[I]->SourceIndex));
        }
    }
    return Values;
}

void ReportNodeDifferences(FAutomationTestBase& Test, const TMap<FString, FString>& Before, const UEdGraph& Graph,
    const TMap<FGuid, TArray<uint8>>* BeforeBytes)
{
    const auto After = DescribeNodes(Graph);
    for (const auto& Value : Before)
    {
        const FString* Current = After.Find(Value.Key);
        if (!Current || *Current != Value.Value)
        {
            Test.AddInfo(Value.Key + TEXT(": before ") + Value.Value + TEXT("; after ") + (Current ? *Current : TEXT("<missing>")));
        }
    }
    if (BeforeBytes)
    {
        TMap<FGuid, TArray<uint8>> Current;
        SerializeNodes(Graph, &Current);
        for (UEdGraphNode* Node : Graph.Nodes)
        {
            const auto* Old = BeforeBytes->Find(Node->NodeGuid);
            const auto& Now = Current.FindChecked(Node->NodeGuid);
            if (Old && *Old != Now)
            {
                int32 Offset = 0;
                while (Offset < FMath::Min(Old->Num(), Now.Num()) && (*Old)[Offset] == Now[Offset]) { ++Offset; }
                const int32 Start = FMath::Max(0, Offset - 16);
                Test.AddInfo(FString::Printf(TEXT("%s serialized bytes differ at %d (sizes %d/%d); before %s; after %s"),
                    *Node->GetName(), Offset, Old->Num(), Now.Num(),
                    *BytesToHex(Old->GetData() + Start, FMath::Min(48, Old->Num() - Start)),
                    *BytesToHex(Now.GetData() + Start, FMath::Min(48, Now.Num() - Start))));
            }
        }
    }
}

const FMeasuredNode& Find(const FGraphMeasurement& Measurement, const FGuid& Id)
{
    const FMeasuredNode* Result = Measurement.Nodes.FindByPredicate([&Id](const FMeasuredNode& Node) { return Node.Id == Id; });
    check(Result);
    return *Result;
}

void Compare(FAutomationTestBase& Test, const FGraphMeasurement& A, const FGraphMeasurement& B)
{
    if (!Test.TestEqual(TEXT("Node count"), A.Nodes.Num(), B.Nodes.Num()))
    {
        return;
    }
    for (int32 Index = 0; Index < A.Nodes.Num(); ++Index)
    {
        const FMeasuredNode& Left = A.Nodes[Index];
        const FMeasuredNode& Right = B.Nodes[Index];
        Test.TestEqual(TEXT("Stable node order"), Left.Id, Right.Id);
        Test.TestEqual(TEXT("Body dimensions"), Left.BodySize, Right.BodySize);
        Test.TestEqual(TEXT("Visual minimum"), Left.VisualBounds.Min, Right.VisualBounds.Min);
        Test.TestEqual(TEXT("Visual maximum"), Left.VisualBounds.Max, Right.VisualBounds.Max);
        if (!Test.TestEqual(TEXT("Pin count"), Left.Pins.Num(), Right.Pins.Num()))
        {
            continue;
        }
        for (int32 PinIndex = 0; PinIndex < Left.Pins.Num(); ++PinIndex)
        {
            Test.TestEqual(TEXT("Original pin order"), Left.Pins[PinIndex].Id, Right.Pins[PinIndex].Id);
            Test.TestTrue(TEXT("Same pin attachment with fresh widgets"),
                Left.Pins[PinIndex].AttachmentOffset == Right.Pins[PinIndex].AttachmentOffset);
        }
    }
}

}
#endif
