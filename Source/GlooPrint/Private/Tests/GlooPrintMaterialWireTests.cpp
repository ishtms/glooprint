// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintMaterialTestUtils.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "IMaterialEditor.h"
#include "MaterialEditorModule.h"
#include "ShaderCompiler.h"
#include "Framework/Application/SlateApplication.h"
#include "MaterialGraphNode_Knot.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FMaterialWireCheck final : public IAutomationLatentCommand
{
public:
    FMaterialWireCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle) : Test(InTest), Style(InStyle) {}
    ~FMaterialWireCheck() { Restore(); }
    bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle; Cursor = Slate.GetCursorPos(); bRestore = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = Style; GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintMaterialWires_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            Fixture = MakeUnique<FMaterialFixture>(0, 3, Package.Get());
            Fixture->Material->SetFlags(RF_Public | RF_Standalone);
            Fixture->Constant->GraphNode->SetPosition(FVector2f(-400, 100));
            Fixture->Add->GraphNode->SetPosition(FVector2f(180, 160));
            Fixture->Graph->RootNode->SetPosition(FVector2f(800, 40));
            for (UEdGraphNode* Node : Fixture->Graph->Nodes) { ApplyBackingLayout(*Node); }
            AssetEditor = IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, {}, Fixture->Material.Get());
            Fixture->Material.Reset(); // Native undo may replace the original asset.
            Graph = CastChecked<UMaterial>(AssetEditor->GetMaterialInterface())->MaterialGraph;
            for (UMaterialExpression* Expression : Graph->Material->GetExpressions())
            {
                if (auto* Candidate = Cast<UMaterialExpressionAdd>(Expression)) { Add = Candidate; }
            }
            Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (Phase == 8)
        {
            if (++Frames < 15) { return false; }
            Test.TestFalse(TEXT("Closing material graph releases routes"), ClosedCache.IsValid()); return Finish();
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(FString::Printf(TEXT("Material wire test timed out in phase %d"), Phase)); return Finish(); }
        if (!Editor)
        {
            if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) { return false; }
            Editor = SGraphEditor::FindGraphEditorForGraph(Graph);
            if (!Editor) { return false; }
            Window = Slate.FindWidgetWindow(Editor.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-480, -100), .75f); Frames = 0;
        }
        auto* Panel = Editor->GetGraphPanel(); const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady() || ++Frames < 12) { return false; }
        Output = CastChecked<UMaterialGraphNode>(Add->GraphNode)->GetOutputPin(0);
        // During the broken-link phase the input is recovered directly from the root.
        Input = nullptr;
        for (auto* Pin : Graph->RootNode->Pins)
        {
            if (Pin->SourceIndex >= 0 && Graph->MaterialInputs.IsValidIndex(Pin->SourceIndex) &&
                Graph->MaterialInputs[Pin->SourceIndex].GetProperty() == MP_EmissiveColor) { Input = Pin; break; }
        }
        if (!Test.TestNotNull(TEXT("Material emissive input exists"), Input)) { return Finish(); }
        if (Phase == 0 || Phase == 3 || Phase == 5 || Phase == 7)
        {
            const FRouteKey Key{Output->GetOwningNode()->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId};
            const auto* Route = Cache->GetRoutes().Wires.Find(Key);
            if (!Test.TestTrue(TEXT("Material connection has a routed visible curve"), Route && !Route->Curves.IsEmpty())) { return Finish(); }
            const FVector2f Point = EvaluateRoute(*Route, Route->Length * .65f);
            Mouse = Panel->GetCachedGeometry().LocalToAbsolute((Point - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            MoveMouse(*Panel);
            Builds = Cache->GetBuildCount(); return Next(Phase == 0 ? 1 : Phase == 3 ? 4 : Phase == 5 ? 6 : 9);
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        if (Phase == 1 || Phase == 4 || Phase == 6 || Phase == 9)
        {
            if (!Test.TestTrue(TEXT("Native hover resolves original material pins"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
                ((A == Output && B == Input) || (B == Output && A == Input)))) { return Finish(); }
            Test.TestEqual(TEXT("Idle material hover reuses completed routes"), Cache->GetBuildCount(), Builds);
        }
        if (Phase == 1)
        {
            const FModifierKeysState Shift(true, false, false, false, false, false, false, false, false);
            Click(Shift);
            const auto Marked = Panel->MarkedPin.Pin();
            Test.TestTrue(TEXT("Shift-click material wire marks an original endpoint"), Marked && (Marked->GetPinObj() == Output || Marked->GetPinObj() == Input));
            Click(Shift); Test.TestFalse(TEXT("Second Shift-click clears the mark"), Panel->MarkedPin.IsValid());
            MoveMouse(*Panel);
            return Next(10);
        }
        if (Phase == 10)
        {
            Test.TestTrue(TEXT("Wire remains hovered before double-click"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B));
            Nodes = Graph->Nodes.Num();
            Click(FModifierKeysState());
            const FPointerEvent DoubleClick(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
            Slate.ProcessMouseButtonDoubleClickEvent(Window->GetNativeWindow(), DoubleClick);
            Slate.ProcessMouseButtonUpEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {}, EKeys::LeftMouseButton, 0, FModifierKeysState()));
            Test.TestEqual(TEXT("Double-click creates one native material reroute"), Graph->Nodes.Num(), Nodes + 1);
            Test.TestTrue(TEXT("Material schema inserts a real knot"), Graph->Nodes.ContainsByPredicate([](const auto& Node) { return Node->template IsA<UMaterialGraphNode_Knot>(); }));
            Test.TestTrue(TEXT("Material reroute supports one-step undo"), GEditor->UndoTransaction()); return Next(3);
        }
        if (Phase == 4)
        {
            const FModifierKeysState Alt(false, false, false, false, true, false, false, false, false);
            Click(Alt);
            Test.TestFalse(TEXT("Alt-click disconnects the material wire through its schema"), Output->LinkedTo.Contains(Input));
            Test.TestTrue(TEXT("Material disconnect supports undo"), GEditor->UndoTransaction());
            Editor->SetViewLocation(FVector2f(-500, -150), .5f); return Next(5);
        }
        if (Phase == 6)
        {
            Test.TestEqual(TEXT("Gestures restore the original material nodes"), Graph->Nodes.Num(), Nodes);
            Add->GraphNode->ReconstructNode(); Graph->NotifyGraphChanged(); return Next(7);
        }
        if (Phase == 9)
        {
            const FModifierKeysState Control(false, false, true, false, false, false, false, false, false);
            Slate.ProcessMouseButtonDownEvent(Window->GetNativeWindow(), FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Mouse,
                {EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, Control));
            if (!Test.TestTrue(TEXT("Control-click starts native material reconnection"), Slate.GetDragDroppingContent().IsValid())) { return Finish(); }
            Test.TestFalse(TEXT("Native reconnection detaches the hovered material link"), Output->LinkedTo.Contains(Input));
            UEdGraphPin* Target = nullptr;
            for (auto* Pin : Graph->RootNode->Pins)
            {
                if (Graph->MaterialInputs.IsValidIndex(Pin->SourceIndex) &&
                    Graph->MaterialInputs[Pin->SourceIndex].GetProperty() == MP_BaseColor) { Target = Pin; break; }
            }
            const auto RootWidget = Panel->GetNodeWidgetFromGuid(Graph->RootNode->NodeGuid);
            const auto TargetWidget = Target && RootWidget ? RootWidget->FindWidgetForPin(Target) : nullptr;
            if (!Test.TestTrue(TEXT("Native material reconnection target is visible"), TargetWidget.IsValid())) { return Finish(); }
            const FVector2f Previous = Mouse;
            Mouse = TargetWidget->GetCachedGeometry().LocalToAbsolute(TargetWidget->GetCachedGeometry().GetLocalSize() * .5f);
            Slate.SetCursorPos(FVector2D(Mouse)); Mouse = Slate.GetCursorPos();
            Slate.ProcessMouseMoveEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Previous,
                {EKeys::LeftMouseButton}, EKeys::Invalid, 0, Control), false);
            Slate.ProcessMouseButtonUpEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {}, EKeys::LeftMouseButton, 0, Control));
            Test.TestFalse(TEXT("Material reconnection ends native dragging"), Slate.GetDragDroppingContent().IsValid());
            Test.TestTrue(TEXT("Native material reconnection reaches the new property"), Output->LinkedTo.Contains(Target) && !Output->LinkedTo.Contains(Input));
            Test.TestTrue(TEXT("Undo material reconnection creation"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo material reconnection detachment"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Material reconnection undo restores the original property"), Output->LinkedTo.Contains(Input) && !Output->LinkedTo.Contains(Target));
            return Next(11);
        }
        if (Phase == 11)
        {
            ClosedCache = Cache; AssetEditor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); AssetEditor.Reset(); Window.Reset(); Editor.Reset(); return Next(8);
        }
        return false;
    }
