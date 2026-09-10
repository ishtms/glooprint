// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IAutomationDriver.h"
#include "IAutomationDriverModule.h"
#include "IDriverSequence.h"
#include "ImageUtils.h"
#include "Input/HittestGrid.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FRouteContinuationCheck final : public IAutomationLatentCommand
{
public:
    explicit FRouteContinuationCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FRouteContinuationCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
            bRestore = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            auto* Source = Fixture->Add<UK2Node_ExecutionSequence>({0, 0});
            Target = Fixture->Add<UK2Node_ExecutionSequence>({6000, 5000});
            for (int32 I = 2; I < LinkCount; ++I) { Source->AddInputPin(); }
            for (int32 I = 0; I < LinkCount; ++I)
            {
                if (!Test.TestTrue(TEXT("Create real native execution fan-in"),
                    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Source->GetThenPinGivenIndex(I),
                        Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))) { return Finish(); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint cancellable native wire rebuild")))
                .ClientSize(FVector2f(1400, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, 0), 0.15f);
            Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native routing continuation did not finish in 45 seconds.")); return Finish(); }
        if (Phase == 5)
        {
            if (++Frames < 10) { return false; }
            Test.TestFalse(TEXT("Closing a graph releases its pending routing job and cache"), WeakCache.IsValid());
            Test.TestTrue(TEXT("Closing during routing leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache) { return false; }
        if (Phase == 0)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            if (MeasurementEvictionBuilds == INDEX_NONE)
            {
                const auto Measurements = Panel->GetMetaData<FMeasurementCache>();
                if (!Test.TestTrue(TEXT("Pending routing owns shared measurements"), Measurements.IsValid())) { return Finish(); }
                MeasurementEvictionBuilds = Cache->GetBuildCount(); Measurements->Invalidate();
                return false;
            }
            if (Cache->GetBuildCount() == MeasurementEvictionBuilds) { return false; }
            Test.TestTrue(TEXT("Evicting shared measurements restarts native capture"), Cache->GetBuildCount() > MeasurementEvictionBuilds);
            Test.TestFalse(TEXT("Incomplete native rebuild is not ready"), Cache->IsReady());
            Test.TestTrue(TEXT("Incomplete native rebuild exposes no partial routes"), Cache->GetRoutes().Wires.IsEmpty());
            Before = SerializeNodes(*Fixture->Graph);
            Queue = GEditor->Trans->GetQueueLength();
            {
                FScopedTransaction Transaction(FText::FromString(TEXT("Move target during native routing")));
                Target->Modify(); Target->NodePosY += 160;
                Fixture->Graph->NotifyGraphChanged();
            }
            Changed = SerializeNodes(*Fixture->Graph);
            Test.TestFalse(TEXT("Graph edit immediately discards the old routing job"), Cache->HasPendingRouting());
            Test.TestFalse(TEXT("Stale graph keeps native fallback until a new job finishes"), Cache->IsReady());
            Builds = Cache->GetBuildCount(); Phase = 1; return false;
        }
        if (Phase == 1)
        {
            if (!Cache->IsReady()) { return false; }
            Test.TestTrue(TEXT("Edit starts a fresh capture instead of resuming the old snapshot"), Cache->GetBuildCount() > Builds);
            Test.TestEqual(TEXT("Finished cache retains every native connection"), Cache->GetRoutes().Wires.Num(), LinkCount);
            Test.TestTrue(TEXT("Routing leaves all serialized values unchanged"), SerializeNodes(*Fixture->Graph) == Changed);
            Test.TestEqual(TEXT("Background routing adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue + 1);
            FLayoutGraph Graph; FRouteSet Expected; FString Reason;
            FMeasurementOptions Options; Options.PinVisibility = Panel->GetPinVisibility();
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Read current native graph after resumed routing"),
                CaptureGraphForRouting(Fixture->Graph, Scale, Graph, Reason, Options) && ComputeRoutes(Graph, Expected, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            for (const auto& Pair : Expected.Wires)
            {
                const auto* Actual = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Only the current graph's exact route is published"), Actual && Actual->Points == Pair.Value.Points && Actual->Fallback == Pair.Value.Fallback);
            }
            Test.TestTrue(TEXT("Undo of the user's edit succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores exact state despite intervening background routing"), SerializeNodes(*Fixture->Graph) == Before);
            Phase = 2; return false;
        }
        if (Phase == 2)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Native;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Test.TestFalse(TEXT("Native style cancels pending custom computation"), Cache->HasPendingRouting());
            Builds = Cache->GetBuildCount(); Frames = 0; Phase = 3; return false;
        }
        if (Phase == 3)
        {
            if (++Frames < 10) { return false; }
            Test.TestEqual(TEXT("Canceled native-style work remains idle"), Cache->GetBuildCount(), Builds);
            Test.TestTrue(TEXT("Style cancellation leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Phase = 4; return false;
        }
        if (Phase == 4)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            WeakCache = Cache;
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Frames = 0; Phase = 5; return false;
        }
        return false;
    }
private:
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (Window) { Window->RequestDestroyWindow(); }
        Window.Reset(); Editor.Reset();
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
        GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
    }
    bool Finish() { Restore(); return true; }
    static constexpr int32 LinkCount = 512;
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_ExecutionSequence* Target = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> WeakCache;
    TArray<uint8> Before, Changed;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    int32 Phase = 0, Frames = 0, Queue = 0, Builds = 0;
    int32 MeasurementEvictionBuilds = INDEX_NONE;
    double Deadline = 0;
    bool bRestore = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRouteContinuationEditorTest, "GlooPrint.Editor.RoutingContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRouteContinuationEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FRouteContinuationCheck(*this)); return true;
}

class FPlannedRouteReuseCheck final : public IAutomationLatentCommand
{
public:
    explicit FPlannedRouteReuseCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FPlannedRouteReuseCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (bClosing)
        {
            if (++ClosingFrames < 10) { return false; }
            Test.TestFalse(TEXT("Closing the graph releases its staged route input and result"), WeakCache.IsValid());
            return true;
        }
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Fixture = MakeUnique<FFixture>();
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint validated route reuse")))
                .ClientSize(FVector2f(1400, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Deadline = FPlatformTime::Seconds() + 45; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Planned route reuse did not settle in 45 seconds.")); return true; }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        FString Reason;
        FLayoutGraph Current; FRouteSet Expected;
        FMeasurementOptions Options; Options.PinVisibility = Panel->GetPinVisibility();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        if (!Test.TestTrue(TEXT("Independent cold native routes are available"),
            CaptureGraphForRouting(Fixture->Graph, Scale, Current, Reason, Options) && ComputeRoutes(Current, Expected, Reason)))
        {
            Test.AddError(Reason); return true;
        }
        if (bWaiting)
        {
            Test.TestEqual(TEXT("Only unchanged routing inputs reuse a plan"), Cache->GetReusedPlanCount(), ReusedBefore + (Case == 0 ? 1 : 0));
            Test.TestEqual(TEXT("Reused or rebuilt result preserves every connection"), Cache->GetRoutes().Wires.Num(), Expected.Wires.Num());
            for (const auto& Pair : Expected.Wires)
            {
                const auto* Actual = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Published paths agree with independent cold routing"), Actual && Actual->Points == Pair.Value.Points && Actual->Fallback == Pair.Value.Fallback);
            }
            if (Test.HasAnyErrors()) { return true; }
            if (++Case == 4)
            {
                Cache->Invalidate();
                Cache->StagePlannedRoutes(MoveTemp(Current), MoveTemp(Expected), EGlooPrintWireStyle::Rounded90);
                Test.TestTrue(TEXT("Staged reuse counts as pending work"), Cache->HasPendingRouting());
                WeakCache = Cache; bClosing = true;
                Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
                return false;
            }
        }
        ReusedBefore = Cache->GetReusedPlanCount();
        Cache->Invalidate();
        Cache->StagePlannedRoutes(MoveTemp(Current), MoveTemp(Expected), EGlooPrintWireStyle::Rounded90);
        Test.TestFalse(TEXT("Staging never publishes unvalidated routes"), Cache->IsReady());
        Test.TestTrue(TEXT("Staged routes remain private"), Cache->GetRoutes().Wires.IsEmpty());
        if (Case == 1) { Fixture->Print->NodePosY += 160; }
        else if (Case == 2)
        {
            Fixture->Print->FindPinChecked(TEXT("InString"))->DefaultValue += TEXT(" a long unnotified default that changes the native input widget width");
        }
        else if (Case == 3)
        {
            auto* Input = Fixture->Print->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            auto* Output = Input->LinkedTo[0];
            Output->BreakLinkTo(Input);
            Output->MakeLinkTo(Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
        }
        bWaiting = true; return false;
    }
private:
    void Restore()
    {
        if (Window) { Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset(); }
        if (Fixture)
        {
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); Fixture.Reset();
        }
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> WeakCache;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    double Deadline = 0;
    int32 Case = 0, ReusedBefore = 0, ClosingFrames = 0;
    bool bWaiting = false, bClosing = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlannedRouteReuseEditorTest, "GlooPrint.Editor.PlannedRouteReuse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPlannedRouteReuseEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FPlannedRouteReuseCheck(*this)); return true;
}

class FFormatContinuationCheck final : public IAutomationLatentCommand
{
public:
    explicit FFormatContinuationCheck(FAutomationTestBase& InTest, bool bInProgressCase = false)
        : Test(InTest), bProgressCase(bInProgressCase),
          bDesktopInput(bInProgressCase && FParse::Param(FCommandLine::Get(), TEXT("GlooPrintDesktopCancel"))) {}
    virtual ~FFormatContinuationCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (bProgressCase && !bRequestedActivation)
        {
            RequestEditorActivation(); bRequestedActivation = true;
            ActivationDeadline = FPlatformTime::Seconds() + 10;
            return false;
        }
        if (bProgressCase && !Fixture && !FPlatformApplicationMisc::IsThisApplicationForeground())
        {
            if (FPlatformTime::Seconds() > ActivationDeadline)
            {
                Test.AddError(TEXT("Editor could not become foreground for native Cancel verification.")); return Finish();
            }
            return false;
        }
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->WireStyle = EGlooPrintWireStyle::Native; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            if (bProgressCase)
            {
                Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintCancel_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
                OriginalCursor = Slate.GetCursorPos();
                auto& Module = IAutomationDriverModule::Get();
                if (!Test.TestFalse(TEXT("No other input driver owns the disposable progress fixture"), Module.IsEnabled())) { return Finish(); }
                if (!bDesktopInput)
                {
                    Module.Enable(); bOwnDriver = true; Slate.UsePlatformCursorForCursorUser(true);
                    Driver = Module.CreateAsyncDriver(); InputState = MakeShared<FInputState>();
                    InputSequence = Driver->CreateSequence();
                    InputSequence->Actions().Wait(FDriverWaitDelegate::CreateLambda([State = InputState](const FTimespan& Elapsed)
                    {
                        State->bStarted = true;
                        return State->bStop || Elapsed > FTimespan::FromSeconds(45) ? FDriverWaitResponse::Passed() :
                            FDriverWaitResponse::Wait(FTimespan::FromMilliseconds(1));
                    }));
                    InputAction = InputSequence->Perform();
                    Slate.SetCursorPos(FVector2D::ZeroVector);
                }
            }
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false, Package.Get());
            Source = Fixture->Add<UK2Node_ExecutionSequence>({0, 0});
            Target = Fixture->Add<UK2Node_ExecutionSequence>({6000, 5000});
            for (int32 Group = 0; Group < (bProgressCase ? 4 : 1); ++Group)
            {
                auto* Fan = Group == 0 ? Source : Fixture->Add<UK2Node_ExecutionSequence>({float(-500 * Group), float(20000 * Group)});
                for (int32 I = 2; I < 512; ++I) { Fan->AddInputPin(); }
                for (int32 I = 0; I < 512; ++I)
                {
                    if (!Test.TestTrue(TEXT("Create native fan for cancellable F"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
                        Fan->GetThenPinGivenIndex(I), Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))) { return Finish(); }
                }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint cancellable F")))
                .ClientSize(FVector2f(1400, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            if (bProgressCase) { Window->BringToFront(true); }
            Editor->SetNodeSelection(Source, true);
            Editor->SetViewLocation(FVector2f(-100, 0), 0.15f);
            Deadline = FPlatformTime::Seconds() + (bDesktopInput ? 120 : 45);
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native F continuation exceeded its input/continuation deadline.")); return Finish(); }
        if (++Frames < 12) { return false; }
        if (bProgressCase) { return CheckProgress(); }
        if (Phase == 0)
        {
            FFormatPlan Reference; FString Reason;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Capture complete reference plan with native measurements"),
                PlanFormatGraph(Fixture->Graph, Scale, {Source->NodeGuid}, Reference, Reason))) { Test.AddError(Reason); return Finish(); }
            Plan = MoveTemp(Reference);
            Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            BeginFormat();
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            CheckUnchanged(TEXT("Escape during F"));
            BeginFormat();
            Target->NodePosX += 160;
            Before = SerializeNodes(*Fixture->Graph);
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            CheckUnchanged(TEXT("Unnotified node edit during F"));
            BeginFormat();
            Editor->ClearSelectionSet();
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            CheckUnchanged(TEXT("Selection changed during F"));
            Editor->SetNodeSelection(Source, true);
            BeginFormat();
            GetMutableDefault<UGlooPrintSettings>()->bFormattingEnabled = false;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            CheckUnchanged(TEXT("Settings changed during F"));
            GetMutableDefault<UGlooPrintSettings>()->bFormattingEnabled = true;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            BeginFormat();
            Phase = 5; Frames = 12; return false;
        }
        if (Phase == 5)
        {
            if (FIntPoint(Target->NodePosX, Target->NodePosY) == PendingTarget)
            {
                Test.TestEqual(TEXT("Pending planning never opens a transaction"), GEditor->Trans->GetQueueLength(), Queue);
                Test.TestTrue(TEXT("Pending computation preserves all graph bytes"), SerializeNodes(*Fixture->Graph) == Before);
                Test.TestTrue(TEXT("Duplicate F remains handled"), PressF());
                Test.TestTrue(TEXT("Key repeat remains handled"), PressF(true));
                return false;
            }
            Test.TestEqual(TEXT("Completed F applies exactly one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const int32 I = Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; });
                Test.TestEqual(TEXT("Resumed F matches reference positions"), FIntPoint(Node->NodePosX, Node->NodePosY), Plan.Layout.Positions[I]);
            }
            Test.TestTrue(TEXT("Selected source remains the anchor"), Source->NodePosX == 0 && Source->NodePosY == 0 && Editor->GetSelectedNodes().Contains(Source));
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Resumed F undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores exact graph including the intervening position edit"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Resumed F redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores exact formatted result"), SerializeNodes(*Fixture->Graph) == After);
            Before = After; Queue = GEditor->Trans->GetQueueLength();
            Test.TestTrue(TEXT("No-op F is accepted"), PressF());
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            CheckUnchanged(TEXT("Resumed no-op F"));
            Test.TestTrue(TEXT("F before closing its panel is accepted"), PressF());
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Phase = 7; Frames = 0; return false;
        }
        CheckUnchanged(TEXT("Panel closed during F"));
        return Finish();
    }
private:
    struct FDesktopInput final : IInputProcessor
    {
        TWeakPtr<SWidget> Panel, Button;
        bool bF = false, bDown = false, bUp = false;
        virtual void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}
        virtual bool HandleKeyDownEvent(FSlateApplication& Slate, const FKeyEvent& Event) override
        {
            if (Event.GetKey() == EKeys::F && !Event.IsRepeat() && Slate.GetKeyboardFocusedWidget() == Panel.Pin()) { bF = true; }
            return false;
        }
        bool IsButtonEvent(FSlateApplication& Slate, const FPointerEvent& Event) const
        {
            const auto ButtonWidget = Button.Pin();
            return Event.GetEffectingButton() == EKeys::LeftMouseButton && ButtonWidget &&
                Slate.LocateWindowUnderMouse(Event.GetScreenSpacePosition(), Slate.GetInteractiveTopLevelWindows(), false, 0).ContainsWidget(ButtonWidget.Get());
        }
        virtual bool HandleMouseButtonDownEvent(FSlateApplication& Slate, const FPointerEvent& Event) override
        {
            bDown |= IsButtonEvent(Slate, Event); return false;
        }
        virtual bool HandleMouseButtonUpEvent(FSlateApplication& Slate, const FPointerEvent& Event) override
        {
            bUp |= bDown && IsButtonEvent(Slate, Event); return false;
        }
    };
    struct FProgressWidgets
    {
        TSharedPtr<SWindow> Window;
        TSharedPtr<SNotificationItem> Item;
        TSharedPtr<SWidget> Cancel;
        int32 Count = 0;
    };
    FProgressWidgets FindProgress()
    {
        FProgressWidgets Found;
        TArray<TSharedRef<SWindow>> Windows;
        FSlateNotificationManager::Get().GetWindows(Windows);
        for (const auto& Candidate : Windows)
        {
            TArray<TSharedRef<SWidget>> Widgets{Candidate};
            for (int32 I = 0; I < Widgets.Num() && I < 4096; ++I)
            {
                const auto Widget = Widgets[I];
                if (!Widget->GetVisibility().IsVisible()) { continue; }
                if (Widget->GetType() == TEXT("STextBlock") &&
                    StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == TEXT("Formatting Blueprint graph…"))
                {
                    ++Found.Count; Found.Window = Candidate;
                    auto Parent = Widget->GetParentWidget();
                    for (int32 Depth = 0; Parent && Depth < 32; ++Depth, Parent = Parent->GetParentWidget())
                    {
                        if (Parent->GetType() == TEXT("SNotificationItemImpl"))
                        {
                            Found.Item = StaticCastSharedPtr<SNotificationItem>(Parent); break;
                        }
                    }
                }
                const auto Children = Widget->GetChildren();
                for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
            }
        }
        if (Found.Item)
        {
            TArray<TSharedRef<SWidget>> Widgets{Found.Item.ToSharedRef()};
            for (int32 I = 0; I < Widgets.Num() && I < 1024; ++I)
            {
                const auto Widget = Widgets[I];
                if (!Widget->GetVisibility().IsVisible()) { continue; }
                if (Widget->GetType() == TEXT("STextBlock") && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == TEXT("Cancel"))
                {
                    auto Parent = Widget->GetParentWidget();
                    for (int32 Depth = 0; Parent && Depth < 16; ++Depth, Parent = Parent->GetParentWidget())
                    {
                        if (Parent->GetType() == TEXT("SButton")) { Found.Cancel = Parent; break; }
                    }
                }
                const auto Children = Widget->GetChildren();
                for (int32 C = 0; C < Children->Num(); ++C) { Widgets.Add(Children->GetChildAt(C)); }
            }
        }
        return Found;
    }
    bool CheckProgress()
    {
        if (bDesktopInput) { return CheckDesktopProgress(); }
        auto& Slate = FSlateApplication::Get();
        if (!InputState->bStarted) { return false; }
        if (Phase == 0)
        {
            Test.TestEqual(TEXT("No progress notification exists before F"), FindProgress().Count, 0);
            Package->SetDirtyFlag(false); Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            BeginFormat();
            if (InitialMs < 250) { Test.TestEqual(TEXT("Fast initial input does not flash progress"), FindProgress().Count, 0); }
            Phase = 1; return false;
        }
        if (!Test.TestEqual(TEXT("Pending or canceled progress creates no transaction"), GEditor->Trans->GetQueueLength(), Queue) ||
            !Test.TestEqual(TEXT("No partial layout moves the target"), FIntPoint(Target->NodePosX, Target->NodePosY), PendingTarget)) { return Finish(); }
        Test.TestFalse(TEXT("Pending or canceled progress leaves its private package clean"), Package->IsDirty());
        auto Progress = FindProgress();
        if (Phase == 1)
        {
            if (!Progress.Item || !Progress.Cancel)
            {
                if (FPlatformTime::Seconds() - StartedAt > 5)
                {
                    Test.AddError(FString::Printf(TEXT("No usable slow-format notification after five seconds (initial F %.3fms, matching labels %d)."), InitialMs, Progress.Count));
                    return Finish();
                }
                return false;
            }
            Test.TestEqual(TEXT("Slow F shows exactly one notification"), Progress.Count, 1);
            Test.TestTrue(TEXT("Progress waits for the slow-job threshold"), FPlatformTime::Seconds() - StartedAt >= 0.25);
            Test.TestEqual(TEXT("Notification represents pending work"), Progress.Item->GetCompletionState(), SNotificationItem::CS_Pending);
            Test.TestTrue(TEXT("Duplicate F is coalesced while progress is visible"), PressF());
            Test.TestTrue(TEXT("Repeated key is ignored while progress is visible"), PressF(true));
            SeenAt = FPlatformTime::Seconds(); Phase = 2; return false;
        }
        if (Phase == 2)
        {
            if (FPlatformTime::Seconds() - SeenAt < 0.6) { return false; }
            if (!Test.TestTrue(TEXT("One usable notification survives duplicate requests"), Progress.Count == 1 && Progress.Item && Progress.Cancel)) { return Finish(); }
            const auto Geometry = Progress.Cancel->GetCachedGeometry();
            const FSlateRect Client = Progress.Window->GetClientRectInScreen();
            if (!Progress.Window->IsVisible() || Geometry.GetLocalSize().X <= 0 || Geometry.GetLocalSize().Y <= 0 ||
                !Client.ContainsPoint(Geometry.LocalToAbsolute(FVector2f::ZeroVector)) ||
                !Client.ContainsPoint(Geometry.LocalToAbsolute(Geometry.GetLocalSize())))
            {
                if (FPlatformTime::Seconds() - SeenAt < 1) { return false; }
                Test.AddError(FString::Printf(TEXT("Progress was not painted: visible %d, window %.0fx%.0f, button %.0fx%.0f."),
                    Progress.Window->IsVisible(), Progress.Window->GetSizeInScreen().X, Progress.Window->GetSizeInScreen().Y,
                    Geometry.GetLocalSize().X, Geometry.GetLocalSize().Y));
                return Finish();
            }
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture the real slow-format notification"), Slate.TakeScreenshot(Progress.Item.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save native progress capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-FormatProgress.png"))));
            }
            const FVector2f Previous = Slate.GetCursorPos();
            Slate.SetCursorPos(Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
            const FVector2f Mouse = Slate.GetCursorPos();
            const FPointerEvent Move(FSlateApplication::CursorPointerIndex, Mouse, Previous, {}, EKeys::Invalid, 0, FModifierKeysState());
            Slate.ProcessMouseMoveEvent(Move, false);
            const auto NativePath = Slate.LocateWindowUnderMouse(Mouse, Slate.GetInteractiveTopLevelWindows(), false, 0);
            if (!Test.TestTrue(TEXT("Native window selection and mouse movement reach Cancel"), NativePath.ContainsWidget(Progress.Cancel.Get()) && Progress.Cancel->IsHovered()))
            {
                Test.AddInfo(FString::Printf(TEXT("Cancel hit diagnostics: path %d, hovered %d, mouse %.1f,%.1f, window %.1f,%.1f %.1fx%.1f."),
                    NativePath.ContainsWidget(Progress.Cancel.Get()), Progress.Cancel->IsHovered(), Mouse.X, Mouse.Y,
                    Progress.Window->GetPositionInScreen().X, Progress.Window->GetPositionInScreen().Y,
                    Progress.Window->GetSizeInScreen().X, Progress.Window->GetSizeInScreen().Y));
                return Finish();
            }
            const double ClickAt = FPlatformTime::Seconds();
            const FPointerEvent Down(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
            const FPointerEvent Up(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {}, EKeys::LeftMouseButton, 0, FModifierKeysState());
            Test.TestTrue(TEXT("Native notification handles Cancel press"), Slate.ProcessMouseButtonDownEvent(Progress.Window->GetNativeWindow(), Down));
            Test.TestTrue(TEXT("Native notification handles Cancel release"), Slate.ProcessMouseButtonUpEvent(Up));
            CancelMs = (FPlatformTime::Seconds() - ClickAt) * 1000;
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            CheckUnchanged(TEXT("Native notification Cancel"));
            CanceledAt = FPlatformTime::Seconds(); Phase = 3; return false;
        }
        if (Phase == 3)
        {
            if (FPlatformTime::Seconds() - CanceledAt < 1.0 || Progress.Count != 0) { return false; }
            CheckUnchanged(TEXT("Cancel after returning focus to the graph"));
            Test.AddInfo(FString::Printf(TEXT("Native progress/2048 links: initial F %.3fms; notification observed %.3fms after F; full Cancel pointer processing %.3fms. Initial F includes the first capture slice; not whole-command completion or p95."),
                InitialMs, (SeenAt - StartedAt) * 1000, CancelMs));
            InputState->bStop = true; Phase = 4; return false;
        }
        if (!InputAction.GetFuture().IsReady()) { return false; }
        Test.TestTrue(TEXT("Native cursor ownership sequence finishes"), InputAction.GetFuture().Get());
        return Finish();
    }
    bool CheckDesktopProgress()
    {
        auto& Slate = FSlateApplication::Get();
        if (Phase == 0)
        {
            Test.TestFalse(TEXT("Desktop fixture leaves Unreal's synthetic input driver disabled"), IAutomationDriverModule::Get().IsEnabled());
            Test.TestEqual(TEXT("Desktop fixture starts without progress"), FindProgress().Count, 0);
            Package->SetDirtyFlag(false); Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            PendingTarget = FIntPoint(Target->NodePosX, Target->NodePosY);
            DesktopInput = MakeShared<FDesktopInput>(); DesktopInput->Panel = Editor->GetGraphPanel()->AsShared();
            if (!Test.TestTrue(TEXT("Register passive desktop input observer"), Slate.RegisterInputPreProcessor(DesktopInput, 0))) { return Finish(); }
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            Window->SetTitle(FText::FromString(TEXT("GlooPrint desktop Cancel - press F")));
            Test.AddInfo(TEXT("Desktop input mode: waiting for F and an OS-dispatched Cancel click; no synthetic input driver or supplied pointer path."));
            Phase = 1; return false;
        }
        if (Phase == 1)
        {
            if (!DesktopInput->bF) { return false; }
            StartedAt = FPlatformTime::Seconds(); Phase = 2;
        }
        if (!Test.TestEqual(TEXT("Desktop cancellation creates no layout transaction"), GEditor->Trans->GetQueueLength(), Queue) ||
            !Test.TestEqual(TEXT("Desktop cancellation never applies a partial layout"), FIntPoint(Target->NodePosX, Target->NodePosY), PendingTarget) ||
            !Test.TestFalse(TEXT("Desktop cancellation leaves the private package clean"), Package->IsDirty())) { return Finish(); }
        auto Progress = FindProgress();
        if (Phase == 2)
        {
            if (Progress.Item && Progress.Cancel)
            {
                DesktopInput->Button = Progress.Cancel;
                if (!SeenAt) { SeenAt = FPlatformTime::Seconds(); }
                if (!bDesktopCaptured && FPlatformTime::Seconds() - SeenAt >= 0.6)
                {
                    TArray<FColor> Pixels; FIntVector Size;
                    if (Test.TestTrue(TEXT("Capture desktop Cancel notification"), Slate.TakeScreenshot(Progress.Item.ToSharedRef(), Pixels, Size)))
                    {
                        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                        Test.TestTrue(TEXT("Save desktop Cancel capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-DesktopCancel.png"))));
                    }
                    bDesktopCaptured = true;
                }
            }
            if (!DesktopInput->bUp)
            {
                if (SeenAt && !Test.TestTrue(TEXT("Progress remains usable until desktop release, including while Cancel is pressed"),
                    Progress.Count == 1 && Progress.Item && Progress.Item->GetCompletionState() == SNotificationItem::CS_Pending)) { return Finish(); }
                if (!SeenAt && FPlatformTime::Seconds() - StartedAt > 5) { Test.AddError(TEXT("Desktop F did not expose its slow-job notification.")); return Finish(); }
                return false;
            }
            Test.TestTrue(TEXT("Desktop Cancel receives both native press and release"), DesktopInput->bDown && DesktopInput->bUp);
            Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
            CheckUnchanged(TEXT("OS-dispatched Cancel")); CanceledAt = FPlatformTime::Seconds(); Phase = 3; return false;
        }
        if (FPlatformTime::Seconds() - CanceledAt < 1 || Progress.Count != 0) { return false; }
        CheckUnchanged(TEXT("Desktop cancellation after graph refocus"));
        Test.TestFalse(TEXT("Desktop check never enabled the synthetic input driver"), IAutomationDriverModule::Get().IsEnabled());
        Test.AddInfo(TEXT("Desktop F and Cancel press/release observed through normal native window selection; exact graph, package and undo state preserved after refocus."));
        return Finish();
    }
    bool PressF(bool bRepeat = false) { return FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, bRepeat, 0, 0)); }
    void BeginFormat()
    {
        FSlateApplication::Get().SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        PendingTarget = FIntPoint(Target->NodePosX, Target->NodePosY);
        const double Start = FPlatformTime::Seconds();
        const bool bHandled = PressF();
        if (bProgressCase) { InitialMs = (FPlatformTime::Seconds() - Start) * 1000; StartedAt = Start; }
        Test.TestTrue(TEXT("Actual F starts a slow native graph plan"), bHandled);
        CheckUnchanged(TEXT("F yields before applying its pending plan"));
    }
    void CheckUnchanged(const FString& Context)
    {
        Test.TestTrue(Context + TEXT(" leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
        Test.TestEqual(Context + TEXT(" adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
    }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (DesktopInput) { FSlateApplication::Get().UnregisterInputPreProcessor(DesktopInput); DesktopInput.Reset(); }
        if (Window) { Window->RequestDestroyWindow(); }
        Window.Reset(); Editor.Reset();
        if (InputState) { InputState->bStop = true; }
        InputSequence.Reset(); Driver.Reset();
        if (bOwnDriver)
        {
            auto& Slate = FSlateApplication::Get();
            Slate.ReleaseAllPointerCapture(); Slate.CloseToolTip();
            IAutomationDriverModule::Get().Disable(); bOwnDriver = false;
            Slate.SetCursorPos(OriginalCursor);
        }
        if (Package.IsValid()) { Package->SetDirtyFlag(false); }
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
    }
    FAutomationTestBase& Test;
    struct FInputState { bool bStarted = false, bStop = false; };
    TSharedPtr<FInputState> InputState;
    TSharedPtr<FDesktopInput> DesktopInput;
    TSharedPtr<IAsyncAutomationDriver, ESPMode::ThreadSafe> Driver;
    TSharedPtr<IAsyncDriverSequence, ESPMode::ThreadSafe> InputSequence;
    TAsyncResult<bool> InputAction;
    TStrongObjectPtr<UPackage> Package;
    FVector2D OriginalCursor;
    TUniquePtr<FFixture> Fixture;
    UK2Node_ExecutionSequence* Source = nullptr;
    UK2Node_ExecutionSequence* Target = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FFormatPlan Plan;
    FIntPoint PendingTarget;
    TArray<uint8> Before, After;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    int32 Phase = 0, Frames = 0, Queue = 0;
    double Deadline = 0, ActivationDeadline = 0;
    double StartedAt = 0, SeenAt = 0, CanceledAt = 0, InitialMs = 0, CancelMs = 0;
    bool bProgressCase = false, bOwnDriver = false, bRequestedActivation = false;
    bool bDesktopInput = false, bDesktopCaptured = false;
    bool bRestore = false, bOriginalEnabled = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatContinuationEditorTest, "GlooPrint.Editor.FormatContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFormatContinuationEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFormatContinuationCheck(*this)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatProgressEditorTest, "GlooPrint.Editor.FormatProgressCancel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFormatProgressEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFormatContinuationCheck(*this, true)); return true;
}
}
#endif
