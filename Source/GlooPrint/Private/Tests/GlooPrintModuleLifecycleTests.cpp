// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputBindingManager.h"
#include "GraphEditorModule.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/IToolTip.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FModuleLifecycleCheck final : public IAutomationLatentCommand
{
public:
    explicit FModuleLifecycleCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FModuleLifecycleCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled;
            OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintLifecycle_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), true, Package.Get());
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint module lifecycle"))).ClientSize(FVector2f(1200, 800))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-100, -120), 0.75f);
            Editor->SetNodeSelection(Fixture->Branch, true); Slate.SetCursorPos(FVector2D::ZeroVector);
            RouteOnlyEditor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(false);
            RouteOnlyWindow = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint routing-only lifecycle")))
                .ClientSize(FVector2f(500, 400))[RouteOnlyEditor.ToSharedRef()];
            Slate.AddWindow(RouteOnlyWindow.ToSharedRef());
            Deadline = FPlatformTime::Seconds() + 40;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Module lifecycle fixture exceeded 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        if (Phase == 4)
        {
            Test.TestFalse(TEXT("Closing the reloaded panel releases its measurement cache"), ClosedMeasurement.IsValid());
            Test.TestFalse(TEXT("Closing the reloaded panel releases its route cache"), ClosedRoutes.IsValid());
            Test.TestFalse(TEXT("Closing the panel never formatted releases its measurements"), ClosedRouteOnlyMeasurement.IsValid());
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        auto& Modules = FModuleManager::Get();
        if (Phase == 0)
        {
            HeldRoutes = Panel->GetMetaData<FRouteCache>();
            if (!HeldRoutes || !HeldRoutes->IsReady()) { return false; }
            const auto OtherRoutes = RouteOnlyEditor->GetGraphPanel()->GetMetaData<FRouteCache>();
            if (!OtherRoutes || !OtherRoutes->IsReady()) { return false; }
            ClosedRouteOnlyMeasurement = RouteOnlyEditor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
            Test.TestTrue(TEXT("A panel never formatted owns automatic measurements"), ClosedRouteOnlyMeasurement.IsValid());
            MenuCount = FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor")).GetAllGraphEditorContextMenuExtender().Num();
            CheckRegistrations(true);
            Factory = MakeShared<FDelayedNodeFactory>(); Factory->Target = Fixture->Branch;
            FEdGraphUtilities::RegisterVisualNodeFactory(Factory);
            const auto Node = Panel->GetNodeWidgetFromGuid(Fixture->Print->NodeGuid);
            const auto Pin = Node ? Node->FindWidgetForPin(Fixture->Print->FindPinChecked(TEXT("InString"))) : nullptr;
            const auto Tooltip = Pin ? Pin->GetToolTip() : nullptr;
            Test.TestTrue(TEXT("Prime the native serialized function-pin tooltip"), Tooltip && !Tooltip->IsEmpty());
            if (const auto InitialMeasurements = Panel->GetMetaData<FMeasurementCache>();
                Test.TestTrue(TEXT("Automatic routing warms measurements before any F"), InitialMeasurements.IsValid()))
            {
                Test.TestEqual(TEXT("Automatic routing caches every native node"), InitialMeasurements->GetEntryCount(), Fixture->Graph->Nodes.Num());
                InitialMeasurements->Invalidate();
            }
            Package->SetDirtyFlag(false); Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("F starts a pending format before module shutdown"), PressF());
            Test.TestEqual(TEXT("Pending format tried the delayed native widget once"), Factory->Readiness->Creations, 1);
            CheckUnchanged(TEXT("Pending format"));
            ClosedMeasurement = Panel->GetMetaData<FMeasurementCache>();
            Test.TestTrue(TEXT("Pending format owns a measurement cache"), ClosedMeasurement.IsValid());
            HeldRoutes->Invalidate(); OldBuilds = HeldRoutes->GetBuildCount();
            auto* Module = Modules.GetModule(TEXT("GlooPrint"));
            if (!Test.TestNotNull(TEXT("Native module manager owns GlooPrint"), Module)) { return Finish(); }
            PreviousModuleIdentity = Module;
            Test.TestTrue(TEXT("Loaded module owns its settings delegate"), GetDefault<UGlooPrintSettings>()->OnChanged.IsBoundToObject(Module));
            Module->PreUnloadCallback();
            bNeedsReload = Modules.UnloadModule(TEXT("GlooPrint"), false, false);
            if (!Test.TestTrue(TEXT("Native module manager unloads the module instance"), bNeedsReload)) { return Finish(); }
            CheckRegistrations(false);
            Test.TestFalse(TEXT("Shutdown releases the pending measurement cache"), ClosedMeasurement.IsValid());
            Test.TestFalse(TEXT("Shutdown releases measurements in a panel never formatted"), ClosedRouteOnlyMeasurement.IsValid());
            Test.TestFalse(TEXT("Shutdown removes route metadata from the live panel"), Panel->GetMetaData<FRouteCache>().IsValid());
            Test.TestTrue(TEXT("Shutdown clears retained route data"), !HeldRoutes->IsReady() && HeldRoutes->GetRoutes().Wires.IsEmpty());
            FEdGraphUtilities::UnregisterVisualNodeFactory(Factory); Factory.Reset();
            Fixture->Graph->NotifyGraphChanged(); Slate.InvalidateAllWidgets(false);
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); HeldRoutes->Invalidate();
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            CheckRegistrations(false); CheckUnchanged(TEXT("Unloaded module after later frames"));
            Test.TestEqual(TEXT("Stopped route cache performs no queued rebuild"), HeldRoutes->GetBuildCount(), OldBuilds);
            Test.TestFalse(TEXT("Unloaded module recreates no measurement metadata"), Panel->GetMetaData<FMeasurementCache>().IsValid());
            Test.TestFalse(TEXT("Unloaded module recreates no route metadata"), Panel->GetMetaData<FRouteCache>().IsValid());
            Test.TestFalse(TEXT("Routing-only panel recreates no measurement metadata while unloaded"),
                RouteOnlyEditor->GetGraphPanel()->GetMetaData<FMeasurementCache>().IsValid());
            Capture(TEXT("GlooPrint-ModuleStopped.png"));
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly); PressF();
            CheckUnchanged(TEXT("F with GlooPrint unloaded"));
            if (!Reload()) { return Finish(); }
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            const auto NewRoutes = Panel->GetMetaData<FRouteCache>();
            if (!NewRoutes || !NewRoutes->IsReady()) { return false; }
            CheckRegistrations(true); CheckUnchanged(TEXT("Module reload"));
            Test.TestTrue(TEXT("Reload rebuilds routes in a fresh cache without F"), NewRoutes != HeldRoutes);
            Test.TestEqual(TEXT("Reload retains both original wire pairs"), NewRoutes->GetRoutes().Wires.Num(), 2);
            Test.TestEqual(TEXT("Reloaded routes need no native fallback"), NewRoutes->GetRoutes().FallbackCount, 0);
            Test.TestEqual(TEXT("Retired cache remains stopped after module reload"), HeldRoutes->GetBuildCount(), OldBuilds);
            Test.TestFalse(TEXT("Reload never revives the old pending measurement cache"), ClosedMeasurement.IsValid());
            const auto ReloadedMeasurements = Panel->GetMetaData<FMeasurementCache>();
            Test.TestTrue(TEXT("Reloaded routing warms a fresh measurement cache before F"),
                ReloadedMeasurements && ReloadedMeasurements->GetEntryCount() == Fixture->Graph->Nodes.Num());
            Capture(TEXT("GlooPrint-ModuleReloaded.png"));
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Reloaded module handles actual F"), PressF());
            Phase = 5; return false;
        }
        if (Phase == 5)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Only the new explicit F formats the graph"), After != Before);
            Test.TestEqual(TEXT("Reloaded F creates exactly one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            Test.TestTrue(TEXT("Reloaded format marks the owned package dirty"), Package->IsDirty());
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            Test.TestTrue(TEXT("Reloaded format undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Reloaded undo restores all original node and pin values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Reloaded format redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Reloaded redo restores all formatted values"), After == SerializeNodes(*Fixture->Graph));
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly); PressF();
            Phase = 6; Frames = 0; return false;
        }
        Test.TestTrue(TEXT("Reloaded second F is a no-op"), After == SerializeNodes(*Fixture->Graph));
        Test.TestEqual(TEXT("Reloaded no-op adds no extra transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
        ClosedMeasurement = Panel->GetMetaData<FMeasurementCache>(); ClosedRoutes = Panel->GetMetaData<FRouteCache>();
        ClosedRouteOnlyMeasurement = RouteOnlyEditor->GetGraphPanel()->GetMetaData<FMeasurementCache>();
        Test.TestTrue(TEXT("Reloaded routing-only panel has fresh measurements before closure"), ClosedRouteOnlyMeasurement.IsValid());
        Test.TestTrue(TEXT("Reloaded panel owns both caches before closure"), ClosedMeasurement.IsValid() && ClosedRoutes.IsValid());
        HeldRoutes.Reset();
        Window->RequestDestroyWindow(); Editor.Reset(); Window.Reset();
        RouteOnlyWindow->RequestDestroyWindow(); RouteOnlyEditor.Reset(); RouteOnlyWindow.Reset();
        Phase = 4; Frames = 0; return false;
    }
private:
    void CheckRegistrations(bool bLoaded)
    {
        Test.TestEqual(TEXT("Native module loaded state matches lifecycle"), FModuleManager::Get().IsModuleLoaded(TEXT("GlooPrint")), bLoaded);
        Test.TestEqual(TEXT("Shortcut context follows module lifetime"), FInputBindingManager::Get().GetContextByName(TEXT("GlooPrint")).IsValid(), bLoaded);
        Test.TestEqual(TEXT("Diagnostic command follows module lifetime"), IConsoleManager::Get().FindConsoleObject(TEXT("GlooPrint.MeasureGraph")) != nullptr, bLoaded);
        const auto Container = FModuleManager::GetModuleChecked<ISettingsModule>(TEXT("Settings")).GetContainer(TEXT("Editor"));
        const auto Category = Container ? Container->GetCategory(TEXT("Plugins")) : nullptr;
        const auto Section = Category ? Category->GetSection(TEXT("GlooPrint")) : nullptr;
        Test.TestEqual(TEXT("Preferences section follows module lifetime"), Section.IsValid(), bLoaded);
        if (Section) { Test.TestTrue(TEXT("Reloaded preferences still own the original settings CDO"), Section->GetSettingsObject().Get() == GetDefault<UGlooPrintSettings>()); }
        const int32 CurrentMenus = FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor")).GetAllGraphEditorContextMenuExtender().Num();
        Test.TestEqual(TEXT("Exactly one graph-menu extender follows module lifetime"), CurrentMenus, MenuCount - (bLoaded ? 0 : 1));
        if (!bLoaded)
        {
            Test.TestFalse(TEXT("The destroyed module leaves no raw settings delegate"), GetDefault<UGlooPrintSettings>()->OnChanged.IsBoundToObject(PreviousModuleIdentity));
        }
        else
        {
            TArray<TSharedPtr<FUICommandInfo>> Commands;
            FInputBindingManager::Get().GetCommandInfosFromContext(TEXT("GlooPrint"), Commands);
            Test.TestEqual(TEXT("Reload registers exactly one Format Graph command"), Commands.Num(), 1);
        }
    }
    void CheckUnchanged(const FString& Context)
    {
        Test.TestTrue(Context + TEXT(" preserves every graph value"), Before == SerializeNodes(*Fixture->Graph));
        Test.TestEqual(Context + TEXT(" adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestFalse(Context + TEXT(" leaves the owned package clean"), Package->IsDirty());
    }
    bool Reload()
    {
        if (!bNeedsReload) { return true; }
        bNeedsReload = false;
        return Test.TestTrue(TEXT("Native module manager loads and starts GlooPrint again"), FModuleManager::Get().LoadModuleWithCallback(TEXT("GlooPrint"), *GLog));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native module lifecycle graph"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save native module lifecycle capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
    }
    bool PressF() { return FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)); }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (Factory) { FEdGraphUtilities::UnregisterVisualNodeFactory(Factory); Factory.Reset(); }
        if (Window) { Window->RequestDestroyWindow(); Editor.Reset(); Window.Reset(); }
        if (RouteOnlyWindow) { RouteOnlyWindow->RequestDestroyWindow(); RouteOnlyEditor.Reset(); RouteOnlyWindow.Reset(); }
        Reload(); HeldRoutes.Reset();
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
        if (Package) { Package->SetDirtyFlag(false); }
    }
    bool Finish() { Restore(); return true; }
    FAutomationTestBase& Test;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SGraphEditor> RouteOnlyEditor;
    TSharedPtr<SWindow> RouteOnlyWindow;
    TSharedPtr<FDelayedNodeFactory> Factory;
    TSharedPtr<FRouteCache> HeldRoutes;
    TWeakPtr<FRouteCache> ClosedRoutes;
    TWeakPtr<FMeasurementCache> ClosedMeasurement;
    TWeakPtr<FMeasurementCache> ClosedRouteOnlyMeasurement;
    TArray<uint8> Before, After;
    FVector2D OriginalCursor;
    const void* PreviousModuleIdentity = nullptr;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    int32 Phase = 0, Frames = 0, Queue = 0, MenuCount = 0, OldBuilds = 0;
    double Deadline = 0;
    bool bOriginalEnabled = true, bRestore = false, bNeedsReload = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModuleLifecycleTest, "GlooPrint.Editor.ModuleLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModuleLifecycleTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FModuleLifecycleCheck(*this)); return true;
}
}
#endif