private:
    void MoveMouse(SGraphPanel& Panel)
    {
        auto& Slate = FSlateApplication::Get();
        Window->HACK_ForceToFront();
        const FVector2f Previous = Slate.GetCursorPos();
        Slate.SetCursorPos(FVector2D(Mouse)); Mouse = Slate.GetCursorPos();
        Slate.ProcessMouseMoveEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Previous, {}, EKeys::Invalid, 0, FModifierKeysState()), false);
        Test.TestTrue(TEXT("Native material graph receives pointer motion"), Panel.IsHovered());
    }
    void Click(const FModifierKeysState& Modifiers)
    {
        auto& Slate = FSlateApplication::Get();
        Slate.ProcessMouseButtonDownEvent(Window->GetNativeWindow(), FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Mouse,
            {EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, Modifiers));
        Slate.ProcessMouseButtonUpEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Mouse, Mouse, {}, EKeys::LeftMouseButton, 0, Modifiers));
    }
    bool Next(int32 NewPhase) { Phase = NewPhase; Frames = 0; return false; }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        if (AssetEditor) { AssetEditor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); AssetEditor.Reset(); }
        Window.Reset();
        Editor.Reset();
        if (bRestore)
        {
            bRestore = false; GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); FSlateApplication::Get().SetCursorPos(Cursor);
        }
    }
    FAutomationTestBase& Test;
    EGlooPrintWireStyle Style, OriginalStyle;
    TUniquePtr<FMaterialFixture> Fixture;
    TStrongObjectPtr<UPackage> Package;
    TSharedPtr<IMaterialEditor> AssetEditor;
    UMaterialGraph* Graph = nullptr;
    UMaterialExpressionAdd* Add = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> ClosedCache;
    UEdGraphPin* Output = nullptr; UEdGraphPin* Input = nullptr;
    FVector2D Cursor; FVector2f Mouse;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, Nodes = 0;
    bool bRestore = false;
};

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMaterialWireGesturesTest, "GlooPrint.Materials.WireGestures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FMaterialWireGesturesTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    Names.Add(TEXT("Rounded")); Commands.Add(TEXT("0")); Names.Add(TEXT("Diagonal")); Commands.Add(TEXT("1"));
}
bool FMaterialWireGesturesTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FMaterialWireCheck(*this, Parameters == TEXT("0") ? EGlooPrintWireStyle::Rounded90 : EGlooPrintWireStyle::Diagonal45)); return true;
}
}
#endif
