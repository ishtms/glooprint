// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "Settings/EditorStyleSettings.h"
#include "GlooPrintWireDrawing.h"
#include "ConnectionDrawingPolicy.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "ISettingsModule.h"
#include "ISettingsContainer.h"
#include "ISettingsCategory.h"
#include "ISettingsSection.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "SGraphPanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include <limits>

namespace GlooPrint::Tests
{
class FSettingsCheck final : public IAutomationLatentCommand
{
public:
    explicit FSettingsCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FSettingsCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        if (!Fixture)
        {
            OriginalLayout = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            OriginalGrid = GetDefault<UEditorStyleSettings>()->GridSnapSize;
            bOriginalEnabled = Settings->bFormattingEnabled; OriginalCursor = Slate.GetCursorPos(); bRestore = true;
            Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = false; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            Fixture->Branch = Fixture->Add<UK2Node_IfThenElse>({0, 0});
            Upper = Fixture->Add<UK2Node_ExecutionSequence>({600, 300});
            Lower = Fixture->Add<UK2Node_ExecutionSequence>({600, 700});
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Upper->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            Test.TestTrue(TEXT("Create native branch connection"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input));
            Test.TestTrue(TEXT("Create native alternative connection"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
                Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else), Lower->FindPinChecked(UEdGraphSchema_K2::PN_Execute)));
            Key = {Fixture->Branch->NodeGuid, Output->PinId, Upper->NodeGuid, Input->PinId};
            Comment = Fixture->Add<UEdGraphNode_Comment>({-80, -80});
            Comment->NodeWidth = 1100; Comment->NodeHeight = 1100; Comment->NodeComment = TEXT("Wire styles and independent formatting");
            Before = SerializeNodes(*Fixture->Graph);
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint preferences and wire styles")))
                .ClientSize(FVector2f(1200, 850))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-140, -130), 0.75f);
            Editor->SetNodeSelection(Fixture->Branch, true);
            Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Settings fixture did not settle in 45 seconds.")); return Finish(); }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if ((Phase == 0 || Phase == 1 || Phase == 6) && (!Cache || !Cache->IsReady())) { return false; }
        if (++Frames < 8) { return false; }
        if (Phase == 0)
        {
            const auto* Route = Cache->GetRoutes().Wires.Find(Key);
            if (!Test.TestTrue(TEXT("Custom routes work with formatting disabled"), Route && !Route->Curves.IsEmpty())) { return Finish(); }
            RoundedPoints = Route->Points;
            const int32 DisabledQueue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            FEditor InputProbe;
            Test.TestFalse(TEXT("Disabled formatter yields F to other handlers"), InputProbe.HandleKeyDownEvent(Slate, KeyEvent()));
            Slate.ProcessKeyDownEvent(KeyEvent());
            Test.TestTrue(TEXT("Disabled actual F does not format"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Disabled F creates no undo entry"), GEditor->Trans->GetQueueLength(), DisabledQueue);
            int32 Changed = -1; FString Reason;
            Test.TestFalse(TEXT("Disabled programmatic command also refuses"), FormatGraph(Fixture->Graph, 1, {}, Changed, Reason));
            Test.TestEqual(TEXT("Disabled command changes no nodes"), Changed, 0);
            Test.TestTrue(TEXT("Disabled explicit command explains the setting"), Reason.Contains(TEXT("disabled")));
            Settings->WireStyle = EGlooPrintWireStyle::Diagonal45;
            FPropertyChangedEvent Event(FindFProperty<FProperty>(UGlooPrintSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UGlooPrintSettings, WireStyle)));
            Settings->PostEditChangeProperty(Event);
            return Advance();
        }
        if (Phase == 1)
        {
            const auto* Route = Cache->GetRoutes().Wires.Find(Key);
            if (!Test.TestTrue(TEXT("Changing style rebuilds custom routes"), Route && !Route->Curves.IsEmpty())) { return Finish(); }
            Test.TestTrue(TEXT("Style preserves selected routing corridor"), Route->Points == RoundedPoints);
            const FRouteCurve* Diagonal = Route->Curves.FindByPredicate([](const auto& C)
            {
                const auto D = C.End - C.Start;
                return FMath::Abs(D.X) > 4 && FMath::Abs(D.Y) > 4 && C.StartTangent == C.EndTangent;
            });
            if (!Test.TestTrue(TEXT("Native graph renders a diagonal segment"), Diagonal != nullptr)) { return Finish(); }
            const FVector2f P = (Diagonal->Start + Diagonal->End) * 0.5f;
            Mouse = Panel->GetCachedGeometry().LocalToAbsolute((P - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, Mouse);
            Builds = Cache->GetBuildCount();
            return Advance();
        }
        if (Phase == 2)
        {
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestTrue(TEXT("Hover on the visible diagonal identifies the original pin pair"),
                Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
                ((A == Output && B == Input) || (A == Input && B == Output)));
            Test.TestTrue(TEXT("Restyling leaves serialized graph data unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Diagonal painting reuses routes"), Cache->GetBuildCount(), Builds);
            Capture(Editor.ToSharedRef(), TEXT("GlooPrint-DiagonalStyle.png"));
            Settings->WireStyle = EGlooPrintWireStyle::Native; Settings->NotifyChanged();
            return Advance();
        }
        if (Phase == 3)
        {
            Test.TestFalse(TEXT("Native mode releases ready custom routes"), Cache->IsReady());
            Test.TestTrue(TEXT("Native mode empties custom route values"), Cache->GetRoutes().Wires.IsEmpty());
            auto Factory = MakeShared<FWireDrawing>();
            FSlateWindowElementList Elements(Window);
            TUniquePtr<FConnectionDrawingPolicy> Policy(Factory->CreateConnectionPolicy(Fixture->Graph->GetSchema(), 0, 1, 1,
                FSlateRect(0, 0, 1200, 850), Elements, Fixture->Graph));
            Test.TestFalse(TEXT("Native setting yields connection factory ownership"), Policy.IsValid());
            Fixture->Graph->NotifyGraphChanged();
            return Advance();
        }
        if (Phase == 4)
        {
            Test.TestEqual(TEXT("Graph notifications do not run custom search in native mode"), Cache->GetBuildCount(), Builds);
            Test.TestTrue(TEXT("Switching to native wires preserves graph data"), Before == SerializeNodes(*Fixture->Graph));
            Capture(Editor.ToSharedRef(), TEXT("GlooPrint-NativeStyle.png"));
            Settings->bFormattingEnabled = true; Settings->HorizontalSpacing = 192; Settings->VerticalSpacing = 112; Settings->CommentPadding = 64;
            GetMutableDefault<UEditorStyleSettings>()->GridSnapSize = 32;
            Settings->NotifyChanged();
            Test.TestTrue(TEXT("Editing spacing does not move nodes automatically"), Before == SerializeNodes(*Fixture->Graph));
            CheckPersistence();
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("F works while custom styling is off"), Slate.ProcessKeyDownEvent(KeyEvent()));
            Phase = 8; return false;
        }
        if (Phase == 8)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Native-wire F actually formats"), After != Before);
            Test.TestEqual(TEXT("Configured format is one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            CheckSpacing();
            Test.TestTrue(TEXT("Configured format undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Configured format undo restores exact graph"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Configured format redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Configured format redo restores exact result"), SerializeNodes(*Fixture->Graph) == After);
            Slate.ProcessKeyDownEvent(KeyEvent());
            Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            Test.TestTrue(TEXT("Repeated format with settings is a cold no-op"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Configured no-op creates no transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            FVector2f CurrentView; float CurrentZoom;
            Editor->GetViewLocation(CurrentView, CurrentZoom);
            Test.TestEqual(TEXT("Configured formatting preserves camera"), CurrentView, View);
            Test.TestEqual(TEXT("Configured formatting preserves zoom"), CurrentZoom, Zoom);
            Test.TestEqual(TEXT("Configured formatting retains the anchor"), Fixture->Branch->NodePosX, 0);
            Settings->bFormattingEnabled = false; Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->NotifyChanged();
            return Advance();
        }
        if (Phase == 6)
        {
            Test.TestTrue(TEXT("Custom wires rebuild when re-enabled without F"), Cache->GetBuildCount() > Builds);
            Test.TestTrue(TEXT("Re-enabling wires leaves formatted data alone"), SerializeNodes(*Fixture->Graph) == After);
            Capture(Editor.ToSharedRef(), TEXT("GlooPrint-NativeGrid.png"));
            FModuleManager::GetModuleChecked<ISettingsModule>(TEXT("Settings")).ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("GlooPrint"));
            Preferences = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TEXT("EditorSettings")));
            return Advance();
        }
        if (Test.TestTrue(TEXT("Native Editor Preferences page opens"), Preferences.IsValid()))
        {
            TArray<TSharedRef<SWidget>> Pending{Preferences->GetContent()};
            FString Labels;
            for (int32 I = 0; I < Pending.Num() && I < 16384; ++I)
            {
                const auto Widget = Pending[I];
                if (Widget->GetType() == TEXT("STextBlock")) { Labels += StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() + TEXT("\n"); }
                FChildren* Children = Widget->GetChildren();
                for (int32 C = 0; C < Children->Num(); ++C) { Pending.Add(Children->GetChildAt(C)); }
            }
            for (const TCHAR* Label : {TEXT("Formatting enabled"), TEXT("Wire Style"), TEXT("Horizontal Spacing"), TEXT("Vertical Spacing"), TEXT("Comment Padding")})
            {
                Test.TestTrue(FString(TEXT("Preferences displays ")) + Label, Labels.Contains(Label));
            }
            Capture(Preferences->GetContent(), TEXT("GlooPrint-Preferences.png"));
        }
        return Finish();
    }

private:
    void CheckPersistence()
    {
        auto& Module = FModuleManager::GetModuleChecked<ISettingsModule>(TEXT("Settings"));
        const auto Container = Module.GetContainer(TEXT("Editor"));
        const auto Category = Container ? Container->GetCategory(TEXT("Plugins")) : nullptr;
        const auto Section = Category ? Category->GetSection(TEXT("GlooPrint")) : nullptr;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        if (!Test.TestTrue(TEXT("Editor Preferences owns the actual settings CDO"), Section && Section->GetSettingsObject().Get() == Settings)) { return; }
        Test.TestEqual(TEXT("Settings use project-local user preferences"), Settings->GetClass()->ClassConfigName, FName(TEXT("EditorPerProjectUserSettings")));
        Test.TestFalse(TEXT("Preference edits do not target shared default config"), Settings->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig));
        const FString Filename = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("GlooPrint-Settings-"), TEXT(".ini"));
        if (Test.TestTrue(TEXT("Native settings export saves config properties"), Section->Export(Filename)))
        {
            TStrongObjectPtr<UGlooPrintSettings> Reloaded(NewObject<UGlooPrintSettings>());
            Reloaded->bFormattingEnabled = false; Reloaded->WireStyle = EGlooPrintWireStyle::Rounded90;
            Reloaded->HorizontalSpacing = 16; Reloaded->VerticalSpacing = 16; Reloaded->CommentPadding = 8;
            Reloaded->LoadConfig(nullptr, *Filename);
            Test.TestTrue(TEXT("Config restores formatting enabled"), Reloaded->bFormattingEnabled);
            Test.TestTrue(TEXT("Config restores native style"), Reloaded->WireStyle == EGlooPrintWireStyle::Native);
            Test.TestEqual(TEXT("Config restores horizontal spacing"), Reloaded->HorizontalSpacing, 192.f);
            Test.TestEqual(TEXT("Config restores vertical spacing"), Reloaded->VerticalSpacing, 112.f);
            Test.TestEqual(TEXT("Config restores comment padding"), Reloaded->CommentPadding, 64.f);
            Reloaded->HorizontalSpacing = -100; Reloaded->VerticalSpacing = std::numeric_limits<float>::quiet_NaN();
            Reloaded->CommentPadding = 100000; Reloaded->WireStyle = static_cast<EGlooPrintWireStyle>(255);
            Reloaded->NotifyChanged();
            Test.TestEqual(TEXT("Out-of-range config spacing is bounded"), Reloaded->HorizontalSpacing, 16.f);
            Test.TestEqual(TEXT("Nonfinite config spacing uses the default"), Reloaded->VerticalSpacing, 48.f);
            Test.TestEqual(TEXT("Config padding is bounded"), Reloaded->CommentPadding, 512.f);
            Test.TestTrue(TEXT("Invalid config style uses rounded wires"), Reloaded->WireStyle == EGlooPrintWireStyle::Rounded90);
        }
        IFileManager::Get().Delete(*Filename);
    }
    void CheckSpacing()
    {
        FGraphMeasurement Measured; FString Reason;
        const float Scale = Window->GetDPIScaleFactor() * FSlateApplication::Get().GetApplicationScale();
        if (!Test.TestTrue(TEXT("Measure configured result"), MeasureGraph(Fixture->Graph, Scale, Measured, Reason))) { Test.AddError(Reason); return; }
        const auto& Branch = Find(Measured, Fixture->Branch->NodeGuid);
        const auto& A = Find(Measured, Upper->NodeGuid); const auto& B = Find(Measured, Lower->NodeGuid);
        Test.TestTrue(TEXT("Horizontal spacing reaches actual node bodies"), FMath::Min(A.Position.X, B.Position.X) - Branch.Position.X - Branch.BodySize.X >= 192);
        Test.TestEqual(TEXT("Native editor uses the configured graph grid"), SNodePanel::GetSnapGridSize(), 32u);
        Test.TestEqual(TEXT("Upper native branch column follows the editor grid"), (A.Position.X - Branch.Position.X) % 32, 0);
        Test.TestEqual(TEXT("Lower native branch column follows the editor grid"), (B.Position.X - Branch.Position.X) % 32, 0);
        Test.AddInfo(FString::Printf(TEXT("Native grid32: upper/lower columns %d/%d graph units from the fixed anchor."),
            A.Position.X - Branch.Position.X, B.Position.X - Branch.Position.X));
        const auto& Top = A.Position.Y < B.Position.Y ? A : B; const auto& Bottom = A.Position.Y < B.Position.Y ? B : A;
        Test.TestTrue(TEXT("Vertical spacing separates branch outputs"), Bottom.Position.Y - Top.Position.Y - Top.BodySize.Y >= 112);
        for (const auto* Node : {&Branch, &A, &B})
        {
            const FVector2f Min = FVector2f(Node->Position) + Node->VisualBounds.Min;
            const FVector2f Max = FVector2f(Node->Position) + Node->VisualBounds.Max;
            Test.TestTrue(TEXT("Configured comment padding encloses visual bounds"), Min.X - Comment->NodePosX >= 63.5f &&
                Comment->NodePosX + Comment->NodeWidth - Max.X >= 63.5f && Comment->NodePosY + Comment->NodeHeight - Max.Y >= 63.5f);
        }
    }
    static FKeyEvent KeyEvent() { return FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0); }
    bool Advance() { ++Phase; Frames = 0; return false; }
    void Capture(TSharedRef<SWidget> Widget, const TCHAR* Filename)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native settings/style fixture"), FSlateApplication::Get().TakeScreenshot(Widget, Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save native settings/style screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Filename)));
        }
    }
    void Restore()
    {
        if (!bRestore) { return; }
        bRestore = false;
        GetMutableDefault<UEditorStyleSettings>()->GridSnapSize = OriginalGrid;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->bFormattingEnabled = bOriginalEnabled; Settings->WireStyle = OriginalStyle;
        Settings->HorizontalSpacing = OriginalLayout.HorizontalSpacing; Settings->VerticalSpacing = OriginalLayout.VerticalSpacing;
        Settings->CommentPadding = OriginalLayout.CommentPadding; Settings->NotifyChanged();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
    }
    bool Finish()
    {
        if (Preferences) { Preferences->RequestCloseTab(); Preferences.Reset(); }
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); Restore(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SDockTab> Preferences;
    UK2Node_ExecutionSequence* Upper = nullptr;
    UK2Node_ExecutionSequence* Lower = nullptr;
    UEdGraphNode_Comment* Comment = nullptr;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key;
    TArray<FVector2f> RoundedPoints;
    TArray<uint8> Before, After;
    FLayoutSettings OriginalLayout;
    uint32 OriginalGrid = 16;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FVector2D OriginalCursor;
    FVector2f Mouse, View;
    float Zoom = 0;
    double Deadline = 0;
    int32 Frames = 0, Phase = 0, Builds = 0, Queue = 0;
    bool bOriginalEnabled = true, bRestore = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsTest, "GlooPrint.Editor.SettingsAndWireStyles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSettingsTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSettingsCheck(*this)); return true;
}
}
#endif
