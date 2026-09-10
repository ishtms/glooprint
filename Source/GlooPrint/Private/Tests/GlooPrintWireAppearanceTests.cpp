// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "BlueprintConnectionDrawingPolicy.h"
#include "BlueprintEditorSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericApplication.h"
#include "IAutomationDriver.h"
#include "IAutomationDriverModule.h"
#include "IDriverSequence.h"
#include "ImageUtils.h"
#include "Input/HittestGrid.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Types/PaintArgs.h"
#include "Widgets/Images/SLayeredImage.h"
#include "Widgets/IToolTip.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWireAppearanceCheck final : public IAutomationLatentCommand
{
public:
    explicit FWireAppearanceCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FWireAppearanceCheck() { Restore(); }

    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        auto& Slate = FSlateApplication::Get();
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Wire appearance checks exceeded 45 seconds.")); return Finish(); }
        if (!InputState->bStarted) { return false; }
        if (Phase == 7)
        {
            if (!InputAction.GetFuture().IsReady()) { return false; }
            Test.TestTrue(TEXT("Appearance input sequence finishes"), InputAction.GetFuture().Get());
            return Finish();
        }
        if (++Frames < 12) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 0)
        {
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    auto PinWidget = Widget ? Widget->FindWidgetForPin(Pin) : nullptr;
                    if (PinWidget && PinWidget->GetToolTip()) { PinWidget->GetToolTip()->IsEmpty(); }
                }
            }
            Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength(); Builds = Cache->GetBuildCount();
            Test.TestEqual(TEXT("All nine native connections have custom routes"), Cache->GetRoutes().Wires.Num(), Pairs.Num());
            Test.TestEqual(TEXT("Appearance fixture has no native fallback"), Cache->GetRoutes().FallbackCount, 0);
            CompareDrawing(*Cache, EState::Normal);
            CompareDrawing(*Cache, EState::Preview);
            CompareIncompatiblePins();
            Capture(TEXT("colors"));
            const auto* Route = Cache->GetRoutes().Wires.Find(Key(Pairs[0]));
            if (!Test.TestTrue(TEXT("Boolean wire has an interior route to hover"), Route && Route->Curves.Num() > 2)) { return Finish(); }
            const FVector2f Point = EvaluateRoute(*Route, Route->Length * 0.5f);
            HoverPoint = Panel->GetCachedGeometry().LocalToAbsolute((Point - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            Move(HoverPoint); HoverStarted = Slate.GetCurrentTime();
            return Next(1);
        }
        if (Phase == 1)
        {
            if (Slate.GetCurrentTime() - HoverStarted < 1.5) { return false; }
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            const auto& Pair = Pairs[0];
            if (!Test.TestTrue(TEXT("Actual mouse hover identifies the boolean wire's original pins"),
                Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
                ((A == Pair.Output() && B == Pair.Input()) || (B == Pair.Output() && A == Pair.Input())))) { return Finish(); }
            CompareDrawing(*Cache, EState::Hover); Capture(TEXT("hover"));
            const FModifierKeysState Shift(true, false, false, false, false, false, false, false, false);
            Button(true, Shift); Button(false, Shift);
            return Next(2);
        }
        if (Phase == 2)
        {
            const auto Marked = Panel->MarkedPin.Pin();
            if (!Test.TestTrue(TEXT("Native Shift input marks an endpoint of the visible boolean wire"),
                Marked && (Marked->GetPinObj() == Pairs[0].Output() || Marked->GetPinObj() == Pairs[0].Input()))) { return Finish(); }
            CompareDrawing(*Cache, EState::Marked);
            const FModifierKeysState Shift(true, false, false, false, false, false, false, false, false);
            Button(true, Shift); Button(false, Shift);
            Move(Panel->GetCachedGeometry().LocalToAbsolute(FVector2f(30, 30)));
            Button(true); Button(false);
            Editor->SetNodeSelection(Pairs[0].From, true); Editor->SetNodeSelection(Pairs[0].To, true);
            CompareDrawing(*Cache, EState::Selected);
            Editor->ClearSelectionSet();
            GetMutableDefault<UBlueprintEditorSettings>()->bShowPanelContextMenuForIncompatibleConnections = false;
            BeginPreview(); return Next(3);
        }
        if (Phase == 3 || Phase == 5)
        {
            if (!Test.TestTrue(TEXT("Native pin input starts a connection drag"), Slate.GetDragDroppingContent().IsValid())) { return Finish(); }
            const auto ObjectPin = Widget(Pairs[5].Input());
            const FLinearColor Actual = PaintedPinColor(ObjectPin);
            Test.TestTrue(TEXT("Actual preview retains native incompatible-pin feedback"),
                Actual.Equals(Phase == 3 ? NativeIncompatible : NativeNormalObject, 0.001f));
            if (Phase == 3) { Capture(TEXT("incompatible")); }
            const FKeyEvent Escape(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
            Test.TestTrue(TEXT("Escape cancels the native connection preview"), Slate.ProcessKeyDownEvent(Escape));
            Slate.ProcessKeyUpEvent(Escape); Button(false);
            return Next(Phase + 1);
        }
        if (Phase == 4)
        {
            CheckPreviewRestored();
            GetMutableDefault<UBlueprintEditorSettings>()->bShowPanelContextMenuForIncompatibleConnections = true;
            BeginPreview(); return Next(5);
        }
        CheckPreviewRestored();
        Test.TestTrue(TEXT("Appearance input preserves all serialized graph data"), Before == SerializeNodes(*Fixture->Graph));
        Test.TestEqual(TEXT("Appearance input opens no transaction"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestEqual(TEXT("Hover, marking, selection and previews reuse routes"), Cache->GetBuildCount(), Builds);
        if (Style == EGlooPrintWireStyle::Rounded90)
        {
            Style = EGlooPrintWireStyle::Diagonal45;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = Style; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Move(Panel->GetCachedGeometry().LocalToAbsolute(FVector2f(30, 30)));
            return Next(0);
        }
        InputState->bComplete = true; return Next(7);
    }

private:
    enum class EState { Normal, Hover, Marked, Selected, Preview };
    struct FPair
    {
        UEdGraphNode* From = nullptr; UEdGraphNode* To = nullptr;
        FName FromPin, ToPin;
        UEdGraphPin* Output() const { return From->FindPinChecked(FromPin); }
        UEdGraphPin* Input() const { return To->FindPinChecked(ToPin); }
    };
    struct FInputState { bool bStarted = false, bComplete = false, bCanceled = false; };

    bool Start()
    {
        auto& Slate = FSlateApplication::Get();
        OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
        bOriginalMenu = GetDefault<UBlueprintEditorSettings>()->bShowPanelContextMenuForIncompatibleConnections;
        OriginalCursor = Slate.GetCursorPos(); bRestore = true;
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = Style; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
        Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
        TArray<FEdGraphPinType> Types; Types.SetNum(7);
        Types[0].PinCategory = UEdGraphSchema_K2::PC_Boolean;
        Types[1].PinCategory = UEdGraphSchema_K2::PC_Int;
        Types[2].PinCategory = UEdGraphSchema_K2::PC_Real; Types[2].PinSubCategory = UEdGraphSchema_K2::PC_Double;
        Types[3].PinCategory = UEdGraphSchema_K2::PC_String;
        Types[4].PinCategory = UEdGraphSchema_K2::PC_Struct; Types[4].PinSubCategoryObject = TBaseStructure<FVector>::Get();
        Types[5].PinCategory = UEdGraphSchema_K2::PC_Object; Types[5].PinSubCategoryObject = AActor::StaticClass();
        Types[6] = Types[1]; Types[6].ContainerType = EPinContainerType::Array;
        const TArray<FName> Names{TEXT("BooleanValue"), TEXT("IntegerValue"), TEXT("NumberValue"), TEXT("StringValue"), TEXT("VectorValue"), TEXT("ObjectValue"), TEXT("ArrayValue")};
        for (int32 I = 0; I < Types.Num(); ++I)
        {
            if (!Test.TestTrue(TEXT("Native Blueprint accepts the representative member type"),
                FBlueprintEditorUtils::AddMemberVariable(Fixture->Blueprint.Get(), Names[I], Types[I]))) { return Finish(); }
        }
        for (int32 I = 0; I < Types.Num(); ++I)
        {
            const FVector2f Position(float((I % 2) * 700), float((I / 2) * 220));
            auto* Get = NewObject<UK2Node_VariableGet>(Fixture->Graph); Get->VariableReference.SetSelfMember(Names[I]);
            Fixture->Initialize(*Get, Position);
            auto* Set = NewObject<UK2Node_VariableSet>(Fixture->Graph); Set->VariableReference.SetSelfMember(Names[I]);
            Fixture->Initialize(*Set, Position + FVector2f(350, 65));
            Link(Get->FindPinChecked(Names[I]), Set->FindPinChecked(Names[I]));
        }
        auto* Branch = Fixture->Add<UK2Node_IfThenElse>(FVector2f(700, 660));
        auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>(FVector2f(1050, 725));
        Link(Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then), Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        auto* Delegate = Fixture->Add<UK2Node_CreateDelegate>(FVector2f(0, 850));
        Delegate->SetFunction(TEXT("K2_DestroyActor"));
        auto* Timer = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Timer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate")));
        Fixture->Initialize(*Timer, FVector2f(350, 915));
        Timer->AdvancedPinDisplay = ENodeAdvancedPins::Shown;
        Link(Delegate->GetDelegateOutPin(), Timer->FindPinChecked(TEXT("Delegate")));
        if (!bLinksValid) { return Finish(); }
        FCompilerResultsLog Compile;
        FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Compile);
        if (!Test.TestEqual(TEXT("Native color fixture compiles"), Compile.NumErrors, 0)) { return Finish(); }
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint native wire appearance")))
            .ClientSize(FVector2f(1400, 1100))[Editor.ToSharedRef()];
        Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-80, -80), 0.65f);
        auto& Module = IAutomationDriverModule::Get();
        if (!Test.TestFalse(TEXT("Appearance fixture owns an independent input driver"), Module.IsEnabled())) { return Finish(); }
        Module.Enable(); bOwnDriver = true; Slate.UsePlatformCursorForCursorUser(true);
        Driver = Module.CreateAsyncDriver(); InputState = MakeShared<FInputState>();
        InputSequence = Driver->CreateSequence();
        InputSequence->Actions().Wait(FDriverWaitDelegate::CreateLambda([State = InputState](const FTimespan& Elapsed)
        {
            State->bStarted = true;
            if (State->bCanceled || Elapsed > FTimespan::FromSeconds(45)) { return FDriverWaitResponse::Failed(); }
            return State->bComplete ? FDriverWaitResponse::Passed() : FDriverWaitResponse::Wait(FTimespan::FromMilliseconds(1));
        }));
        InputAction = InputSequence->Perform();
        Slate.SetCursorPos(FVector2D::ZeroVector); Deadline = FPlatformTime::Seconds() + 45;
        return false;
    }
    void Link(UEdGraphPin* Output, UEdGraphPin* Input)
    {
        bLinksValid &= Test.TestTrue(TEXT("Native schema connects a typed fixture wire"), Fixture->Graph->GetSchema()->TryCreateConnection(Output, Input));
        Pairs.Add({Output->GetOwningNode(), Input->GetOwningNode(), Output->PinName, Input->PinName});
    }
    FRouteKey Key(const FPair& Pair) const { return {Pair.From->NodeGuid, Pair.Output()->PinId, Pair.To->NodeGuid, Pair.Input()->PinId}; }
    TSharedPtr<SGraphPin> Widget(UEdGraphPin* Pin) const
    {
        const auto Node = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Pin->GetOwningNode()->NodeGuid);
        return Node ? Node->FindWidgetForPin(Pin) : nullptr;
    }
    void CompareDrawing(const FRouteCache& Cache, EState State)
    {
        FArrangedChildren Nodes(EVisibility::Visible);
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            const auto Native = Editor->GetGraphPanel()->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (!Test.TestTrue(TEXT("Color fixture has native node geometry"), Native.IsValid())) { return; }
            Nodes.AddWidget(FArrangedWidget(Native.ToSharedRef(), Native->GetCachedGeometry()));
        }
        const float Scale = Nodes[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        const auto Factory = MakeShared<FWireDrawing>();
        for (int32 I = 0; I < Pairs.Num(); ++I)
        {
            const FPair& Pair = Pairs[I];
            const auto Out = Widget(Pair.Output()), In = Widget(Pair.Input());
            if (!Test.TestTrue(TEXT("Both original pins have live widgets"), Out.IsValid() && In.IsValid())) { return; }
            TMap<TSharedRef<SWidget>, FArrangedWidget> Pins;
            Pins.Add(Out.ToSharedRef(), FArrangedWidget(Out.ToSharedRef(), Out->GetCachedGeometry()));
            Pins.Add(In.ToSharedRef(), FArrangedWidget(In.ToSharedRef(), In->GetCachedGeometry()));
            FSlateWindowElementList NativeElements(Window), CustomElements(Window);
            FKismetConnectionDrawingPolicy Native(0, 1, Scale, Clip, NativeElements, Fixture->Graph);
            TUniquePtr<FConnectionDrawingPolicy> Custom(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, Scale, Clip, CustomElements, Fixture->Graph));
            if (!Test.TestTrue(TEXT("Custom policy exists for appearance comparison"), Custom.IsValid())) { return; }
            for (FConnectionDrawingPolicy* Policy : {static_cast<FConnectionDrawingPolicy*>(&Native), Custom.Get()})
            {
                Policy->SetAbsoluteMousePosition(FVector2f(-100000, -100000));
                if (State == EState::Hover) { Policy->SetHoveredPins({Pairs[0].Output(), Pairs[0].Input()}, {}, HoverStarted); }
                if (State == EState::Marked) { Policy->SetMarkedPin(Editor->GetGraphPanel()->MarkedPin); }
                if (State == EState::Selected) { Policy->SetSelectedNodes(Editor->GetGraphPanel()->GetSelectedGraphNodes()); }
                if (State == EState::Preview) { Policy->SetHoveredPins({}, {Widget(Pairs[0].Output())}, 0); }
                Policy->Draw(Pins, Nodes);
            }
            const auto& Original = NativeElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
            const auto& Pieces = CustomElements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Spline>();
            if (!Test.TestEqual(TEXT("Native policy draws exactly the requested original connection"), Original.Num(), 1)) { continue; }
            const auto* Route = Cache.GetRoutes().Wires.Find(Key(Pair));
            if (!Test.TestTrue(TEXT("Typed wire has a bent custom route"), Route && Route->Curves.Num() > 1)) { continue; }
            Test.TestEqual(TEXT("Custom appearance is verified on every rendered route piece"), Pieces.Num(), Route->Curves.Num());
            for (const auto& Piece : Pieces)
            {
                Test.TestTrue(TEXT("Custom segment retains native wire color and alpha"), Piece.GetTint().Equals(Original[0].GetTint(), 0.001f));
                Test.TestTrue(TEXT("Custom segment retains native wire thickness"), FMath::IsNearlyEqual(Piece.GetThickness(), Original[0].GetThickness(), 0.001f));
            }
            if (State == EState::Normal)
            {
                Test.TestTrue(TEXT("Normal wire uses its native schema pin-type color"),
                    Original[0].GetTint().Equals(Fixture->Graph->GetSchema()->GetPinTypeColor(Pair.Output()->PinType), 0.001f));
                if (I == 0) { NormalBooleanThickness = Original[0].GetThickness(); }
                if (I == 1) { NormalOtherColor = Original[0].GetTint(); }
            }
            if (State == EState::Hover || State == EState::Marked)
            {
                if (I == 0) { Test.TestTrue(TEXT("Native emphasis thickens the identified boolean wire"), Original[0].GetThickness() > NormalBooleanThickness); }
                if (I == 1) { Test.TestFalse(TEXT("Native emphasis fades unrelated wires"), Original[0].GetTint().Equals(NormalOtherColor, 0.001f)); }
            }
        }
    }
    FLinearColor PaintedPinColor(const TSharedPtr<SGraphPin>& Pin)
    {
        if (!Test.TestTrue(TEXT("Pin color inspection has a live widget"), Pin.IsValid())) { return FLinearColor::Transparent; }
        TArray<TSharedRef<SWidget>> Widgets{Pin.ToSharedRef()};
        for (int32 I = 0; I < Widgets.Num() && I < 256; ++I)
        {
            const auto Current = Widgets[I];
            if (Current->GetType() == TEXT("SLayeredImage"))
            {
                Current->UpdateAllAttributes();
                FHittestGrid Grid;
                FSlateWindowElementList Elements(Window);
                FPaintArgs Args(Pin.Get(), Grid, FVector2f::ZeroVector, FSlateApplication::Get().GetCurrentTime(), 0);
                StaticCastSharedRef<SLayeredImage>(Current)->OnPaint(Args, Current->GetCachedGeometry(),
                    FSlateRect(-100000, -100000, 100000, 100000), Elements, 0, FWidgetStyle(), true);
                const auto& Icons = Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Box>();
                if (Icons.IsEmpty())
                {
                    Test.AddError(FString::Printf(TEXT("Native pin icon emits no box: %s.%s (%s), pin visibility %s, image visibility %s, image size %s, rounded boxes %d."),
                        *Pin->GetPinObj()->GetOwningNode()->GetName(), *Pin->GetPinObj()->PinName.ToString(), *Pin->GetTypeAsString(),
                        *Pin->GetVisibility().ToString(), *Current->GetVisibility().ToString(), *Current->GetCachedGeometry().GetLocalSize().ToString(),
                        Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_RoundedBox>().Num()));
                    return FLinearColor::Transparent;
                }
                return Icons[0].GetTint();
            }
            FChildren* Children = Current->GetChildren();
            for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
        }
        Test.AddError(TEXT("No native pin icon found within the bounded widget walk."));
        return FLinearColor::Transparent;
    }
    void CompareIncompatiblePins()
    {
        TSet<TSharedRef<SWidget>> Pins; Editor->GetGraphPanel()->GetAllPins(Pins);
        const auto StartPin = Widget(Pairs[0].Output()), ObjectPin = Widget(Pairs[5].Input());
        if (!Test.TestTrue(TEXT("Compatibility comparison has live typed pins"), StartPin.IsValid() && ObjectPin.IsValid())) { return; }
        Test.TestEqual(TEXT("Boolean-to-object connection is natively disallowed"),
            Fixture->Graph->GetSchema()->CanCreateConnection(Pairs[0].Output(), Pairs[5].Input()).Response, CONNECT_RESPONSE_DISALLOW);
        FSlateWindowElementList NativeElements(Window), CustomElements(Window);
        const FSlateRect Clip(-100000, -100000, 100000, 100000);
        FKismetConnectionDrawingPolicy Native(0, 1, 1, Clip, NativeElements, Fixture->Graph);
        const auto Factory = MakeShared<FWireDrawing>();
        TUniquePtr<FConnectionDrawingPolicy> Custom(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, 1, Clip, CustomElements, Fixture->Graph));
        Native.ResetIncompatiblePinDrawState(Pins); NativeNormalObject = PaintedPinColor(ObjectPin);
        for (bool bShowMenu : {false, true})
        {
            GetMutableDefault<UBlueprintEditorSettings>()->bShowPanelContextMenuForIncompatibleConnections = bShowMenu;
            Native.SetIncompatiblePinDrawState(StartPin, Pins);
            TMap<FGuid, FLinearColor> Colors;
            for (const auto& Pin : Pins)
            {
                const auto Typed = StaticCastSharedRef<SGraphPin>(Pin);
                Colors.Add(Typed->GetPinObj()->PinId, PaintedPinColor(Typed));
            }
            if (!bShowMenu) { NativeIncompatible = PaintedPinColor(ObjectPin); }
            Native.ResetIncompatiblePinDrawState(Pins);
            Custom->SetIncompatiblePinDrawState(StartPin, Pins);
            for (const auto& Pin : Pins)
            {
                const auto Typed = StaticCastSharedRef<SGraphPin>(Pin);
                Test.TestTrue(TEXT("Custom policy preserves every native pin's compatibility feedback"),
                    PaintedPinColor(Typed).Equals(Colors.FindChecked(Typed->GetPinObj()->PinId), 0.001f));
            }
            Custom->ResetIncompatiblePinDrawState(Pins);
        }
        Test.TestFalse(TEXT("Native incompatible feedback visibly dims the object pin"), NativeIncompatible.Equals(NativeNormalObject, 0.001f));
    }
    void BeginPreview()
    {
        const auto Pin = Widget(Pairs[0].Output());
        Move(Pin->GetCachedGeometry().GetAbsolutePosition() + Pin->GetCachedGeometry().GetAbsoluteSize() * 0.5f);
        Button(true); Move(Mouse + FVector2f(25, 10));
        const auto Target = Widget(Pairs[5].Input());
        Move(Target->GetCachedGeometry().GetAbsolutePosition() + Target->GetCachedGeometry().GetAbsoluteSize() * 0.5f);
    }
    void CheckPreviewRestored()
    {
        Test.TestFalse(TEXT("Canceled preview releases its native drag operation"), FSlateApplication::Get().GetDragDroppingContent().IsValid());
        Test.TestTrue(TEXT("Canceling preview restores the original object-pin color"),
            PaintedPinColor(Widget(Pairs[5].Input())).Equals(NativeNormalObject, 0.001f));
    }
    void Move(FVector2f Position)
    {
        auto& Slate = FSlateApplication::Get(); const FVector2f Previous = Slate.GetCursorPos();
        Slate.SetCursorPos(FVector2D(Position)); Mouse = Slate.GetCursorPos();
        Test.TestEqual(TEXT("Appearance input owns both Slate and platform cursor coordinates"), Mouse, FVector2f(Slate.GetPlatformApplication()->Cursor->GetPosition()));
        const FPointerEvent Event(FSlateApplication::CursorPointerIndex, Mouse, Previous, Slate.GetPressedMouseButtons(), EKeys::Invalid, 0, Slate.GetModifierKeys());
        Slate.ProcessMouseMoveEvent(Event, false);
        const FWidgetPath Path(Window->GetHittestGrid().GetBubblePath(Mouse, 0, false, 0));
        Test.TestTrue(TEXT("Appearance input reaches the actual painted graph"), Path.ContainsWidget(Editor->GetGraphPanel()));
    }
    void Button(bool bDown, const FModifierKeysState& Modifiers = {})
    {
        auto& Slate = FSlateApplication::Get();
        const FPointerEvent Event(FSlateApplication::CursorPointerIndex, Mouse, Mouse,
            bDown ? TSet<FKey>{EKeys::LeftMouseButton} : TSet<FKey>{}, EKeys::LeftMouseButton, 0, Modifiers);
        if (bDown) { Slate.ProcessMouseButtonDownEvent(Window->GetNativeWindow(), Event); }
        else { Slate.ProcessMouseButtonUpEvent(Event); }
    }
    void Capture(const TCHAR* Suffix)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native wire appearance"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            const FString Name = FString::Printf(TEXT("GlooPrint-Appearance-%s-%s.png"), Style == EGlooPrintWireStyle::Rounded90 ? TEXT("rounded") : TEXT("diagonal"), Suffix);
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save wire appearance capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    bool Next(int32 Value) { Phase = Value; Frames = 0; return false; }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (InputState) { InputState->bCanceled = true; }
        auto& Slate = FSlateApplication::Get();
        if (bOwnDriver)
        {
            if (Slate.GetDragDroppingContent()) { Slate.CancelDragDrop(); }
            if (Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton)) { Button(false); }
            Slate.ReleaseAllPointerCapture(); Slate.CloseToolTip();
            InputSequence.Reset(); Driver.Reset(); IAutomationDriverModule::Get().Disable(); bOwnDriver = false;
        }
        Slate.SetCursorPos(OriginalCursor);
        GetMutableDefault<UBlueprintEditorSettings>()->bShowPanelContextMenuForIncompatibleConnections = bOriginalMenu;
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
        if (Window) { Window->RequestDestroyWindow(); } Editor.Reset(); Window.Reset();
    }
    bool Finish() { Restore(); return true; }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TArray<FPair> Pairs;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TSharedPtr<IAsyncAutomationDriver, ESPMode::ThreadSafe> Driver;
    TSharedPtr<IAsyncDriverSequence, ESPMode::ThreadSafe> InputSequence;
    TAsyncResult<bool> InputAction;
    TSharedPtr<FInputState> InputState;
    TArray<uint8> Before;
    FVector2f Mouse, HoverPoint;
    FVector2D OriginalCursor;
    FLinearColor NativeNormalObject, NativeIncompatible, NormalOtherColor;
    double Deadline = 0, HoverStarted = 0;
    float NormalBooleanThickness = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, Queue = 0;
    EGlooPrintWireStyle Style = EGlooPrintWireStyle::Rounded90, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    bool bRestore = false, bOwnDriver = false, bLinksValid = true, bOriginalMenu = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWireAppearanceTest, "GlooPrint.Editor.WireAppearance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWireAppearanceTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld) { AddError(TEXT("Wire appearance checks require an idle editor.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FWireAppearanceCheck(*this)); return true;
}
}
#endif
