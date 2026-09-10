// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintWireDrawing.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FSearchWireCheck final : public IAutomationLatentCommand
{
public:
    explicit FSearchWireCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalCursor = Slate.GetCursorPos();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            Fixture->Branch = Fixture->Add<UK2Node_IfThenElse>({0, 300});
            auto* Target = Fixture->Add<UK2Node_ExecutionSequence>({2500, 300});
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
            Key = {Fixture->Branch->NodeGuid, Output->PinId, Target->NodeGuid, Input->PinId};
            for (const FVector2f Position : {FVector2f(600, -50), FVector2f(1200, 550), FVector2f(1800, -50)})
            {
                auto* Obstacle = Fixture->Add<UK2Node_ExecutionSequence>(Position);
                for (int32 I = 0; I < 24; ++I) { Obstacle->AddInputPin(); }
            }
            Gate({100, 0}, 2700, TEXT("Upper search boundary"));
            Gate({100, 1200}, 2700, TEXT("Lower search boundary"));
            Before = SerializeNodes(*Fixture->Graph);
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint searched route"))).ClientSize(FVector2f(1200, 850))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -150), 0.42f);
            Deadline = FPlatformTime::Seconds() + 30;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Searched route fixture did not settle in 30 seconds.")); return Finish(); }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        const auto* Route = Cache->GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Native fixture finds a custom searched route"), Route && Route->Method == ERouteMethod::Search))
        {
            if (Route)
            {
                FString Points;
                for (FVector2f P : Route->Points) { Points += FString::Printf(TEXT(" %.2f:%.2f"), P.X, P.Y); }
                Test.AddInfo(FString::Printf(TEXT("Unexpected route: method=%d fallback=%d expanded=%d points=%s"),
                    int32(Route->Method), int32(Route->Fallback), Route->Search.ExpandedStates, *Points));
            }
            for (const UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
                if (Widget) { Test.AddInfo(FString::Printf(TEXT("Obstacle %s at %d:%d size=%s"),
                    *Node->GetName(), Node->NodePosX, Node->NodePosY, *Widget->GetDesiredSize().ToString())); }
            }
            return Finish();
        }
        if (++Frames < 8) { return false; }
        if (Phase == 0)
        {
            Test.TestEqual(TEXT("Native obstacle fixture has no fallback"), Cache->GetRoutes().FallbackCount, 0);
            Test.TestTrue(TEXT("Search leaves all serialized native node/pin data unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.AddInfo(FString::Printf(TEXT("Native search: %d expanded states, %d segment checks, %d bends."),
                Route->Search.ExpandedStates, Route->Search.SegmentChecks, Route->Points.Num() - 2));
            const FVector2f P = EvaluateRoute(*Route, Route->Length * 0.5f);
            Mouse = Panel->GetCachedGeometry().LocalToAbsolute((P - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, Mouse);
            Builds = Cache->GetBuildCount(); Phase = 1; Frames = 0;
            return false;
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        Test.TestTrue(TEXT("Native hover identifies the searched wire's original pins"),
            Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
            ((A == Output && B == Input) || (A == Input && B == Output)));
        Test.TestEqual(TEXT("Painting the searched wire does not repeat A*"), Cache->GetBuildCount(), Builds);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture searched native route"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save searched route screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-SearchedRoute.png"))));
        }
        return Finish();
    }
private:
    void Gate(FVector2f Position, int32 Width, const TCHAR* Label)
    {
        auto* Comment = Fixture->Add<UEdGraphNode_Comment>(Position);
        Comment->NodeWidth = Width; Comment->NodeHeight = 110; Comment->NodeComment = Label;
        for (int32 I = 1; I < 24; ++I) { Comment->NodeComment += TEXT("  "); Comment->NodeComment += Label; }
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
        Editor.Reset(); Window.Reset(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key;
    TArray<uint8> Before;
    FVector2D OriginalCursor;
    FVector2f Mouse;
    double Deadline = 0;
    int32 Frames = 0, Phase = 0, Builds = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchWireEditorTest, "GlooPrint.Editor.SearchedWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSearchWireEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSearchWireCheck(*this)); return true;
}
}
#endif
