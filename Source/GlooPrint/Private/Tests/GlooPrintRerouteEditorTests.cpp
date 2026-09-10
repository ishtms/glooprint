// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FRerouteCheck final : public IAutomationLatentCommand
{
public:
    explicit FRerouteCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FRerouteCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->bFormattingEnabled = true; Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("ExistingReroutesExample");
            Exec.Add(Knot({700, 60})); Exec.Add(Knot({150, 250}));
            auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>({600, 200});
            Exec.Add(Knot({250, 400}));
            First = Call(TEXT("PrintString"), {850, 200});
            auto* Second = Call(TEXT("PrintString"), {400, 600});
            Exec.Add(Knot({50, 600}));
            auto* Join = Call(TEXT("PrintString"), {600, 350});
            auto* Shared = Call(TEXT("MakeLiteralString"), {300, 500});
            Shared->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("Shared through existing reroutes");
            Data.Add(Knot({750, 480})); Data.Add(Knot({100, 520})); Data.Add(Knot({600, 640}));
            Data[1]->NodeComment = TEXT("Shared value"); Data[1]->bCommentBubbleVisible = true; Data[1]->bCommentBubblePinned = true;
            Link(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Exec[0]->GetInputPin());
            Link(Exec[0]->GetOutputPin(), Exec[1]->GetInputPin());
            Link(Exec[1]->GetOutputPin(), Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Sequence->GetThenPinGivenIndex(0), Exec[2]->GetInputPin());
            Link(Exec[2]->GetOutputPin(), First->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Sequence->GetThenPinGivenIndex(1), Second->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(First->FindPinChecked(UEdGraphSchema_K2::PN_Then), Exec[3]->GetInputPin());
            Link(Second->FindPinChecked(UEdGraphSchema_K2::PN_Then), Exec[3]->GetInputPin());
            Link(Exec[3]->GetOutputPin(), Join->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            Link(Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Data[0]->GetInputPin());
            Link(Data[0]->GetOutputPin(), Data[1]->GetInputPin());
            Link(Data[1]->GetOutputPin(), First->FindPinChecked(TEXT("InString")));
            Link(Data[1]->GetOutputPin(), Second->FindPinChecked(TEXT("InString")));
            Link(Data[1]->GetOutputPin(), Data[2]->GetInputPin());
            Link(Data[2]->GetOutputPin(), Join->FindPinChecked(TEXT("InString")));
            if (!bConnectionsValid) { return Finish(); }
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Connected execution and data reroutes compile as native Actor Blueprint"), Compile.NumErrors, 0)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint existing execution and data reroutes")))
                .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-100, -100), 1);
            Editor->SetNodeSelection(Entry, true); Slate.SetCursorPos(FVector2D::ZeroVector);
            Deadline = FPlatformTime::Seconds() + 40; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Connected reroute fixture did not settle in 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel(); const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph); Properties = DescribeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Connected reroutes have a complete format plan"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            Test.TestTrue(TEXT("Reroute planning leaves native data unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Plan includes every existing reroute and ordinary node"), Plan.Snapshot.Nodes.Num(), 13);
            Test.TestEqual(TEXT("Plan preserves all fifteen original links"), Plan.Routes.Wires.Num(), 15);
            Test.TestEqual(TEXT("Connected reroutes need no native fallback wires"), Plan.Routes.FallbackCount, 0);
            Test.AddInfo(FString::Printf(TEXT("Native reroutes: %d spacing repairs, %d fallbacks."), Plan.SpacingRepairs, Plan.Routes.FallbackCount));
            CheckGeometry(false); CheckWaypoints();
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats through existing reroutes"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 5; return false;
        }
        if (Phase == 5)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph); Test.TestTrue(TEXT("F moves the scattered reroute graph"), Before != After);
            Test.TestEqual(TEXT("Reroute layout is one undo step"), GEditor->Trans->GetQueueLength(), Queue + 1);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const FString* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" preserved while formatting reroutes"), Value && *Value == Pair.Value);
            }
            Test.TestTrue(TEXT("Reroute layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Reroute undo restores every serialized value"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Reroute layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Reroute redo restores every serialized value"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            CheckLinks();
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Live wire passes through its original knot pin pair"), Live && Live->Points == Pair.Value.Points);
            }
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Connected reroutes replan with a cold measurement"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Reroute waypoint positions are cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
                for (const auto& Pair : Plan.Routes.Wires)
                {
                    const auto* Wire = Cold.Routes.Wires.Find(Pair.Key);
                    Test.TestTrue(TEXT("Reroute paths are cold-idempotent"), Wire && Wire->Points == Pair.Value.Points);
                }
            }
            Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            Test.TestTrue(TEXT("Repeated F leaves reroute data exactly unchanged"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Reroute no-op adds no undo step"), GEditor->Trans->GetQueueLength(), Queue);
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Reroute F preserves camera"), AfterView, View); Test.TestEqual(TEXT("Reroute F preserves zoom"), AfterZoom, Zoom);
            Editor->ZoomToFit(false); CaptureAfter = FPlatformTime::Seconds() + 1;
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            if (FPlatformTime::Seconds() < CaptureAfter) { return false; }
            CheckGeometry(true);
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture connected native reroutes"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save connected reroute capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-ConnectedReroutes.png"))));
            }
            Builds = Cache->GetBuildCount(); Hover(Exec[0]->GetOutputPin(), Exec[1]->GetInputPin(), *Cache); Phase = 3; Frames = 0; return false;
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        auto* From = Phase == 3 ? Exec[0]->GetOutputPin() : Data[1]->GetOutputPin();
        auto* To = Phase == 3 ? Exec[1]->GetInputPin() : First->FindPinChecked(TEXT("InString"));
        Test.TestTrue(TEXT("Visible wire hover identifies the exact original reroute pin pair"),
            Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) && ((A == From && B == To) || (A == To && B == From)));
        Test.TestEqual(TEXT("Reroute wire hover and paint reuse cached routes"), Cache->GetBuildCount(), Builds);
        if (Phase == 3) { Hover(Data[1]->GetOutputPin(), First->FindPinChecked(TEXT("InString")), *Cache); Phase = 4; Frames = 0; return false; }
        return Finish();
    }
