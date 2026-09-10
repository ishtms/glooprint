// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Algo/Reverse.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "ImageUtils.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
namespace
{
constexpr int32 Sources[6][3] = {{3, 5, 4}, {1, 0, 4}, {3, 5, 0}, {3, 1, 0}, {4, 1, 2}, {3, 2, 1}};

uint64 PairwiseCrossings(const FLayoutGraph& Graph, const FLayoutResult& Layout)
{
    uint64 Count = 0;
    for (int32 A = 0; A < Graph.Edges.Num(); ++A)
    {
        const auto& FromA = Graph.Pins[Graph.Edges[A].From]; const auto& ToA = Graph.Pins[Graph.Edges[A].To];
        for (int32 B = 0; B < A; ++B)
        {
            const auto& FromB = Graph.Pins[Graph.Edges[B].From]; const auto& ToB = Graph.Pins[Graph.Edges[B].To];
            const float From = Layout.Positions[FromA.Node].Y + FromA.Offset.GetValue().Y - Layout.Positions[FromB.Node].Y - FromB.Offset.GetValue().Y;
            const float To = Layout.Positions[ToA.Node].Y + ToA.Offset.GetValue().Y - Layout.Positions[ToB.Node].Y - ToB.Offset.GetValue().Y;
            Count += From * To < 0 ? 1 : 0;
        }
    }
    return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrossingLayoutTest, "GlooPrint.Layout.RetainsBestCrossingCandidate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrossingLayoutTest::RunTest(const FString& Parameters)
{
    FLayoutGraph Graph;
    for (int32 I = 0; I < 12; ++I)
    {
        const bool bSource = I < 6;
        auto& Node = Graph.Nodes.AddDefaulted_GetRef();
        Node.Geometry.Id = FGuid(0, 0, 0, I + 1);
        Node.Geometry.Position = {bSource ? 0 : 500, (I % 6) * 200};
        Node.Geometry.BodySize = {128, bSource ? 80.f : 144.f};
        Node.Geometry.VisualBounds = {FVector2f::ZeroVector, Node.Geometry.BodySize};
        Node.FirstPin = Graph.Pins.Num(); Node.PinCount = bSource ? 1 : 3;
        for (int32 P = 0; P < Node.PinCount; ++P)
        {
            Graph.Pins.Add({FGuid(0, 0, I + 1, P + 1), I, P, bSource, ELinkKind::Data, FVector2f(bSource ? 128 : 0, 40 + P * 32)});
        }
    }
    for (int32 I = 0; I < 6; ++I)
    {
        for (int32 P = 0; P < 3; ++P)
        {
            const int32 Source = Sources[I][P], Target = I + 6;
            const int32 Edge = Graph.Edges.Add({Graph.Nodes[Source].FirstPin, Graph.Nodes[Target].FirstPin + P, ELinkKind::Data});
            Graph.Nodes[Source].Outgoing.Add(Edge); Graph.Nodes[Target].Incoming.Add(Edge);
        }
    }
    Graph.Anchor = 0; FLayoutResult Result; FString Reason;
    if (!TestTrue(TEXT("Bipartite candidate graph lays out"), ComputeLayout(Graph, {}, Result, Reason))) { AddError(Reason); return false; }
    TestEqual(TEXT("Better packed order has 43 strict attachment crossings"), PairwiseCrossings(Graph, Result), uint64(43));
    TestEqual(TEXT("Indexed score agrees with direct pairwise geometry"), Result.OrderingCrossings, PairwiseCrossings(Graph, Result));
    TestTrue(TEXT("Four sweeps would regress to 44 crossings"), Result.LastCandidateCrossings.IsSet() && Result.LastCandidateCrossings.GetValue() == 44);
    TestEqual(TEXT("Retain the better second-sweep candidate"), Result.OrderingSweeps, 2);
    for (int32 I = 0; I < Graph.Nodes.Num(); ++I)
    {
        for (int32 J = 0; J < I; ++J)
        {
            const auto A = FVector2f(Result.Positions[I]), B = FVector2f(Result.Positions[J]);
            const auto SA = Graph.Nodes[I].Geometry.BodySize, SB = Graph.Nodes[J].Geometry.BodySize;
            TestFalse(TEXT("Candidate selection preserves body clearance"), A.X < B.X + SB.X && A.X + SA.X > B.X && A.Y < B.Y + SB.Y && A.Y + SA.Y > B.Y);
        }
        Graph.Nodes[I].Geometry.Position = Result.Positions[I];
    }
    FLayoutResult Cold;
    TestTrue(TEXT("Crossing candidate can be recomputed cold"), ComputeLayout(Graph, {}, Cold, Reason));
    TestTrue(TEXT("Candidate selection is cold-idempotent"), Cold.Positions == Result.Positions && Cold.OrderingCrossings == Result.OrderingCrossings);
    Algo::Reverse(Graph.Edges);
    for (auto& Node : Graph.Nodes) { Node.Incoming.Reset(); Node.Outgoing.Reset(); }
    for (int32 I = 0; I < Graph.Edges.Num(); ++I)
    {
        const auto& Edge = Graph.Edges[I];
        Graph.Nodes[Graph.Pins[Edge.From].Node].Outgoing.Add(I); Graph.Nodes[Graph.Pins[Edge.To].Node].Incoming.Add(I);
    }
    FLayoutResult Shuffled;
    TestTrue(TEXT("Shuffled candidate graph lays out"), ComputeLayout(Graph, {}, Shuffled, Reason));
    TestTrue(TEXT("Enumeration order does not change the chosen geometry or score"), Shuffled.Positions == Result.Positions && Shuffled.OrderingCrossings == Result.OrderingCrossings);
    return true;
}

class FCrossingCheck final : public IAutomationLatentCommand
{
public:
    explicit FCrossingCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FCrossingCheck() { Restore(); }
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
            for (int32 I = 0; I < 6; ++I)
            {
                auto* Node = Call(UKismetSystemLibrary::StaticClass(), TEXT("MakeLiteralDouble"), {0, float(I * 160)});
                Node->FindPinChecked(TEXT("Value"))->DefaultValue = LexToString(I + 1);
                Inputs.Add(Node);
            }
            const FName Names[] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
            for (int32 I = 0; I < 6; ++I)
            {
                auto* Node = Call(UKismetMathLibrary::StaticClass(), TEXT("MakeVector"), {500, float(I * 200)});
                Node->NodeComment = FString::Printf(TEXT("Vector %d"), I); Node->bCommentBubbleVisible = true; Node->bCommentBubblePinned = true;
                for (int32 P = 0; P < 3; ++P)
                {
                    bConnectionsValid &= Test.TestTrue(TEXT("Native double producers connect directly to original vector inputs"),
                        GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Inputs[Sources[I][P]]->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Node->FindPinChecked(Names[P])));
                }
            }
            if (!bConnectionsValid) { return Finish(); }
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Native crossing graph compiles"), Compile.NumErrors, 0)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint crossing candidate retention"))).ClientSize(FVector2f(1450, 1050))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef()); Editor->SetNodeSelection(Inputs[0], true);
            Editor->SetViewLocation(FVector2f(-150, -150), 0.75f); Slate.SetCursorPos(FVector2D::ZeroVector);
            Deadline = FPlatformTime::Seconds() + 45; return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native crossing fixture did not settle in 45 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel(); const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale(); FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph); Properties = DescribeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Native crossing graph has a valid layout/routing plan"), PlanFormatGraph(Fixture->Graph, Scale, {Inputs[0]->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Candidate trials are read-only"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Native geometry retains a better candidate than four sweeps"), Plan.Layout.LastCandidateCrossings.IsSet() && Plan.Layout.OrderingCrossings < Plan.Layout.LastCandidateCrossings.GetValue());
            Test.TestEqual(TEXT("Native ordering score agrees with actual pin pairs"), Plan.Layout.OrderingCrossings, PairwiseCrossings(Plan.Snapshot, Plan.Layout));
            Test.TestEqual(TEXT("Original native node count is preserved"), Plan.Snapshot.Nodes.Num(), 12);
            Test.TestEqual(TEXT("All eighteen native links have planned paths"), Plan.Routes.Wires.Num(), 18);
            Test.TestEqual(TEXT("Native crossing fixture has no routing fallback"), Plan.Routes.FallbackCount, 0);
            Test.AddInfo(FString::Printf(TEXT("Native crossing order: %llu retained, %llu after four sweeps, chosen%d; %d spacing repairs, %d fallbacks."),
                Plan.Layout.OrderingCrossings, Plan.Layout.LastCandidateCrossings.Get(0), Plan.Layout.OrderingSweeps, Plan.SpacingRepairs, Plan.Routes.FallbackCount));
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount(); Editor->GetViewLocation(View, Zoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F applies the chosen crossing candidate"), Slate.ProcessKeyDownEvent(KeyEvent()));
            Phase = 4; return false;
        }
        if (Phase == 4)
        {
            if (Before == SerializeNodes(*Fixture->Graph)) { return false; }
            After = SerializeNodes(*Fixture->Graph); Test.TestTrue(TEXT("F changes the original bipartite layout"), Before != After);
            Test.TestEqual(TEXT("All candidate trials produce one undo step"), GEditor->Trans->GetQueueLength(), Queue + 1);
            const auto Actual = DescribeNodes(*Fixture->Graph);
            for (const auto& Pair : Properties)
            {
                if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
                const auto* Value = Actual.Find(Pair.Key); Test.TestTrue(Pair.Key + TEXT(" preserved by candidate selection"), Value && *Value == Pair.Value);
            }
            Test.TestTrue(TEXT("Candidate layout undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Candidate undo restores all serialized data"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Candidate layout redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Candidate redo restores all serialized data"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>(); if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Native candidate selection replans cold"), PlanFormatGraph(Fixture->Graph, Scale, {Inputs[0]->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Native cold candidate keeps positions and score"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.OrderingCrossings == Plan.Layout.OrderingCrossings);
                for (const auto& Pair : Plan.Routes.Wires)
                {
                    const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key); const auto* Again = Cold.Routes.Wires.Find(Pair.Key);
                    Test.TestTrue(TEXT("Live and cold paths retain the original planned connection"), Live && Again && Live->Points == Pair.Value.Points && Again->Points == Pair.Value.Points);
                }
            }
            Queue = GEditor->Trans->GetQueueLength(); Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly); Slate.ProcessKeyDownEvent(KeyEvent());
            Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            Test.TestTrue(TEXT("Repeated F with candidate selection is an exact no-op"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Repeated candidate selection adds no undo step"), GEditor->Trans->GetQueueLength(), Queue);
            FVector2f CurrentView; float CurrentZoom; Editor->GetViewLocation(CurrentView, CurrentZoom);
            Test.TestEqual(TEXT("Candidate trials preserve the camera"), CurrentView, View); Test.TestEqual(TEXT("Candidate trials preserve zoom"), CurrentZoom, Zoom);
            Test.TestEqual(TEXT("Chosen native source anchor stays fixed"), FIntPoint(Inputs[0]->NodePosX, Inputs[0]->NodePosY), FIntPoint::ZeroValue);
            Editor->ZoomToFit(false); CaptureAfter = FPlatformTime::Seconds() + 1; Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            if (FPlatformTime::Seconds() < CaptureAfter) { return false; }
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture native crossing candidate result"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save native crossing capture"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-CrossingCandidates.png"))));
            }
            float Longest = 0; FVector2f Point;
            for (const auto& Pair : Cache->GetRoutes().Wires)
            {
                for (int32 I = 1; I + 1 < Pair.Value.Curves.Num(); ++I)
                {
                    const auto& Curve = Pair.Value.Curves[I];
                    if (FMath::Abs(Curve.End.X - Curve.Start.X) < 0.1f && FMath::Abs(Curve.End.Y - Curve.Start.Y) > Longest)
                    {
                        Longest = FMath::Abs(Curve.End.Y - Curve.Start.Y); HoverKey = Pair.Key; Point = Curve.Start + (Curve.End - Curve.Start) * 0.37f;
                    }
                }
            }
            if (!Test.TestTrue(TEXT("Crossing fixture has an interior data lane to hover"), Longest > 48)) { return Finish(); }
            Builds = Cache->GetBuildCount();
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, Panel->GetCachedGeometry().LocalToAbsolute((Point - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount()));
            Phase = 3; Frames = 0; return false;
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        const bool bHit = Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B);
        Test.TestTrue(TEXT("Visible crossing lane hover identifies its original source and vector input"), bHit && A && B &&
            ((A->PinId == HoverKey.FromPin && B->PinId == HoverKey.ToPin) || (B->PinId == HoverKey.FromPin && A->PinId == HoverKey.ToPin)));
        Test.TestEqual(TEXT("Native hover/zoom reuses chosen routes"), Cache->GetBuildCount(), Builds);
        return Finish();
    }
private:
    static FKeyEvent KeyEvent() { return FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0); }
    UK2Node_CallFunction* Call(UClass* Library, FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(Library->FindFunctionByName(Name)); Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
        Settings->VerticalSpacing = OriginalSettings.VerticalSpacing; Settings->CommentPadding = OriginalSettings.CommentPadding;
        Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged(); FSlateApplication::Get().SetCursorPos(OriginalCursor);
    }
    bool Finish() { if (Window) { Window->RequestDestroyWindow(); } Editor.Reset(); Window.Reset(); Restore(); return true; }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TArray<UK2Node_CallFunction*> Inputs;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    FRouteKey HoverKey;
    TArray<uint8> Before, After;
    TMap<FString, FString> Properties;
    FVector2D OriginalCursor;
    FVector2f View;
    float Zoom = 0;
    double Deadline = 0, CaptureAfter = 0;
    int32 Phase = 0, Frames = 0, Builds = 0, Queue = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrossingEditorTest, "GlooPrint.Editor.CrossingCandidateRetention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrossingEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FCrossingCheck(*this)); return true;
}
}
#endif
