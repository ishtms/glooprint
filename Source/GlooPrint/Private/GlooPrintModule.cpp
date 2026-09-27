// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintMeasurement.h"
#include "GlooPrintEditor.h"
#include "GlooPrintWireDrawing.h"
#include "GlooPrintSettings.h"

#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogGlooPrint, Log, All);

class FGlooPrintModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        bStopped = false;
        // Normal editor exit destroys Slate and UObjects before unloading modules.
        // Release our editor integrations while both systems are still available.
        PreExitHandle = FEditorDelegates::OnEditorPreExit.AddRaw(this, &FGlooPrintModule::ShutdownModule);
        if (!IsRunningCommandlet() && FSlateApplication::IsInitialized())
        {
            Editor = MakeShared<GlooPrint::FEditor>();
            Editor->Initialize();
            WireDrawing = MakeShared<GlooPrint::FWireDrawing>();
            FEdGraphUtilities::RegisterVisualPinConnectionFactory(WireDrawing);
            WireDrawing->InitializeMaterialPanels();
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            SettingsObject = Settings;
            SettingsChangedHandle = Settings->OnChanged.AddRaw(this, &FGlooPrintModule::OnSettingsChanged);
            SettingsSection = FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).RegisterSettings(
                TEXT("Editor"), TEXT("Plugins"), TEXT("GlooPrint"), NSLOCTEXT("GlooPrint", "SettingsName", "GlooPrint"),
                NSLOCTEXT("GlooPrint", "SettingsDescription", "Format Blueprint, Material and Voxel graphs and choose their wire style."), Settings);
            if (const auto Section = SettingsSection.Pin())
            {
                Section->OnModified().BindUObject(Settings, &UGlooPrintSettings::NotifyChanged);
            }
        }
        MeasureCommand = MakeUnique<FAutoConsoleCommand>(
            TEXT("GlooPrint.MeasureGraph"),
            TEXT("Read-only geometry prototype. Usage: GlooPrint.MeasureGraph <loaded graph object path>"),
            FConsoleCommandWithArgsDelegate::CreateStatic(&MeasureGraph));
    }

    virtual void ShutdownModule() override
    {
        if (bStopped) { return; }
        bStopped = true;
        FEditorDelegates::OnEditorPreExit.Remove(PreExitHandle);
        PreExitHandle.Reset();
        // Never fetch/create a CDO during teardown, including commandlet shutdown.
        if (auto* Settings = SettingsObject.Get()) { Settings->OnChanged.Remove(SettingsChangedHandle); }
        SettingsObject.Reset();
        SettingsChangedHandle.Reset();
        if (const auto Section = SettingsSection.Pin()) { Section->OnModified().Unbind(); }
        SettingsSection.Reset();
        if (auto* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
        {
            SettingsModule->UnregisterSettings(TEXT("Editor"), TEXT("Plugins"), TEXT("GlooPrint"));
        }
        if (WireDrawing)
        {
            FEdGraphUtilities::UnregisterVisualPinConnectionFactory(WireDrawing);
            WireDrawing->Shutdown(); WireDrawing.Reset();
        }
        if (Editor) { Editor->Shutdown(); Editor.Reset(); }
        MeasureCommand.Reset();
    }

private:
    void OnSettingsChanged()
    {
        if (Editor) { Editor->OnSettingsChanged(); }
        if (WireDrawing) { WireDrawing->OnSettingsChanged(); }
    }

    static void MeasureGraph(const TArray<FString>& Args)
    {
        if (!IsInGameThread() || !FSlateApplication::IsInitialized())
        {
            UE_LOG(LogGlooPrint, Display, TEXT("Measurement requires the editor thread and initialized Slate."));
            return;
        }
        if (Args.Num() != 1)
        {
            UE_LOG(LogGlooPrint, Display, TEXT("Usage: GlooPrint.MeasureGraph /Game/Folder/BP_Name.BP_Name:EventGraph (open the Blueprint first)"));
            return;
        }
        UEdGraph* Graph = FindObject<UEdGraph>(nullptr, *Args[0]);
        const TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();
        if (!Window)
        {
            UE_LOG(LogGlooPrint, Display, TEXT("Measurement requires an active editor window for its display scale."));
            return;
        }
        const float LayoutScale = Window->GetDPIScaleFactor() * FSlateApplication::Get().GetApplicationScale();
        GlooPrint::FGraphMeasurement Measurement;
        FString Reason;
        if (!GlooPrint::MeasureGraph(Graph, LayoutScale, Measurement, Reason))
        {
            UE_LOG(LogGlooPrint, Display, TEXT("Measurement stopped: %s"), *Reason);
            return;
        }
        int32 PinCount = 0;
        int32 MeasuredPinCount = 0;
        for (const GlooPrint::FMeasuredNode& Node : Measurement.Nodes)
        {
            for (const GlooPrint::FMeasuredPin& Pin : Node.Pins)
            {
                ++PinCount;
                MeasuredPinCount += Pin.AttachmentOffset.IsSet() ? 1 : 0;
            }
            UE_LOG(LogGlooPrint, Display, TEXT("%s: body %.1f x %.1f, visual [%.1f, %.1f] to [%.1f, %.1f], %d pins"),
                *Node.Id.ToString(), Node.BodySize.X, Node.BodySize.Y,
                Node.VisualBounds.Min.X, Node.VisualBounds.Min.Y,
                Node.VisualBounds.Max.X, Node.VisualBounds.Max.Y, Node.Pins.Num());
        }
        UE_LOG(LogGlooPrint, Display, TEXT("Measured %d nodes, %d/%d pin attachments in %s. No layout changes."),
            Measurement.Nodes.Num(), MeasuredPinCount, PinCount, *Graph->GetPathName());
    }

    TUniquePtr<FAutoConsoleCommand> MeasureCommand;
    TSharedPtr<GlooPrint::FEditor> Editor;
    TSharedPtr<GlooPrint::FWireDrawing> WireDrawing;
    TWeakPtr<ISettingsSection> SettingsSection;
    TWeakObjectPtr<UGlooPrintSettings> SettingsObject;
    FDelegateHandle SettingsChangedHandle;
    FDelegateHandle PreExitHandle;
    bool bStopped = false;
};

IMPLEMENT_MODULE(FGlooPrintModule, GlooPrint)
