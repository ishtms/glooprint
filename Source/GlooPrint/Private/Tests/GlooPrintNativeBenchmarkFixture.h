// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"

namespace GlooPrint::Tests
{
inline bool PopulateNativeChain(FAutomationTestBase& Test, FFixture& Fixture, int32 Count, int32 Outputs,
    UK2Node_ExecutionSequence*& Entry, int32& Pins)
{
    UK2Node_ExecutionSequence* Previous = nullptr;
    for (int32 I = 0; I < Count; ++I)
    {
        auto* Node = Fixture.Add<UK2Node_ExecutionSequence>({float(((I * 7) % 10) * 512), float((I / 10) * (Outputs * 32 + 160))});
        Node->NodeGuid = FGuid(0, 0, 0, I + 1);
        for (int32 P = 2; P < Outputs; ++P) { Node->AddInputPin(); }
        if (Previous && !Fixture.Graph->GetSchema()->TryCreateConnection(Previous->GetThenPinGivenIndex(0), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))
        {
            Test.AddError(TEXT("Native schema refused the benchmark chain.")); return false;
        }
        if (!Entry) { Entry = Node; }
        for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
        Pins += Node->Pins.Num(); Previous = Node;
    }
    return true;
}
inline bool PopulateNativeFan(FAutomationTestBase& Test, FFixture& Fixture, int32 Count,
    UEdGraphNode*& Entry, int32& Pins)
{
    UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralInt"));
    if (!Test.TestNotNull(TEXT("Native integer literal function exists"), Function)) { return false; }
    UEdGraphPin* Source = nullptr;
    for (int32 I = 0; I < Count; ++I)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture.Graph);
        Node->SetFromFunction(Function);
        Fixture.Initialize(*Node, {float(((I * 7) % 10) * 512), float((I / 10) * 224)});
        Node->PostPlacedNewNode(); Node->NodeGuid = FGuid(0, 0, 0, I + 1);
        if (!Entry) { Entry = Node; Source = Node->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue); }
        else if (!Fixture.Graph->GetSchema()->TryCreateConnection(Source, Node->FindPinChecked(TEXT("Value"))))
        {
            Test.AddError(TEXT("Native schema refused the data fan.")); return false;
        }
        for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
        Pins += Node->Pins.Num();
    }
    return true;
}

}
#endif
