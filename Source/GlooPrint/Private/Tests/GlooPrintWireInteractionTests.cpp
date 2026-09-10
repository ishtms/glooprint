// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ImageUtils.h"
#include "IAutomationDriver.h"
#include "IAutomationDriverModule.h"
#include "IDriverSequence.h"
#include "Input/HittestGrid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/IToolTip.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWireInteractionCheck final : public IAutomationLatentCommand
{
public:
    FWireInteractionCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle) : Test(InTest), Style(InStyle) {}
    virtual ~FWireInteractionCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!bRequestedActivation)
        {
            FPlatformApplicationMisc::ActivateApplication(); bRequestedActivation = true;
            ActivationDeadline = FPlatformTime::Seconds() + 10;
            return false;
        }
        if (!Fixture && !FPlatformApplicationMisc::IsThisApplicationForeground())
        {
            if (FPlatformTime::Seconds() > ActivationDeadline)
            {
                Test.AddError(TEXT("Editor could not become foreground for native pointer verification.")); return Finish();
            }
            return false;
        }
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
            OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = Style; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Fixture = MakeUnique<FFixture>();
            auto* Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Target = Output->LinkedTo[0]->GetOwningNode();
            Target->SetPosition({700, 280}); Fixture->Print->SetPosition({320, 30});
            Extra = Fixture->Add<UK2Node_IfThenElse>({800, -160});
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            if (!Test.TestTrue(TEXT("Native schema accepts a second independent wire at the shared execution input"),
                GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else), InputPin()))) { return Finish(); }
            Key = {Fixture->Branch->NodeGuid, Output->PinId, Target->NodeGuid, InputPin()->PinId};
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint wire gestures"))).ClientSize(FVector2f(1300, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Window->BringToFront(true);
            auto& Module = IAutomationDriverModule::Get();
            if (!Test.TestFalse(TEXT("No other input driver is active in the disposable fixture"), Module.IsEnabled())) { return Finish(); }
            Module.Enable(); bOwnDriver = true;
            Slate.UsePlatformCursorForCursorUser(true);
            Driver = Module.CreateAsyncDriver(); StartInputSequence();
            Editor->SetViewLocation(FVector2f(-100, -220), 1.f);
            Slate.SetCursorPos(FVector2D::ZeroVector); Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Wire gesture fixture did not settle in 45 seconds.")); return Finish(); }
        if (!InputProgress->bStarted) { return false; }
        if (Phase == 17)
        {
            if (!KeyAction.GetFuture().IsReady()) { return false; }
            Test.TestTrue(TEXT("Continuous native input sequence completes"), KeyAction.GetFuture().Get());
            return Finish();
        }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel(); auto Cache = Panel->GetMetaData<FRouteCache>();
        if (Phase >= 18) { return CheckNodeDrag(*Panel, Cache); }
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 0)
        {
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    const auto PinWidget = Widget ? Widget->FindWidgetForPin(Pin) : nullptr;
                    if (PinWidget && PinWidget->GetToolTip()) { PinWidget->GetToolTip()->IsEmpty(); }
                }
                Positions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
            }
            Before = SerializeNodes(*Fixture->Graph); Builds = Cache->GetBuildCount();
            Test.TestEqual(TEXT("All original shared-input connections have custom routes"), Cache->GetRoutes().Wires.Num(), 3);
            Test.TestEqual(TEXT("Gesture fixture has no native routing fallback"), Cache->GetRoutes().FallbackCount, 0);
            if (Cache->GetRoutes().FallbackCount > 0)
            {
                FLayoutGraph Cold; FRouteSet ColdRoutes; FString Reason;
                const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
                if (CaptureGraphForRouting(Fixture->Graph, Scale, Cold, Reason) && ComputeRoutes(Cold, ColdRoutes, Reason, Style))
                {
                    Test.AddInfo(FString::Printf(TEXT("Before any F or gesture: live fallbacks=%d; independent cold fallbacks=%d."),
                        Cache->GetRoutes().FallbackCount, ColdRoutes.FallbackCount));
                    for (const auto& Pair : ColdRoutes.Wires)
                    {
                        Test.AddInfo(FString::Printf(TEXT("Cold route %s:%s -> %s:%s, reason=%d"),
                            *Pair.Key.FromNode.ToString(), *Pair.Key.FromPin.ToString(), *Pair.Key.ToNode.ToString(), *Pair.Key.ToPin.ToString(), int32(Pair.Value.Fallback)));
                        for (const auto& Point : Pair.Value.Points) { Test.AddInfo(FString::Printf(TEXT("Route point (%.3f,%.3f)"), Point.X, Point.Y)); }
                    }
                    for (const auto& Pin : Cold.Pins)
                    {
                        if (!Pin.Offset.IsSet()) { continue; }
                        const FVector2f Point = FVector2f(Cold.Nodes[Pin.Node].Geometry.Position) + Pin.Offset.GetValue();
                        Test.AddInfo(FString::Printf(TEXT("Cold pin %s:%s at (%.3f,%.3f)"),
                            *Cold.Nodes[Pin.Node].Geometry.Id.ToString(), *Pin.Id.ToString(), Point.X, Point.Y));
                    }
                    for (const auto& Node : Cold.Nodes)
                    {
                        Test.AddInfo(FString::Printf(TEXT("Cold node %s at (%d,%d), body (%.2f,%.2f)"), *Node.Geometry.Id.ToString(),
                            Node.Geometry.Position.X, Node.Geometry.Position.Y, Node.Geometry.BodySize.X, Node.Geometry.BodySize.Y));
                    }
                }
                else { Test.AddInfo(TEXT("Cold routing diagnostic: ") + Reason); }
            }
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture initial native routed wires"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const FString Name = FString::Printf(TEXT("GlooPrint-WireGestureInitial-%s.png"),
                    Style == EGlooPrintWireStyle::Rounded90 ? TEXT("rounded") : TEXT("diagonal"));
                Test.TestTrue(TEXT("Save initial native routed wires"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
            }
            if (!HoverEndpoint(*Panel, *Cache, true)) { return Finish(); }
            return Next(1);
        }
        if (Phase == 1)
        {
            if (!CheckHover(*Panel, OutputPin())) { return Finish(); }
            const int32 Queue = GEditor->Trans->GetQueueLength();
            ShiftClick(*Panel);
            const auto Marked = Panel->MarkedPin.Pin();
            Test.TestTrue(TEXT("Shift-click on a bent wire marks its nearest original output"), Marked && Marked->GetPinObj() == OutputPin());
            ShiftClick(*Panel);
            Test.TestFalse(TEXT("Shift-clicking the same wire endpoint toggles the native mark off"), Panel->MarkedPin.IsValid());
            Test.TestEqual(TEXT("Marking and unmarking a wire creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
            Test.TestTrue(TEXT("Tooltip and Shift mark preserve all graph values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Hover and marking reuse cached routes"), Cache->GetBuildCount(), Builds);
            if (!HoverEndpoint(*Panel, *Cache, false)) { return Finish(); }
            return Next(2);
        }
        if (Phase == 2)
        {
            if (!CheckHover(*Panel, InputPin())) { return Finish(); }
            const int32 Queue = AppliedTransactions();
            ShiftClick(*Panel);
            const auto MarkedInput = Panel->MarkedPin.Pin();
            Test.TestTrue(TEXT("Shift-click near the wire input marks its original input"), MarkedInput && MarkedInput->GetPinObj() == InputPin());
            ShiftClick(*Panel);
            Test.TestFalse(TEXT("Shift-clicking the input end again clears the native mark"), Panel->MarkedPin.IsValid());
            Test.TestTrue(TEXT("Input marking preserves all graph values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Input marking creates no transaction"), AppliedTransactions(), Queue);
            const FModifierKeysState Alt(false, false, false, false, true, false, false, false, false);
            PointerButton(*Panel, true, Alt); PointerButton(*Panel, false, Alt);
            if (!Test.TestTrue(TEXT("Alt-click on the rendered wire breaks only the identified connection"),
                !OutputPin()->LinkedTo.Contains(InputPin()) && OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact())) { return Finish(); }
            Test.TestEqual(TEXT("Native Alt-click creates one break-link transaction"), AppliedTransactions(), Queue + 1);
            const auto Broken = SerializeNodes(*Fixture->Graph);
            CheckUndoRedo(Before, Broken, TEXT("Alt-click"));
            Test.TestTrue(TEXT("Restore original wire for the next gesture"), GEditor->UndoTransaction());
            return Next(3);
        }
        if (Phase == 3)
        {
            const auto ExtraWidget = PinWidget(*Panel, Extra->FindPinChecked(UEdGraphSchema_K2::PN_Then));
            if (!Test.TestTrue(TEXT("Shift connection starts from a native unlinked pin"), ExtraWidget.IsValid())) { return Finish(); }
            Mouse = ExtraWidget->GetCachedGeometry().LocalToAbsolute(ExtraWidget->GetCachedGeometry().GetLocalSize() * 0.5f);
            MoveMouse(*Panel);
            const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
            if (!Test.TestTrue(TEXT("Painted hit grid resolves the first Shift-click pin"), Path.ContainsWidget(ExtraWidget.Get()))) { return Finish(); }
            ShiftClick(*Panel);
            const auto Marked = Panel->MarkedPin.Pin();
            if (!Test.TestTrue(TEXT("First Shift click marks the unlinked output"), Marked && Marked->GetPinObj() == Extra->FindPinChecked(UEdGraphSchema_K2::PN_Then))) { return Finish(); }
            if (!HoverEndpoint(*Panel, *Cache, false)) { return Finish(); }
            return Next(4);
        }
        if (Phase == 4)
        {
            if (!CheckHover(*Panel, InputPin())) { return Finish(); }
            const int32 Queue = AppliedTransactions();
            ShiftClick(*Panel);
            if (!Test.TestTrue(TEXT("Shift connection uses the wire's original input and preserves its other links"),
                Extra->FindPinChecked(UEdGraphSchema_K2::PN_Then)->LinkedTo.Contains(InputPin()) &&
                OutputPin()->LinkedTo.Contains(InputPin()) && OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact())) { return Finish(); }
            Test.TestFalse(TEXT("Completing Shift connection clears the native mark"), Panel->MarkedPin.IsValid());
            Test.TestEqual(TEXT("Native Shift connection creates one transaction"), AppliedTransactions(), Queue + 1);
            const auto Linked = SerializeNodes(*Fixture->Graph); CheckUndoRedo(Before, Linked, TEXT("Shift connection"));
            Test.TestTrue(TEXT("Restore original shared input before Control drag"), GEditor->UndoTransaction());
            ClickEmptyCanvas(*Panel);
            Test.TestTrue(TEXT("Ending native capture changes no graph values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Ending native capture creates no transaction"), AppliedTransactions(), Queue);
            return Next(5);
        }
        if (Phase == 5 || Phase == 16)
        {
            if (!HoverEndpoint(*Panel, *Cache, false)) { return Finish(); }
            return Next(Phase == 5 ? 15 : 6);
        }
        if (Phase == 15)
        {
            if (!CheckHover(*Panel, InputPin())) { return Finish(); }
            const int32 Queue = AppliedTransactions(), Nodes = Fixture->Graph->Nodes.Num();
            PointerButton(*Panel, true, FModifierKeysState()); PointerButton(*Panel, false, FModifierKeysState());
            const FPointerEvent DoubleClick(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
            Slate.ProcessMouseButtonDoubleClickEvent(Window->GetNativeWindow(), DoubleClick);
            PointerButton(*Panel, false, FModifierKeysState());
            if (!Test.TestEqual(TEXT("Full double-click inserts one native reroute"), Fixture->Graph->Nodes.Num(), Nodes + 1)) { return Finish(); }
            const auto* Knot = OutputPin()->LinkedTo.IsEmpty() ? nullptr : Cast<UK2Node_Knot>(OutputPin()->LinkedTo[0]->GetOwningNode());
            if (!Test.TestTrue(TEXT("Double-click splits only the hovered original pin pair"), Knot && Knot->GetOutputPin()->LinkedTo.Contains(InputPin()) && OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact())) { return Finish(); }
            Test.TestEqual(TEXT("Reroute insertion creates one native transaction"), AppliedTransactions(), Queue + 1);
            const auto Inserted = SerializeNodes(*Fixture->Graph); CheckUndoRedo(Before, Inserted, TEXT("Double-click reroute"));
            Test.TestTrue(TEXT("Restore the original wire before Control drag"), GEditor->UndoTransaction());
            return Next(16);
        }
        if (Phase == 6)
        {
            if (!CheckHover(*Panel, InputPin())) { return Finish(); }
            const int32 Queue = AppliedTransactions();
            const FModifierKeysState Control(false, false, true, false, false, false, false, false, false);
            PointerButton(*Panel, true, Control);
            if (!Test.TestTrue(TEXT("Control-click on the curve starts the native connection drag"), Slate.GetDragDroppingContent().IsValid())) { return Finish(); }
            Test.TestTrue(TEXT("Control drag detaches only the hovered wire from the shared input"),
                OutputPin()->LinkedTo.IsEmpty() && OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact());
            const auto Detached = SerializeNodes(*Fixture->Graph);
            const auto DropPin = PinWidget(*Panel, Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            if (!Test.TestTrue(TEXT("Native Control drag has a live drop target"), DropPin.IsValid())) { return Finish(); }
            Mouse = DropPin->GetCachedGeometry().LocalToAbsolute(DropPin->GetCachedGeometry().GetLocalSize() * 0.5f);
            const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
            if (!Test.TestTrue(TEXT("Painted hit grid resolves the actual drop pin"), Path.ContainsWidget(DropPin.Get()))) { return Finish(); }
            Slate.SetCursorPos(FVector2D(Mouse)); Mouse = Slate.GetCursorPos();
            const FPointerEvent Move(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {EKeys::LeftMouseButton}, EKeys::Invalid, 0, Control);
            Slate.ProcessMouseMoveEvent(Move, false);
            const FPointerEvent Up(FSlateApplication::CursorPointerIndex, Mouse, Mouse, TSet<FKey>(), EKeys::LeftMouseButton, 0, Control);
            Slate.ProcessMouseButtonUpEvent(Up);
            Test.TestFalse(TEXT("Native drop ends the connection drag"), Slate.GetDragDroppingContent().IsValid());
            Test.TestTrue(TEXT("Control drag moves exactly the hovered wire to its new input"),
                OutputPin()->LinkedTo.Contains(Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute)) &&
                !OutputPin()->LinkedTo.Contains(InputPin()) && OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact());
            Test.TestEqual(TEXT("Native Control move retains its break and create transactions"), AppliedTransactions(), Queue + 2);
            Moved = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Undo native drop"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Drop undo restores the exact detached state"), Detached == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Undo native detach"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Control move undo restores every original graph value"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Redo native detach"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Detach redo restores the exact detached state"), Detached == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Redo native drop"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Control move redo restores every changed graph value"), Moved == SerializeNodes(*Fixture->Graph));
            return Next(7);
        }
        if (Phase == 7)
        {
            Test.TestFalse(TEXT("Rebuilt routes discard the disconnected pin pair"), Cache->GetRoutes().Wires.Contains(Key));
            const auto* NewInput = Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            const FRouteKey NewKey{Fixture->Branch->NodeGuid, OutputPin()->PinId, Fixture->Print->NodeGuid, NewInput->PinId};
            Test.TestTrue(TEXT("Rebuilt routes include the exact new pin pair"), Cache->GetRoutes().Wires.Contains(NewKey));
            Test.TestEqual(TEXT("Gestures retain the original number of connections"), Cache->GetRoutes().Wires.Num(), 3);
            Test.TestEqual(TEXT("All connections still have custom paths"), Cache->GetRoutes().FallbackCount, 0);
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const auto* Position = Positions.Find(Node->NodeGuid);
                Test.TestTrue(TEXT("Wire gestures never format or move nodes"), Position && *Position == FIntPoint(Node->NodePosX, Node->NodePosY));
            }
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture native wire gesture result"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const FString Name = Style == EGlooPrintWireStyle::Rounded90 ? TEXT("GlooPrint-WireGestures-Rounded.png") : TEXT("GlooPrint-WireGestures-Diagonal.png");
                Test.TestTrue(TEXT("Save native wire gesture capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
            }
            Mouse = Panel->GetCachedGeometry().LocalToAbsolute((FVector2f(-70, -160) - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            MoveMouse(*Panel);
            return Next(8);
        }
        if (Phase == 8)
        {
            Test.TestFalse(TEXT("Empty canvas beside the wires has no phantom spline hit"), Panel->GetPreviousFrameSplineOverlap().IsValid());
            const auto Tooltip = Panel->GetToolTip();
            Test.TestTrue(TEXT("Leaving the visible wire clears its native tooltip"), !Tooltip || Tooltip->IsEmpty());
            Test.TestTrue(TEXT("Rebuilding and leaving a wire changes no graph values"), Moved == SerializeNodes(*Fixture->Graph));
            InputProgress->Gate = 1; return Next(9);
        }
        if (Phase == 9 || Phase == 12)
        {
            if (!Slate.GetModifierKeys().IsAltDown()) { return false; }
            if (!BeginSlice(*Panel, *Cache)) { return Finish(); }
            return Next(Phase == 9 ? 10 : 13);
        }
        if (Phase == 10)
        {
            Test.TestEqual(TEXT("Slice preview retains the requested cursor endpoint"), FVector2f(Slate.GetCursorPos()), Mouse);
            Test.TestTrue(TEXT("Slice preview is read-only"), Moved == SerializeNodes(*Fixture->Graph));
            InputProgress->Gate = 2; return Next(11);
        }
        if (Phase == 11)
        {
            if (Slate.GetModifierKeys().IsAltDown()) { return false; }
            PointerButton(*Panel, false, FModifierKeysState());
            Test.TestFalse(TEXT("Canceling the slice releases mouse capture"), Panel->HasMouseCapture());
            Test.TestTrue(TEXT("Releasing Alt cancels the slice without changing a link"), Moved == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Canceled slice creates no transaction"), AppliedTransactions(), SliceQueue);
            InputProgress->Gate = 3; return Next(12);
        }
        if (Phase == 13)
        {
            Test.TestEqual(TEXT("Slice commit retains the requested cursor endpoint"), FVector2f(Slate.GetCursorPos()), Mouse);
            Test.TestTrue(TEXT("Commit preview still preserves graph data"), Moved == SerializeNodes(*Fixture->Graph));
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture native Alt-slice preview"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                const FString Name = Style == EGlooPrintWireStyle::Rounded90 ? TEXT("GlooPrint-WireSlice-Rounded.png") : TEXT("GlooPrint-WireSlice-Diagonal.png");
                Test.TestTrue(TEXT("Save native Alt-slice preview"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
            }
            PointerButton(*Panel, false, Slate.GetModifierKeys());
            Test.TestFalse(TEXT("Committing the slice releases mouse capture"), Panel->HasMouseCapture());
            if (!Test.TestTrue(TEXT("Alt-drag slices exactly the drawn lane, preserving both neighboring connections"),
                !OtherPin()->LinkedTo.Contains(InputPin()) && OtherLinkIntact() &&
                OutputPin()->LinkedTo.Contains(Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))) { return Finish(); }
            if (!Test.TestEqual(TEXT("Native slice is one transaction"), AppliedTransactions(), SliceQueue + 1)) { return Finish(); }
            const auto Sliced = SerializeNodes(*Fixture->Graph); CheckUndoRedo(Moved, Sliced, TEXT("Alt slice"));
            InputProgress->Gate = 4; return Next(14);
        }
        if (Slate.GetModifierKeys().IsAltDown()) { return false; }
        Test.TestEqual(TEXT("Rebuilt routes retain exactly the two unsliced connections"), Cache->GetRoutes().Wires.Num(), 2);
        Test.TestFalse(TEXT("Rebuilt routes remove the sliced original pin pair"), Cache->GetRoutes().Wires.Contains(SliceKey));
        Test.TestEqual(TEXT("Unsliced wires retain custom routes"), Cache->GetRoutes().FallbackCount, 0);
        Test.TestTrue(TEXT("Completed gestures leave no mouse button pressed"), Slate.GetPressedMouseButtons().IsEmpty());
        DragKey = {Target->NodeGuid, CastChecked<UK2Node_ExecutionSequence>(Target)->GetThenPinGivenIndex(0)->PinId,
            Fixture->Print->NodeGuid, Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute)->PinId};
        return BeginNodeDrag(*Panel, *Cache) ? Next(18) : Finish();
    }
private:
    UEdGraphPin* OutputPin() const { return Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then); }
    UEdGraphPin* OtherPin() const { return Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else); }
    UEdGraphPin* InputPin() const { return Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute); }
    bool OtherLinkIntact() const { return CastChecked<UK2Node_ExecutionSequence>(Target)->GetThenPinGivenIndex(0)->LinkedTo.Contains(Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute)); }
    bool BeginNodeDrag(SGraphPanel& Panel, const FRouteCache& Cache)
    {
        const auto* Route = Cache.GetRoutes().Wires.Find(DragKey);
        if (!Test.TestTrue(TEXT("Node drag starts with the original connected pin pair routed"), Route && Route->Curves.Num() > 2)) { return false; }
        DragNode = DragCase == 0 ? static_cast<UEdGraphNode*>(Extra) : Target;
        const auto Widget = Panel.GetNodeWidgetFromGuid(DragNode->NodeGuid);
        if (!Test.TestTrue(TEXT("Native drag has a painted node widget"), Widget.IsValid())) { return false; }
        if (DragCase == 0)
        {
            float Longest = 0;
            for (int32 I = 1; I + 1 < Route->Curves.Num(); ++I)
            {
                const auto& Curve = Route->Curves[I];
                if (FMath::Abs(Curve.Start.X - Curve.End.X) < 0.1f && Curve.Length > Longest)
                {
                    Longest = Curve.Length; ObstacleProbe = EvaluateRouteCurve(Curve, Curve.Length * 0.5f);
                }
            }
            if (!Test.TestTrue(TEXT("Unrelated node can be dragged across a long visible wire lane"), Longest > 160)) { return false; }
            DragDestination = ObstacleProbe - FVector2f(Widget->GetCachedGeometry().GetLocalSize()) * 0.5f;
        }
        else { DragDestination = DragNode->GetPosition() + FVector2f(160, 128); }
        DragBefore = SerializeNodes(*Fixture->Graph); DragProperties = DescribeNodes(*Fixture->Graph);
        DragOriginal = DragNode->GetPosition(); OldDragPath = Route->Points;
        DragBuilds = Cache.GetBuildCount(); DragQueue = AppliedTransactions();
        Editor->ClearSelectionSet();
        DragGrab = FVector2f(Widget->GetCachedGeometry().GetLocalSize().X * 0.5f, 12);
        Mouse = Widget->GetCachedGeometry().LocalToAbsolute(DragGrab); MoveMouse(Panel);
        const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
        if (!Test.TestTrue(TEXT("Native mouse-down targets the actual node title"), Path.ContainsWidget(Widget.Get()))) { return false; }
        PointerButton(Panel, true, FModifierKeysState());
        MoveDraggedNode(Panel, DragDestination - FVector2f(16, 16));
        return Test.TestTrue(TEXT("Full mouse input moves the requested node"), DragNode->GetPosition() != DragOriginal);
    }
    void MoveDraggedNode(SGraphPanel& Panel, FVector2f Destination)
    {
        Mouse = Panel.GetCachedGeometry().LocalToAbsolute((Destination + DragGrab - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount());
        MoveMouse(Panel);
    }
    bool CheckNodeDrag(SGraphPanel& Panel, const TSharedPtr<FRouteCache>& Cache)
    {
        if (!Test.TestTrue(TEXT("Node drag retains its panel route cache"), Cache.IsValid())) { return Finish(); }
        auto& Slate = FSlateApplication::Get();
        if (Phase == 18 || Phase == 19)
        {
            Test.TestTrue(TEXT("Node drag holds native panel capture and left button"), Panel.HasMouseCapture() && Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));
            Test.TestFalse(TEXT("Native movement invalidates routes while the pointer remains held"), Cache->IsReady());
            Test.TestEqual(TEXT("Held node drag performs no measurement or route rebuild"), Cache->GetBuildCount(), DragBuilds);
            Test.TestEqual(TEXT("Native node movement defers its transaction until release"), AppliedTransactions(), DragQueue);
            CheckDragDrawing(Panel, *Cache, true);
            if (Phase == 18) { MoveDraggedNode(Panel, DragDestination); return Next(19); }
            if (DragCase == 0)
            {
                const auto Widget = Panel.GetNodeWidgetFromGuid(Extra->NodeGuid);
                const FBox2f Bounds(Extra->GetPosition(), Extra->GetPosition() + FVector2f(Widget->GetCachedGeometry().GetLocalSize()));
                Test.TestTrue(TEXT("Unrelated dragged body now occupies the old wire path"), Bounds.IsInside(ObstacleProbe));
            }
            const auto Properties = DescribeNodes(*Fixture->Graph);
            const FString Prefix = DragNode->GetName() + TEXT(".");
            for (const auto& Property : DragProperties)
            {
                if (Property.Key == Prefix + TEXT("NodePosX") || Property.Key == Prefix + TEXT("NodePosY")) { continue; }
                const auto* Current = Properties.Find(Property.Key);
                Test.TestTrue(TEXT("Dragging changes only the requested node position: ") + Property.Key, Current && *Current == Property.Value);
            }
            Dragged = SerializeNodes(*Fixture->Graph);
            if (DragCase == 0) { CaptureDrag(TEXT("held")); }
            PointerButton(Panel, false, FModifierKeysState());
            Test.TestTrue(TEXT("Release ends node capture and button state"), !Panel.HasMouseCapture() && Slate.GetPressedMouseButtons().IsEmpty());
            Test.TestEqual(TEXT("A native node drag creates exactly one transaction"), AppliedTransactions(), DragQueue + 1);
            return Next(20);
        }
        if (!Cache->IsReady()) { return false; }
        const auto* Route = Cache->GetRoutes().Wires.Find(DragKey);
        if (!Test.TestTrue(TEXT("Released drag retains its original connected pin pair"), Route != nullptr)) { return Finish(); }
        if (Phase == 20)
        {
            Test.TestEqual(TEXT("Release coalesces held movement into one route rebuild"), Cache->GetBuildCount(), DragBuilds + 1);
            Test.TestEqual(TEXT("Both original connections have rebuilt custom routes"), Cache->GetRoutes().Wires.Num(), 2);
            Test.TestEqual(TEXT("Node movement needs no native routing fallback"), Cache->GetRoutes().FallbackCount, 0);
            Test.TestTrue(TEXT("Route rebuilding never automatically repositions any node"), Dragged == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Moving an obstacle or endpoint changes the affected route"), Route->Points != OldDragPath);
            ReleasedDragPath = Route->Points; CheckDragDrawing(Panel, *Cache, false);
            if (DragCase == 0) { CaptureDrag(TEXT("released")); }
            Test.TestTrue(TEXT("Undo actual node drag"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Node drag undo restores all original graph data"), DragBefore == SerializeNodes(*Fixture->Graph));
            return Next(21);
        }
        if (Phase == 21)
        {
            Test.TestTrue(TEXT("Node drag undo rebuilds the exact original route"), Route->Points == OldDragPath);
            Test.TestTrue(TEXT("Redo actual node drag"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Node drag redo restores all moved graph data"), Dragged == SerializeNodes(*Fixture->Graph));
            return Next(22);
        }
        Test.TestTrue(TEXT("Node drag redo rebuilds the exact released route"), Route->Points == ReleasedDragPath);
        Test.TestTrue(TEXT("Rebuilding after redo preserves every graph value"), Dragged == SerializeNodes(*Fixture->Graph));
        if (++DragCase == 1) { return BeginNodeDrag(Panel, *Cache) ? Next(18) : Finish(); }
        InputProgress->Gate = 5; return Next(17);
    }
    void CheckDragDrawing(SGraphPanel& Panel, const FRouteCache& Cache, bool bTemporary)
    {
        FArrangedChildren Nodes(EVisibility::Visible);
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const auto Widget = Panel.GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Dragging retains native node geometry"), Widget.IsValid())) { return; }
            Nodes.AddWidget(FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        const auto Factory = MakeShared<FWireDrawing>();
        const auto Obstacle = Panel.GetNodeWidgetFromGuid(Extra->NodeGuid);
        const auto& Geometry = Obstacle->GetCachedGeometry();
        const FBox2f Body(Geometry.GetAbsolutePosition(), Geometry.GetAbsolutePosition() + Geometry.GetAbsoluteSize());
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        for (UEdGraphPin* Output : Node->Pins)
        {
            if (Output->Direction != EGPD_Output) { continue; }
            for (UEdGraphPin* Input : Output->LinkedTo)
            {
                TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
                for (UEdGraphPin* Pin : {Output, Input})
                {
                    const auto Widget = PinWidget(Panel, Pin);
                    if (!Test.TestTrue(TEXT("Dragged connection has both original pin widgets"), Widget.IsValid())) { return; }
                    Pins.Add(Widget.ToSharedRef(), FArrangedWidget(Widget.ToSharedRef(), Widget->GetCachedGeometry()));
                }
                FSlateWindowElementList NativeElements(Window), CustomElements(Window);
                FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Fixture->Graph);
                TUniquePtr<FConnectionDrawingPolicy> Custom(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, Scale, Clip, CustomElements, Fixture->Graph));
                if (!Test.TestTrue(TEXT("Drag drawing uses the actual custom policy"), Custom.IsValid())) { return; }
                Native.SetAbsoluteMousePosition(FVector2f(-100000, -100000)); Custom->SetAbsoluteMousePosition(FVector2f(-100000, -100000));
                Native.Draw(Pins, Nodes); Custom->Draw(Pins, Nodes);
                const auto& Original = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                const auto& Pieces = CustomElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
                if (!Test.TestEqual(TEXT("Native drag drawing retains the requested connection"), Original.Num(), 1) ||
                    !Test.TestTrue(TEXT("Dragging keeps the original wire visible"), !Pieces.IsEmpty())) { continue; }
                Test.TestTrue(TEXT("Dragged wire remains attached to both live native pins"), Pieces[0].P0.Equals(Original[0].P0, 0.1f) && Pieces.Last().P3.Equals(Original[0].P3, 0.1f));
                if (bTemporary)
                {
                    Test.TestEqual(TEXT("Held drag uses one inexpensive native curve per connection"), Pieces.Num(), 1);
                    Test.TestTrue(TEXT("Temporary wire has the native control points"), Pieces[0].P1.Equals(Original[0].P1, 0.1f) && Pieces[0].P2.Equals(Original[0].P2, 0.1f));
                }
                else
                {
                    const auto* Route = Cache.GetRoutes().Wires.Find({Node->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId});
                    if (!Test.TestTrue(TEXT("Released wire has a custom cached route"), Route != nullptr)) { continue; }
                    Test.TestEqual(TEXT("Released wire draws every cached custom piece"), Pieces.Num(), Route->Curves.Num());
                    for (const auto& Piece : Pieces)
                    for (int32 Sample = 0; Sample <= 64; ++Sample)
                    {
                        const float T = float(Sample) / 64.f, U = 1 - T;
                        const FVector2f Point = Piece.P0 * (U * U * U) + Piece.P1 * (3 * U * U * T) + Piece.P2 * (3 * U * T * T) + Piece.P3 * (T * T * T);
                        Test.TestFalse(TEXT("Rebuilt visible curves avoid the unrelated moved body"), Body.IsInside(Point));
                    }
                }
            }
        }
    }
    void CaptureDrag(const TCHAR* State)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture actual node-drag wire drawing"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            const FString Name = FString::Printf(TEXT("GlooPrint-NodeDrag-%s-%s.png"), Style == EGlooPrintWireStyle::Rounded90 ? TEXT("rounded") : TEXT("diagonal"), State);
            Test.TestTrue(TEXT("Save native node-drag capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    struct FInputProgress
    {
        int32 Gate = 0;
        bool bStarted = false, bCanceled = false;
    };
    void StartInputSequence()
    {
        InputProgress = MakeShared<FInputProgress>();
        const auto WaitFor = [State = InputProgress](int32 Gate)
        {
            return FDriverWaitDelegate::CreateLambda([State, Gate](const FTimespan& Elapsed)
            {
                State->bStarted = true;
                if (State->bCanceled || Elapsed > FTimespan::FromSeconds(45)) { return FDriverWaitResponse::Failed(); }
                return State->Gate >= Gate ? FDriverWaitResponse::Passed() : FDriverWaitResponse::Wait(FTimespan::FromMilliseconds(1));
            });
        };
        KeySequence = Driver->CreateSequence();
        KeySequence->Actions().Wait(WaitFor(1)).Press(EKeys::LeftAlt).Wait(WaitFor(2))
            .Release(EKeys::LeftAlt).Wait(WaitFor(3)).Press(EKeys::LeftAlt).Wait(WaitFor(4))
            .Release(EKeys::LeftAlt).Wait(WaitFor(5));
        KeyAction = KeySequence->Perform();
    }
    bool BeginSlice(SGraphPanel& Panel, const FRouteCache& Cache)
    {
        SliceKey = {Fixture->Branch->NodeGuid, OtherPin()->PinId, Target->NodeGuid, InputPin()->PinId};
        const auto* Route = Cache.GetRoutes().Wires.Find(SliceKey);
        if (!Test.TestTrue(TEXT("Slice target has a custom route"), Route && !Route->Curves.IsEmpty())) { return false; }
        float Longest = 0; FVector2f Point;
        for (int32 I = 1; I + 1 < Route->Curves.Num(); ++I)
        {
            const auto& Curve = Route->Curves[I];
            const float Height = FMath::Abs(Curve.End.Y - Curve.Start.Y);
            if (FMath::Abs(Curve.End.X - Curve.Start.X) < 0.1f && Height > Longest)
            {
                Longest = Height; Point = EvaluateRouteCurve(Curve, Curve.Length * 0.5f);
            }
        }
        if (!Test.TestTrue(TEXT("Native slice crosses a long drawn interior lane"), Longest > 80)) { return false; }
        const auto ToScreen = [&](FVector2f P) { return Panel.GetCachedGeometry().LocalToAbsolute((P - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount()); };
        auto& Slate = FSlateApplication::Get();
        Slate.SetCursorPos(ToScreen(Point - FVector2f(20, 0))); Mouse = Slate.GetCursorPos();
        MoveMouse(Panel);
        SliceQueue = AppliedTransactions();
        PointerButton(Panel, true, FSlateApplication::Get().GetModifierKeys());
        if (!Test.TestTrue(TEXT("Native slice captures the pointer"), Panel.HasMouseCapture())) { return false; }
        const FVector2f Start = Mouse;
        Slate.SetCursorPos(ToScreen(Point + FVector2f(20, 0))); Mouse = Slate.GetCursorPos();
        const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
        if (!Test.TestTrue(TEXT("Slice end uses the painted graph hit path"), Path.ContainsWidget(&Panel))) { return false; }
        const FPointerEvent Move(FSlateApplication::CursorPointerIndex, Mouse, Start, {EKeys::LeftMouseButton}, EKeys::Invalid, 0, FSlateApplication::Get().GetModifierKeys());
        FSlateApplication::Get().ProcessMouseMoveEvent(Move, false);
        return true;
    }
    TSharedPtr<SGraphPin> PinWidget(SGraphPanel& Panel, UEdGraphPin* Pin) const
    {
        const auto Node = Panel.GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid);
        return Node ? Node->FindWidgetForPin(Pin) : nullptr;
    }
    bool HoverEndpoint(SGraphPanel& Panel, const FRouteCache& Cache, bool bOutput)
    {
        const auto* Route = Cache.GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Original wire has a routed path for the next gesture"), Route && Route->Curves.Num() > 2)) { return false; }
        const FVector2f Start = Route->Curves[0].Start, End = Route->Curves.Last().End;
        float BestDistance = MAX_flt; FVector2f Point;
        for (int32 I = 1; I + 1 < Route->Curves.Num(); ++I)
        {
            const auto& Curve = Route->Curves[I];
            const FVector2f Candidate = EvaluateRouteCurve(Curve, Curve.Length * 0.5f);
            const float Near = (Candidate - (bOutput ? Start : End)).SizeSquared();
            const float Far = (Candidate - (bOutput ? End : Start)).SizeSquared();
            if (Near > FMath::Square(32.f) && Near < Far && Near < BestDistance) { BestDistance = Near; Point = Candidate; }
        }
        if (!Test.TestTrue(TEXT("A visible interior curve is closer to the requested original endpoint"), BestDistance < MAX_flt)) { return false; }
        Mouse = Panel.GetCachedGeometry().LocalToAbsolute((Point - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount());
        MoveMouse(Panel); return true;
    }
    bool CheckHover(SGraphPanel& Panel, UEdGraphPin* Expected)
    {
        const auto& Overlap = Panel.GetPreviousFrameSplineOverlap();
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        const bool bPair = Overlap.GetPins(Panel, A, B) && ((A == OutputPin() && B == InputPin()) || (B == OutputPin() && A == InputPin()));
        if (!Test.TestTrue(TEXT("Hovered drawn curve resolves its original pin pair"), bPair)) { return false; }
        const auto Best = Overlap.GetBestPinWidget(Panel);
        if (!Test.TestTrue(TEXT("Nearest endpoint uses the whole wire, not the nearest piece endpoint"), Best && Best->GetPinObj() == Expected)) { return false; }
        const auto Tooltip = Panel.GetToolTip();
        return Test.TestTrue(TEXT("Visible wire supplies the exact nonempty native tooltip of its nearest pin"), Tooltip && Tooltip == Best->GetToolTip() && !Tooltip->IsEmpty());
    }
    void ClickEmptyCanvas(SGraphPanel& Panel)
    {
        Mouse = Panel.GetCachedGeometry().LocalToAbsolute((FVector2f(-70, -160) - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount());
        MoveMouse(Panel);
        PointerButton(Panel, true, FModifierKeysState()); PointerButton(Panel, false, FModifierKeysState());
        Test.TestFalse(TEXT("Native canvas click releases retained mouse capture"), Panel.HasMouseCapture());
    }
    void MoveMouse(SGraphPanel& Panel)
    {
        auto& Slate = FSlateApplication::Get();
        const FVector2f Previous = Slate.GetCursorPos();
        Slate.SetCursorPos(FVector2D(Mouse)); Mouse = Slate.GetCursorPos();
        Test.TestEqual(TEXT("Slate and platform cursor coordinates agree"), Mouse, FVector2f(Slate.GetPlatformApplication()->Cursor->GetPosition()));
        const FPointerEvent Event(FSlateApplication::CursorPointerIndex, Mouse, Previous, Slate.GetPressedMouseButtons(), EKeys::Invalid, 0, Slate.GetModifierKeys());
        Slate.ProcessMouseMoveEvent(Event, false);
        const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
        if (!Test.TestTrue(TEXT("Native mouse processing reaches the painted graph"), Path.ContainsWidget(&Panel) && Panel.IsHovered()))
        {
            Test.AddInfo(FString::Printf(TEXT("Mouse (%.3f,%.3f), graph in path=%d, hovered=%d, path widgets=%d"),
                Mouse.X, Mouse.Y, Path.ContainsWidget(&Panel), Panel.IsHovered(), Path.Widgets.Num()));
            for (int32 I = 0; I < Path.Widgets.Num(); ++I) { Test.AddInfo(TEXT("Hit widget: ") + Path.Widgets[I].Widget->GetTypeAsString()); }
            const auto Located = Slate.LocateWindowUnderMouse(Mouse, Slate.GetInteractiveTopLevelWindows(), false, Event.GetUserIndex());
            Test.AddInfo(FString::Printf(TEXT("Native located graph=%d, user=%d, native window matches=%d"),
                Located.ContainsWidget(&Panel), Event.GetUserIndex(), Slate.GetPlatformApplication()->GetWindowUnderCursor() == Window->GetNativeWindow()));
            Test.AddInfo(FString::Printf(TEXT("Window visible=%d, accepts input=%d, native point within=%d, cursor radius=%.3f"),
                Window->IsVisible(), Window->AcceptsInput(), Window->IsScreenspaceMouseWithin(Mouse), Slate.GetCursorRadius()));
            TArray<TSharedRef<SWindow>> Visible; Slate.GetAllVisibleWindowsOrdered(Visible);
            for (const auto& Candidate : Visible)
            {
                Test.AddInfo(FString::Printf(TEXT("Visible window %s, contains mouse=%d"), *Candidate->GetTitle().ToString(), Candidate->GetRectInScreen().ContainsPoint(Mouse)));
            }
        }
    }
    void ShiftClick(SGraphPanel& Panel)
    {
        const FModifierKeysState Shift(true, false, false, false, false, false, false, false, false);
        PointerButton(Panel, true, Shift); PointerButton(Panel, false, Shift);
    }
    void PointerButton(SGraphPanel& Panel, bool bDown, const FModifierKeysState& Modifiers)
    {
        const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
        if (!Test.TestTrue(TEXT("Button input uses the actual painted graph path"), Path.ContainsWidget(&Panel))) { return; }
        const FPointerEvent Event(FSlateApplication::CursorPointerIndex, Mouse, Mouse, bDown ? TSet<FKey>{EKeys::LeftMouseButton} : TSet<FKey>{}, EKeys::LeftMouseButton, 0, Modifiers);
        auto& Slate = FSlateApplication::Get();
        const bool bHandled = bDown ? Slate.ProcessMouseButtonDownEvent(Window->GetNativeWindow(), Event) : Slate.ProcessMouseButtonUpEvent(Event);
        Test.TestTrue(TEXT("Native Slate handles the wire gesture"), bHandled);
    }
    int32 AppliedTransactions() const { return GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); }
    void CheckUndoRedo(const TArray<uint8>& Original, const TArray<uint8>& Changed, const TCHAR* Label)
    {
        Test.TestTrue(FString(Label) + TEXT(" undo succeeds"), GEditor->UndoTransaction());
        Test.TestTrue(FString(Label) + TEXT(" undo restores exact serialized data"), Original == SerializeNodes(*Fixture->Graph));
        Test.TestTrue(FString(Label) + TEXT(" redo succeeds"), GEditor->RedoTransaction());
        Test.TestTrue(FString(Label) + TEXT(" redo restores exact serialized data"), Changed == SerializeNodes(*Fixture->Graph));
    }
    bool Next(int32 Value) { Phase = Value; Frames = 0; return false; }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (InputProgress) { InputProgress->bCanceled = true; }
        auto& Slate = FSlateApplication::Get(); if (Slate.GetDragDroppingContent()) { Slate.CancelDragDrop(); }
        if (bOwnDriver && Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton))
        {
            const FPointerEvent Up(FSlateApplication::CursorPointerIndex, Slate.GetCursorPos(), Slate.GetCursorPos(), TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
            Slate.ProcessMouseButtonUpEvent(Up);
        }
        Slate.ReleaseAllPointerCapture(); Slate.CloseToolTip();
        KeySequence.Reset(); Driver.Reset();
        if (bOwnDriver) { IAutomationDriverModule::Get().Disable(); bOwnDriver = false; }
        Slate.SetCursorPos(OriginalCursor);
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
    }
    bool Finish() { Restore(); if (Window) { Window->RequestDestroyWindow(); } Editor.Reset(); Window.Reset(); return true; }
    FAutomationTestBase& Test;
    bool bRequestedActivation = false;
    double ActivationDeadline = 0;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TUniquePtr<FFixture> Fixture;
    UEdGraphNode* Target = nullptr;
    UK2Node_IfThenElse* Extra = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FRouteKey Key, SliceKey;
    FRouteKey DragKey;
    UEdGraphNode* DragNode = nullptr;
    FVector2f DragOriginal, DragDestination, DragGrab, ObstacleProbe;
    TArray<FVector2f> OldDragPath, ReleasedDragPath;
    TArray<uint8> DragBefore, Dragged;
    TMap<FString, FString> DragProperties;
    int32 DragCase = 0, DragBuilds = 0, DragQueue = 0;
    TSharedPtr<IAsyncAutomationDriver, ESPMode::ThreadSafe> Driver;
    TSharedPtr<IAsyncDriverSequence, ESPMode::ThreadSafe> KeySequence;
    TAsyncResult<bool> KeyAction;
    TSharedPtr<FInputProgress> InputProgress;
    TArray<uint8> Before, Moved;
    TMap<FGuid, FIntPoint> Positions;
    FVector2f Mouse;
    FVector2D OriginalCursor;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, SliceQueue = 0;
    bool bRestore = false, bOwnDriver = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoundedWireInteractionTest, "GlooPrint.Editor.RoundedWireGestures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoundedWireInteractionTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireInteractionCheck(*this, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagonalWireInteractionTest, "GlooPrint.Editor.DiagonalWireGestures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDiagonalWireInteractionTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireInteractionCheck(*this, EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