private:
    void CheckGeometry(bool bFormatted)
    {
        auto* Panel = Editor->GetGraphPanel();
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const auto Native = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Reroute fixture node has native arranged geometry"), Native.IsValid())) { continue; }
            TArray<TSharedRef<SWidget>> Widgets; Native->GetPins(Widgets);
            for (const auto& Widget : Widgets)
            {
                auto* Pin = StaticCastSharedRef<SGraphPin>(Widget)->GetPinObj();
                if (Pin->LinkedTo.IsEmpty()) { continue; }
                const auto* Measured = Plan.Snapshot.Pins.FindByPredicate([Pin](const auto& P) { return P.Id == Pin->PinId; });
                if (!Test.TestTrue(TEXT("Native pin is retained in measurement"), Measured && Measured->Offset.IsSet())) { continue; }
                const auto& G = Widget->GetCachedGeometry(); const FVector2f Size(G.GetLocalSize());
                const FVector2f Screen = G.LocalToAbsolute(FVector2f(Pin->Direction == EGPD_Output ? Size.X : 0, Size.Y * 0.5f));
                if (!bFormatted)
                {
                    Test.TestTrue(TEXT("Native pin widgets retain exact measured attachments"),
                        Measured->Offset.GetValue().Equals(Native->GetCachedGeometry().AbsoluteToLocal(Screen), 0.1f));
                }
                else
                {
                    const FVector2f Point = Panel->GetCachedGeometry().AbsoluteToLocal(Screen) / Panel->GetZoomAmount() + FVector2f(Panel->GetViewOffset());
                    for (const auto& Pair : Plan.Routes.Wires)
                    {
                        const bool bStart = Pair.Key.FromPin == Pin->PinId;
                        if (!bStart && Pair.Key.ToPin != Pin->PinId) { continue; }
                        const auto& Region = bStart ? Pair.Value.StartRegion : Pair.Value.EndRegion;
                        if (!Test.TestTrue(TEXT("Every painted endpoint remains in its validated wire attachment region"), Region.IsInsideOrOn(Point)))
                        {
                            Test.AddInfo(FString::Printf(TEXT("%s.%s %s: native(%.3f,%.3f), region(%.3f,%.3f)-(%.3f,%.3f), zoom%.3f."),
                                *Node->GetName(), *Pin->PinName.ToString(), bStart ? TEXT("start") : TEXT("end"), Point.X, Point.Y,
                                Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y, Panel->GetZoomAmount()));
                        }
                    }
                }
            }
        }
    }
    void CheckWaypoints()
    {
        for (const auto& Edge : Plan.Snapshot.Edges)
        {
            const auto& A = Plan.Snapshot.Pins[Edge.From]; const auto& B = Plan.Snapshot.Pins[Edge.To];
            const FVector2f Start = FVector2f(Plan.Layout.Positions[A.Node]) + A.Offset.GetValue();
            const FVector2f End = FVector2f(Plan.Layout.Positions[B.Node]) + B.Offset.GetValue();
            Test.TestTrue(TEXT("Existing execution and data waypoint chains keep forward dependency order"), End.X > Start.X);
            const auto* Route = Plan.Routes.Wires.Find({Plan.Snapshot.Nodes[A.Node].Geometry.Id, A.Id, Plan.Snapshot.Nodes[B.Node].Geometry.Id, B.Id});
            Test.TestTrue(TEXT("Every original link terminates on its existing waypoint pins"), Route && !Route->Points.IsEmpty() && Route->Points[0].Equals(Start, 0.1f) && Route->Points.Last().Equals(End, 0.1f));
        }
        for (int32 I = 0; I < 2; ++I)
        {
            const auto& A = *Plan.Snapshot.Pins.FindByPredicate([this, I](const auto& P) { return P.Id == Exec[I]->GetOutputPin()->PinId; });
            const auto& B = *Plan.Snapshot.Pins.FindByPredicate([this, I](const auto& P) { return P.Id == Exec[I]->GetOutputPin()->LinkedTo[0]->PinId; });
            Test.TestTrue(TEXT("Primary execution stays straight through reroute chain"), FMath::Abs(Plan.Layout.Positions[A.Node].Y + A.Offset.GetValue().Y - Plan.Layout.Positions[B.Node].Y - B.Offset.GetValue().Y) <= 0.5f);
        }
    }
    void CheckLinks()
    {
        Test.TestEqual(TEXT("Formatting preserves all thirteen native nodes"), Fixture->Graph->Nodes.Num(), 13);
        for (auto* Knot : Knots) { Test.TestTrue(TEXT("The same existing knot object survives F and undo/redo"), Fixture->Graph->Nodes.Contains(Knot)); }
        for (const auto& Pair : Plan.Routes.Wires)
        {
            const auto* FromNode = Fixture->Graph->Nodes.FindByPredicate([&Pair](const auto& N) { return N->NodeGuid == Pair.Key.FromNode; });
            const auto* ToNode = Fixture->Graph->Nodes.FindByPredicate([&Pair](const auto& N) { return N->NodeGuid == Pair.Key.ToNode; });
            const auto* From = FromNode ? (*FromNode)->FindPinById(Pair.Key.FromPin) : nullptr;
            const auto* To = ToNode ? (*ToNode)->FindPinById(Pair.Key.ToPin) : nullptr;
            Test.TestTrue(TEXT("Original reroute links remain reciprocal without reconnection"), From && To && From->LinkedTo.Contains(To) && To->LinkedTo.Contains(From));
        }
        Test.TestEqual(TEXT("Shared execution knot still has both incoming branches"), Exec[3]->GetInputPin()->LinkedTo.Num(), 2);
        Test.TestEqual(TEXT("Shared data knot still has all three consumers"), Data[1]->GetOutputPin()->LinkedTo.Num(), 3);
    }
    void Hover(UEdGraphPin* A, UEdGraphPin* B, const FRouteCache& Cache)
    {
        const auto* Route = Cache.GetRoutes().Wires.Find({A->GetOwningNode()->NodeGuid, A->PinId, B->GetOwningNode()->NodeGuid, B->PinId});
        if (!Test.TestTrue(TEXT("Reroute hover has a custom wire"), Route && !Route->Curves.IsEmpty())) { return; }
        auto* Panel = Editor->GetGraphPanel(); const FVector2f Point = EvaluateRoute(*Route, Route->Length * 0.5f);
        MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, Panel->GetCachedGeometry().LocalToAbsolute((Point - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount()));
    }
    UK2Node_Knot* Knot(FVector2f Position)
    {
        auto* Node = Fixture->Add<UK2Node_Knot>(Position); Knots.Add(Node); return Node;
    }
    UK2Node_CallFunction* Call(FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Name));
        Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native schema accepts connected reroute link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B));
    }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
        Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); Restore(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_CustomEvent* Entry = nullptr;
    UK2Node_CallFunction* First = nullptr;
    TArray<UK2Node_Knot*> Knots, Exec, Data;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    TMap<FString, FString> Properties;
    FVector2D OriginalCursor;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0, CaptureAfter = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, Queue = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRerouteEditorTest, "GlooPrint.Editor.ConnectedRerouteWaypoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRerouteEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FRerouteCheck(*this)); return true;
}
}
#endif
