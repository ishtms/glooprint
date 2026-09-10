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
#include "Kismet/KismetStringLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FPurePackingCheck final : public IAutomationLatentCommand
{
public:
    FPurePackingCheck(FAutomationTestBase& InTest, EGlooPrintWireStyle InStyle, bool bInGroup = false)
        : Test(InTest), Style(InStyle), bGroup(bInGroup) {}
    virtual ~FPurePackingCheck()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        if (bRestore)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
            Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle;
            Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        }
    }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
            bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
            Settings->WireStyle = Style; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(AActor::StaticClass(), false);
            for (int32 C = 0; C < 2; ++C)
            {
                auto* Entry = Fixture->Add<UK2Node_CustomEvent>({0, float(C * 700)});
                Entry->CustomFunctionName = C ? TEXT("SecondMessage") : TEXT("FirstMessage");
                Flow[C].Add(Entry);
                for (int32 I = 0; I < 3; ++I)
                {
                    auto* Node = Call(UKismetSystemLibrary::StaticClass(), TEXT("PrintString"), {float(900 - I * 250), float(C * 700 + I * 150)});
                    Link(Flow[C].Last()->FindPinChecked(UEdGraphSchema_K2::PN_Then), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
                    Flow[C].Add(Node);
                }
                auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>({1200, float(C * 700)});
                if (C == 0) { for (int32 I = 0; I < 10; ++I) { Sequence->AddInputPin(); } }
                Link(Flow[C].Last()->FindPinChecked(UEdGraphSchema_K2::PN_Then), Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
                Flow[C].Add(Sequence);
            }
            Input = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralString"), {-500, 1200});
            Input->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("shared message");
            Shared = Call(UKismetStringLibrary::StaticClass(), TEXT("ToUpper"), {-200, 1200});
            Dependencies = {Input, Shared};
            if (bGroup)
            {
                auto* Second = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralString"), {-500, 1450});
                Second->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT(" together");
                auto* Append = Call(UKismetStringLibrary::StaticClass(), TEXT("Concat_StrStr"), {-350, 1400});
                Link(Input->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Append->FindPinChecked(TEXT("A")));
                Link(Second->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Append->FindPinChecked(TEXT("B")));
                Link(Append->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Shared->FindPinChecked(TEXT("SourceString")));
                Dependencies.Add(Second); Dependencies.Add(Append);
            }
            else { Link(Input->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Shared->FindPinChecked(TEXT("SourceString"))); }
            for (const auto& Chain : Flow) { Link(Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Chain[3]->FindPinChecked(TEXT("InString"))); }
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            if (!bLinksValid) { return true; }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Compile);
            if (!Test.TestEqual(TEXT("Shared-message packing Blueprint compiles"), Compile.NumErrors, 0)) { return true; }
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint pure input gap packing")))
                .ClientSize(FVector2f(1550, 1050))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetViewLocation(FVector2f(-100, -100), 0.65f);
            Editor->SetNodeSelection(Flow[0][0], true);
            Deadline = FPlatformTime::Seconds() + 35;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Pure input packing fixture did not settle in 35 seconds.")); return true; }
        if (++Frames < 12) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (!bFormatted)
        {
            if (!bRequested)
            {
                Before = SerializeNodes(*Fixture->Graph);
                BeforeValues = SerializeTransactionValues(*Fixture->Graph);
                Properties = DescribeNodes(*Fixture->Graph);
                if (!Test.TestTrue(TEXT("Native pure input packing plan computes"), PlanFormatGraph(Fixture->Graph, Scale, {Flow[0][0]->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return true; }
                Test.TestTrue(TEXT("Packing plan is read-only"), Before == SerializeNodes(*Fixture->Graph));
                Test.TestEqual(TEXT("Packing keeps all original links"), Plan.Routes.Wires.Num(), bGroup ? 13 : 11);
                Test.TestEqual(TEXT("Packing needs no native fallback"), Plan.Routes.FallbackCount, 0);
                Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
                Editor->GetViewLocation(View, Zoom);
                Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
                Test.TestTrue(TEXT("Actual F packs pure inputs"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
                bRequested = true; return false;
            }
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            const auto AfterValues = SerializeTransactionValues(*Fixture->Graph);
            Test.TestTrue(TEXT("Packing changes the scattered native layout"), After != Before);
            Test.TestEqual(TEXT("Packing uses one native transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            const auto AfterProperties = DescribeNodes(*Fixture->Graph);
            Test.TestEqual(TEXT("Packing preserves property count"), Properties.Num(), AfterProperties.Num());
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const auto* Value = AfterProperties.Find(Pair.Key);
                Test.TestTrue(Pair.Key + TEXT(" survives packing"), Value && *Value == Pair.Value);
            }
            Test.TestTrue(TEXT("Packing preserves selected entry and exact position"),
                Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Flow[0][0]) &&
                Flow[0][0]->NodePosX == 0 && Flow[0][0]->NodePosY == 0);
            Test.TestTrue(TEXT("Pure input packing undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores every serialized value"), BeforeValues == SerializeTransactionValues(*Fixture->Graph));
            Test.TestTrue(TEXT("Pure input packing redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores every serialized value"), AfterValues == SerializeTransactionValues(*Fixture->Graph));
            bFormatted = true; Frames = 0; return false;
        }
        if (bNoOpRequested)
        {
            Test.TestTrue(TEXT("Repeated F leaves native input graph unchanged"), After == SerializeNodes(*Fixture->Graph));
            Test.TestEqual(TEXT("Repeated F creates no transaction"), GEditor->Trans->GetQueueLength(), Queue);
            return true;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        FFormatPlan Cold;
        if (Test.TestTrue(TEXT("Packed inputs replan with cold measurements"), PlanFormatGraph(Fixture->Graph, Scale, {Flow[0][0]->NodeGuid}, Cold, Reason)))
        {
            Test.TestTrue(TEXT("Pure input packing is cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key); const auto* Fresh = Cold.Routes.Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Every original wire has matching live and cold routes"), Live && Fresh &&
                    Live->Points == Pair.Value.Points && Fresh->Points == Pair.Value.Points &&
                    Live->Fallback == ERouteFallback::None && Fresh->Fallback == ERouteFallback::None);
            }
        }
        else { Test.AddError(Reason); }
        const auto Index = [this](UEdGraphNode* Node) { return Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; }); };
        const auto Bounds = [&](UEdGraphNode* Node)
        {
            const auto& Geometry = Plan.Snapshot.Nodes[Index(Node)].Geometry;
            return FBox2f(FVector2f(Node->NodePosX, Node->NodePosY) + Geometry.VisualBounds.Min,
                FVector2f(Node->NodePosX, Node->NodePosY) + Geometry.VisualBounds.Max);
        };
        for (UEdGraphNode* Node : Fixture->Graph->Nodes)
        {
            Test.TestEqual(TEXT("Actual F applies its preflight placement"), FIntPoint(Node->NodePosX, Node->NodePosY), Plan.Layout.Positions[Index(Node)]);
        }
        FBox2f All(ForceInit), FlowBounds(ForceInit);
        for (const auto& Chain : Flow)
        {
            for (int32 I = 0; I < Chain.Num(); ++I)
            {
                const auto B = Bounds(Chain[I]); FlowBounds += B.Min; FlowBounds += B.Max;
                if (I == 0) { continue; }
                const int32 From = Index(Chain[I - 1]), To = Index(Chain[I]);
                const auto* Out = Plan.Snapshot.Pins.FindByPredicate([From](const auto& P) { return P.Node == From && P.Kind == ELinkKind::Execution && P.bOutput; });
                const auto* In = Plan.Snapshot.Pins.FindByPredicate([To](const auto& P) { return P.Node == To && P.Kind == ELinkKind::Execution && !P.bOutput; });
                if (Test.TestTrue(TEXT("Flow has measured native pins"), Out && In && Out->Offset.IsSet() && In->Offset.IsSet()))
                {
                    Test.TestTrue(TEXT("Pure packing retains straight main execution attachments"),
                        FMath::Abs(Chain[I - 1]->NodePosY + Out->Offset.GetValue().Y - Chain[I]->NodePosY - In->Offset.GetValue().Y) <= 0.5f);
                }
            }
        }
        All += FlowBounds.Min; All += FlowBounds.Max;
        for (auto* Pure : Dependencies)
        {
            const auto B = Bounds(Pure); All += B.Min; All += B.Max;
            if (bGroup)
            {
                Test.TestTrue(TEXT("Every connected expression member fits between the execution rows"),
                    B.Min.Y >= Bounds(Flow[0][1]).Max.Y + 47.5f && B.Max.Y + 47.5f <= Bounds(Flow[1][0]).Min.Y);
                Test.AddInfo(FString::Printf(TEXT("Dependency %s: top %.2f, bottom %.2f."), *Pure->GetNodeTitle(ENodeTitleType::ListView).ToString(), B.Min.Y, B.Max.Y));
            }
            Test.TestTrue(TEXT("Pure dependencies fit inside the existing execution height"), B.Min.Y >= FlowBounds.Min.Y && B.Max.Y <= FlowBounds.Max.Y);
        }
        const auto SharedBounds = Bounds(Shared);
        Test.TestTrue(TEXT("Shared expression uses the clear gap before the second execution row"),
            SharedBounds.Min.Y >= Bounds(Flow[0][2]).Max.Y + 47.5f && SharedBounds.Max.Y + 47.5f <= Bounds(Flow[1][2]).Min.Y);
        for (const auto& Pair : Plan.Routes.Wires) { All += Pair.Value.Bounds.Min; All += Pair.Value.Bounds.Max; }
        Test.TestEqual(TEXT("Shared producer still has its two original consumers"), Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue)->LinkedTo.Num(), 2);
        Test.TestEqual(TEXT("Native node count is unchanged"), Fixture->Graph->Nodes.Num(), bGroup ? 14 : 12);
        Queue = GEditor->Trans->GetQueueLength();
        Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
        Test.TestTrue(TEXT("Packing preserves camera and zoom"), View == AfterView && Zoom == AfterZoom);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture native pure input packing"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            const TCHAR* Name = bGroup
                ? (Style == EGlooPrintWireStyle::Rounded90 ? TEXT("GlooPrint-DependencyPacking-Rounded.png") : TEXT("GlooPrint-DependencyPacking-Diagonal.png"))
                : (Style == EGlooPrintWireStyle::Rounded90 ? TEXT("GlooPrint-PurePacking-Rounded.png") : TEXT("GlooPrint-PurePacking-Diagonal.png"));
            Test.TestTrue(TEXT("Save native input packing capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / Name)));
        }
        Test.AddInfo(FString::Printf(TEXT("Pure input visual/wire envelope %.0f x %.0f; shared Y%d; %d repairs, %d fallbacks."),
            All.Max.X - All.Min.X, All.Max.Y - All.Min.Y, Shared->NodePosY, Plan.SpacingRepairs, Plan.Routes.FallbackCount));
        bNoOpRequested = true; Frames = 0; return false;
    }
private:
    UK2Node_CallFunction* Call(UClass* Library, FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(Library->FindFunctionByName(Name)); Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* From, UEdGraphPin* To)
    {
        bLinksValid &= Test.TestTrue(TEXT("Native schema accepts pure packing fixture link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To));
    }
    FAutomationTestBase& Test;
    EGlooPrintWireStyle Style, OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FLayoutSettings OriginalSettings;
    TUniquePtr<FFixture> Fixture;
    TArray<UEdGraphNode*> Flow[2];
    TArray<UK2Node_CallFunction*> Dependencies;
    bool bGroup = false;
    UK2Node_CallFunction* Input = nullptr;
    UK2Node_CallFunction* Shared = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FFormatPlan Plan;
    TArray<uint8> Before, BeforeValues, After;
    TMap<FString, FString> Properties;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0;
    int32 Frames = 0, Queue = 0;
    bool bRequested = false, bNoOpRequested = false;
    bool bRestore = false, bOriginalEnabled = true, bLinksValid = true, bFormatted = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPurePackingRoundedTest, "GlooPrint.Editor.PureInputGapPacking.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPurePackingRoundedTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FPurePackingCheck(*this, EGlooPrintWireStyle::Rounded90)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPurePackingDiagonalTest, "GlooPrint.Editor.PureInputGapPacking.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPurePackingDiagonalTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FPurePackingCheck(*this, EGlooPrintWireStyle::Diagonal45)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDependencyPackingRoundedTest, "GlooPrint.Editor.DependencyGroupPacking.Rounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDependencyPackingRoundedTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FPurePackingCheck(*this, EGlooPrintWireStyle::Rounded90, true)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDependencyPackingDiagonalTest, "GlooPrint.Editor.DependencyGroupPacking.Diagonal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDependencyPackingDiagonalTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FPurePackingCheck(*this, EGlooPrintWireStyle::Diagonal45, true)); return true;
}

}
#endif
