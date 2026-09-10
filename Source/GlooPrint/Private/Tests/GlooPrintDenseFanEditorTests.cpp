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
#include "Input/HittestGrid.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FDenseFanCheck final : public IAutomationLatentCommand
{
public:
    explicit FDenseFanCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FDenseFanCheck() { Restore(); }
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
            Entry = Fixture->Add<UK2Node_CustomEvent>({0, 0}); Entry->CustomFunctionName = TEXT("DenseFanExample");
            auto* Sequence = Fixture->Add<UK2Node_ExecutionSequence>({600, 0});
            for (int32 I = 2; I < 12; ++I) { Sequence->AddInputPin(); }
            Shared = Call(TEXT("MakeLiteralString"), {800, 700});
            Shared->FindPinChecked(TEXT("Value"))->DefaultValue = TEXT("One value for twelve consumers");
            Join = Call(TEXT("PrintString"), {100, 700});
            Join->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("Shared execution join");
            Link(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Sequence->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            for (int32 I = 0; I < 12; ++I)
            {
                auto* Consumer = Call(TEXT("PrintString"), {float(100 + (I % 3) * 300), float(200 + I * 80)});
                Consumers.Add(Consumer);
                Link(Sequence->GetThenPinGivenIndex(I), Consumer->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
                Link(Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue), Consumer->FindPinChecked(TEXT("InString")));
                Link(Consumer->FindPinChecked(UEdGraphSchema_K2::PN_Then), Join->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
            }
            if (!bConnectionsValid) { return Finish(); }
            for (int32 I = 0; I < Fixture->Graph->Nodes.Num(); ++I) { Fixture->Graph->Nodes[I]->NodeGuid = FGuid(0, 0, 0, I + 1); }
            FCompilerResultsLog Compile;
            FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint.Get(), EBlueprintCompileOptions::None, &Compile);
            if (!Test.TestEqual(TEXT("Dense fan and join compile as a native Actor Blueprint"), Compile.NumErrors, 0)) { return Finish(); }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint twelve-way fan and join")))
                .ClientSize(FVector2f(1450, 1100))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -100), 0.5f); Editor->SetNodeSelection(Entry, true);
            Slate.SetCursorPos(FVector2D::ZeroVector); Deadline = FPlatformTime::Seconds() + 40;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Dense fan fixture did not settle in 40 seconds.")); return Finish(); }
        if (++Frames < 10) { return false; }
        auto* Panel = Editor->GetGraphPanel();
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        FString Reason;
        if (Phase == 0)
        {
            Before = SerializeNodes(*Fixture->Graph);
            if (!Test.TestTrue(TEXT("Dense native fan has a complete format plan"),
                PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Dense planning is read-only"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestEqual(TEXT("Dense plan retains all thirty-seven connections"), Plan.Routes.Wires.Num(), 37);
            Test.TestEqual(TEXT("Dense native fan and join need no fallback wires"), Plan.Routes.FallbackCount, 0);
            Test.TestEqual(TEXT("Dense wire capacity is reserved before global spacing repair"), Plan.SpacingRepairs, 0);
            const auto Index = [this](UEdGraphNode* Node) { return Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; }); };
            const int32 First = Index(Consumers[0]), Second = Index(Consumers[1]);
            if (!Test.TestTrue(TEXT("Dense plan retains the first two branch nodes"), First != INDEX_NONE && Second != INDEX_NONE)) { return Finish(); }
            const float Padding = Plan.Layout.Positions[Second].Y + Plan.Snapshot.Nodes[Second].Geometry.VisualBounds.Min.Y -
                Plan.Layout.Positions[First].Y - Plan.Snapshot.Nodes[First].Geometry.VisualBounds.Max.Y;
            Test.TestTrue(TEXT("Native branch column uses normal spacing within the bounded repair range"), Padding >= 47.5f && Padding <= 96.5f);
            Test.TestTrue(TEXT("Wire capacity keeps configured vertical branch spacing"), Padding >= 47.5f && Padding <= 48.5f);
            Test.AddInfo(FString::Printf(TEXT("Native dense fan: first-to-second branch body padding %.1f."), Padding));
            Test.AddInfo(FString::Printf(TEXT("Native dense fan: %d spacing repairs, %d fallbacks."), Plan.SpacingRepairs, Plan.Routes.FallbackCount));
            FBox2f Envelope(ForceInit);
            for (int32 I = 0; I < Plan.Snapshot.Nodes.Num(); ++I)
            {
                const auto& Bounds = Plan.Snapshot.Nodes[I].Geometry.VisualBounds;
                Envelope += FVector2f(Plan.Layout.Positions[I]) + Bounds.Min;
                Envelope += FVector2f(Plan.Layout.Positions[I]) + Bounds.Max;
            }
            for (const auto& Pair : Plan.Routes.Wires) { Envelope += Pair.Value.Bounds.Min; Envelope += Pair.Value.Bounds.Max; }
            Test.AddInfo(FString::Printf(TEXT("Native dense visual/wire envelope %.0f x %.0f."), Envelope.Max.X - Envelope.Min.X, Envelope.Max.Y - Envelope.Min.Y));
            TArray<float> Turns;
            for (int32 I = 0; I < Consumers.Num(); ++I)
            {
                auto* Input = Consumers[I]->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
                auto* Output = Input->LinkedTo[0];
                const auto* Route = Plan.Routes.Wires.Find({Output->GetOwningNode()->NodeGuid, Output->PinId, Consumers[I]->NodeGuid, Input->PinId});
                if (!Route) { Test.AddError(TEXT("Missing original sequence branch route.")); continue; }
                if (I == Consumers.Num() - 1) { LastBranch = Route->Key; }
                for (int32 P = 1; P < Route->Points.Num(); ++P)
                {
                    if (Route->Points[P].X == Route->Points[P - 1].X && FMath::Abs(Route->Points[P].Y - Route->Points[P - 1].Y) > 24)
                    {
                        Turns.Add(Route->Points[P].X); break;
                    }
                }
            }
            Turns.Sort();
            float LargestTurnGap = 0;
            for (int32 I = 1; I < Turns.Num(); ++I) { LargestTurnGap = FMath::Max(LargestTurnGap, Turns[I] - Turns[I - 1]); }
            Test.TestTrue(TEXT("A stepped branch stays beside the execution bundle"), LargestTurnGap <= 2 * WireLaneSpacing);
            Test.AddInfo(FString::Printf(TEXT("Dense execution first-turn largest gap %.1f."), LargestTurnGap));
            FRouteSet Diagonal;
            if (Test.TestTrue(TEXT("Packed native branch routes support diagonal style"), ComputeLayoutRoutes(Plan.Snapshot, Plan.Layout, Diagonal, Reason, EGlooPrintWireStyle::Diagonal45)))
            {
                for (const auto& Pair : Plan.Routes.Wires)
                {
                    const auto* Wire = Diagonal.Wires.Find(Pair.Key);
                    Test.TestTrue(TEXT("Both styles keep the same packed lanes and original pin pairs"), Wire && Wire->Points == Pair.Value.Points && Wire->Fallback == Pair.Value.Fallback);
                }
                for (const FRouteSet* Routes : {&Plan.Routes, &Diagonal})
                {
                    const auto* Last = Routes->Wires.Find(LastBranch);
                    if (!Test.TestTrue(TEXT("Last branch has its two compact turning sections"), Last && Last->Points.Num() == 6)) { continue; }
                    Test.AddInfo(FString::Printf(TEXT("Dense last branch turns %.2f -> %.2f."), Last->Points[1].X, Last->Points[3].X));
                    float Length = 0;
                    for (int32 I = 1; I < Last->Points.Num(); ++I) { Length += (Last->Points[I] - Last->Points[I - 1]).Size(); }
                    const FVector2f Span = Last->Points.Last() - Last->Points[0];
                    Test.TestEqual(TEXT("Compacting the stepped branch adds no backtracking or wire length"), Length, FMath::Abs(Span.X) + FMath::Abs(Span.Y));
                    int32 NativeInsetSamples = 0, InsetRoundoffSamples = 0;
                    for (int32 C = 0; C < Last->Curves.Num(); ++C)
                    {
                        const auto& Curve = Last->Curves[C];
                        for (int32 Sample = 1; Sample < 100; ++Sample)
                        {
                            const FVector2f P = FMath::CubicInterp(Curve.Start, Curve.StartTangent, Curve.End, Curve.EndTangent, Sample / 100.f);
                            for (int32 N = 0; N < Plan.Snapshot.Nodes.Num(); ++N)
                            {
                                const FVector2f Min(Plan.Layout.Positions[N]), Max = Min + Plan.Snapshot.Nodes[N].Geometry.BodySize;
                                const bool bInside = P.X > Min.X && P.X < Max.X && P.Y > Min.Y && P.Y < Max.Y;
                                const FGuid Id = Plan.Snapshot.Nodes[N].Geometry.Id;
                                const bool bHorizontal = Curve.Start.Y == Curve.End.Y && Curve.StartTangent.Y == 0 &&
                                    Curve.EndTangent.Y == 0 && Curve.Start.X <= Curve.End.X;
                                const bool bNativeTerminal = bHorizontal &&
                                    ((C == 0 && Id == LastBranch.FromNode && Curve.Start == Last->Points[0]) ||
                                     (C == Last->Curves.Num() - 1 && Id == LastBranch.ToNode && Curve.End == Last->Points.Last()));
                                if (bInside && bNativeTerminal)
                                {
                                    ++NativeInsetSamples;
                                    if (P.Y != Curve.Start.Y) { ++InsetRoundoffSamples; }
                                }
                                Test.TestFalse(TEXT("Compacted branch clears native bodies outside its own pin insets"), bInside && !bNativeTerminal);
                            }
                        }
                    }
                    Test.AddInfo(FString::Printf(TEXT("Native pin inset samples: %d (%d with interpolation roundoff)."), NativeInsetSamples, InsetRoundoffSamples));
                }
            }
            else { Test.AddError(Reason); }

            FormatQueue = GEditor->Trans->GetQueueLength();
            Editor->GetViewLocation(FormatView, FormatZoom);
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Test.TestTrue(TEXT("Actual F formats the dense fan"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
            Phase = 10; Frames = 0; return false;
        }
        if (Phase == 10)
        {
            if (GEditor->Trans->GetQueueLength() == FormatQueue) { return false; }
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("F moves the scattered fan"), Before != After);
            Test.TestEqual(TEXT("Dense fan formatting is one undo step"), GEditor->Trans->GetQueueLength(), FormatQueue + 1);
            FVector2f AfterView; float AfterZoom; Editor->GetViewLocation(AfterView, AfterZoom);
            Test.TestEqual(TEXT("Dense F preserves camera"), AfterView, FormatView); Test.TestEqual(TEXT("Dense F preserves zoom"), AfterZoom, FormatZoom);
            Test.TestTrue(TEXT("Dense fan undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Dense fan undo restores every serialized value"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Dense fan redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Dense fan redo restores every serialized value"), SerializeNodes(*Fixture->Graph) == After);
            Phase = 1; Frames = 0; return false;
        }
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        if (Phase == 1)
        {
            Test.TestEqual(TEXT("Dense graph retains all sixteen node identities"), Fixture->Graph->Nodes.Num(), 16);
            Test.TestEqual(TEXT("Same data output still feeds twelve consumers"), Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue)->LinkedTo.Num(), 12);
            Test.TestEqual(TEXT("Same execution input still joins twelve branches"), Join->FindPinChecked(UEdGraphSchema_K2::PN_Execute)->LinkedTo.Num(), 12);
            for (const auto& Pair : Plan.Routes.Wires)
            {
                const auto* Live = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Dense live path agrees with preflight"), Live && Live->Points == Pair.Value.Points && Live->Fallback == Pair.Value.Fallback);
            }
            FFormatPlan Cold;
            if (Test.TestTrue(TEXT("Dense fan replans with cold geometry"), PlanFormatGraph(Fixture->Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.TestTrue(TEXT("Dense fan is cold-idempotent"), Cold.Layout.Positions == Plan.Layout.Positions);
            }
            const int32 Queue = GEditor->Trans->GetQueueLength();
            Slate.SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
            Test.TestTrue(TEXT("Dense repeated F changes no serialized values"), SerializeNodes(*Fixture->Graph) == After);
            Test.TestEqual(TEXT("Dense repeated F adds no undo step"), GEditor->Trans->GetQueueLength(), Queue);
            Editor->SetViewLocation(FVector2f(-100, -100), 0.25f);
            CaptureAfter = FPlatformTime::Seconds() + 1.0; Phase = 2; return false;
        }
        if (FPlatformTime::Seconds() < CaptureAfter) { return false; }
        if (Phase == 2)
        {
            Test.TestEqual(TEXT("Dense attachment checks run at quarter zoom"), Panel->GetZoomAmount(), 0.25f);
            TMap<FGuid, FVector2f> NativePins;
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const auto Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
                if (!Test.TestTrue(TEXT("Dense visible node has native geometry"), Widget.IsValid())) { return Finish(); }
                TArray<TSharedRef<SWidget>> NodePins; Widget->GetPins(NodePins);
                for (const auto& PinWidget : NodePins)
                {
                    const auto* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj();
                    if (Pin->LinkedTo.IsEmpty()) { continue; }
                    const auto& PinGeometry = PinWidget->GetCachedGeometry();
                    const FVector2f PinSize(PinGeometry.GetLocalSize());
                    const FVector2f Screen = PinGeometry.LocalToAbsolute(FVector2f(Pin->Direction == EGPD_Output ? PinSize.X : 0, PinSize.Y * 0.5f));
                    NativePins.Add(Pin->PinId, FVector2f(Panel->GetViewOffset()) + Panel->GetCachedGeometry().AbsoluteToLocal(Screen) / Panel->GetZoomAmount());
                }
            }
            for (const auto& Pair : Cache->GetRoutes().Wires)
            {
                for (bool bStart : {true, false})
                {
                    const auto* Point = NativePins.Find(bStart ? Pair.Key.FromPin : Pair.Key.ToPin);
                    const auto& Region = bStart ? Pair.Value.StartRegion : Pair.Value.EndRegion;
                    if (!Test.TestTrue(TEXT("Every low-detail attachment fits its prevalidated custom terminal"), Point && Region.bIsValid &&
                        Point->X >= Region.Min.X && Point->X <= Region.Max.X && Point->Y >= Region.Min.Y && Point->Y <= Region.Max.Y))
                    {
                        const FGuid NodeId = bStart ? Pair.Key.FromNode : Pair.Key.ToNode;
                        const auto* Node = Fixture->Graph->Nodes.FindByPredicate([NodeId](const auto& N) { return N->NodeGuid == NodeId; });
                        Test.AddInfo(FString::Printf(TEXT("Dense %s terminal %s: native(%.6f,%.6f), region(%.6f,%.6f)-(%.6f,%.6f)."),
                            bStart ? TEXT("start") : TEXT("end"), Node ? *(*Node)->GetName() : TEXT("missing"), Point ? Point->X : 0, Point ? Point->Y : 0,
                            Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y));
                    }
                }
            }
            const auto Native = Panel->GetNodeWidgetFromGuid(Shared->NodeGuid);
            if (!Test.TestTrue(TEXT("Dense shared node has a native widget at low detail"), Native.IsValid())) { return Finish(); }
            TArray<TSharedRef<SWidget>> Pins; Native->GetPins(Pins);
            auto* Output = Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue);
            const auto* PinWidget = Pins.FindByPredicate([Output](const auto& Pin) { return StaticCastSharedRef<SGraphPin>(Pin)->GetPinObj() == Output; });
            if (!Test.TestTrue(TEXT("Shared native output has arranged geometry"), PinWidget != nullptr)) { return Finish(); }
            const auto& Geometry = (*PinWidget)->GetCachedGeometry();
            const FVector2f Size(Geometry.GetLocalSize());
            const FVector2f NativePoint = Geometry.LocalToAbsolute(FVector2f(Size.X, Size.Y * 0.5f));
            const auto* Input = Output->LinkedTo[0];
            const auto* Route = Cache->GetRoutes().Wires.Find({Shared->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId});
            if (!Test.TestTrue(TEXT("Shared low-detail hover has a complete custom path"), Route && Route->Points.Num() >= 2)) { return Finish(); }
            const FVector2f CachedPoint = Panel->GetCachedGeometry().LocalToAbsolute((Route->Points[0] - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            Test.AddInfo(FString::Printf(TEXT("Dense low-detail shared attachment: native(%.3f,%.3f), cached(%.3f,%.3f), zoom%.3f."),
                NativePoint.X, NativePoint.Y, CachedPoint.X, CachedPoint.Y, Panel->GetZoomAmount()));
            Test.TestTrue(TEXT("Low-detail fixture exercises a shrunken inline-value node"), CachedPoint.X - NativePoint.X > 20);
            BuildsBeforeHover = Cache->GetBuildCount();
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, (NativePoint + CachedPoint) * 0.5f);
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            const auto* Route = Cache->GetRoutes().Wires.Find(LastBranch);
            if (!Test.TestTrue(TEXT("Compacted execution branch is available for native hover"), Route && Route->Points.Num() == 6)) { return Finish(); }
            const FVector2f Middle = (Route->Points[1] + Route->Points[2]) * 0.5f;
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel,
                Panel->GetCachedGeometry().LocalToAbsolute((Middle - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount()));
            Phase = 7; Frames = 0; return false;
        }
        UEdGraphPin* HoveredA = nullptr; UEdGraphPin* HoveredB = nullptr;
        auto* Output = Shared->FindPinChecked(UEdGraphSchema_K2::PN_ReturnValue);
        const bool bHovered = Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, HoveredA, HoveredB);
        if (Phase == 3)
        {
            Test.TestTrue(TEXT("Low-detail wire visibly joins the native shared pin"), bHovered &&
                ((HoveredA == Output && Output->LinkedTo.Contains(HoveredB)) || (HoveredB == Output && Output->LinkedTo.Contains(HoveredA))));
            float Longest = 0; FVector2f Middle;
            for (const auto& Pair : Cache->GetRoutes().Wires)
            {
                if (Pair.Key.FromPin != Output->PinId) { continue; }
                const auto& Points = Pair.Value.Points;
                for (int32 I = 1; I < Points.Num(); ++I)
                {
                    const float Length = FMath::Abs(Points[I].Y - Points[I - 1].Y);
                    if (Points[I].X == Points[I - 1].X && Length > Longest)
                    {
                        Longest = Length; Middle = (Points[I] + Points[I - 1]) * 0.5f; HoverTarget = Pair.Key.ToPin;
                    }
                }
            }
            if (!Test.TestTrue(TEXT("Dense fixture contains a long custom data lane"), Longest > 200)) { return Finish(); }
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel,
                Panel->GetCachedGeometry().LocalToAbsolute((Middle - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount()));
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            Test.TestTrue(TEXT("Low-detail interior custom lane identifies its exact original pin pair"), bHovered &&
                ((HoveredA == Output && HoveredB && HoveredB->PinId == HoverTarget) || (HoveredB == Output && HoveredA && HoveredA->PinId == HoverTarget)));
        }
        if (Phase == 7)
        {
            auto* Input = Consumers.Last()->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            auto* From = Input->LinkedTo[0];
            Test.TestTrue(TEXT("Moved execution lane hover identifies the last branch's original pins"), bHovered &&
                ((HoveredA == From && HoveredB == Input) || (HoveredB == From && HoveredA == Input)));
        }
        Test.TestEqual(TEXT("Low-detail attachment does not rebuild or change the cached corridor"), Cache->GetBuildCount(), BuildsBeforeHover);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture dense native fan"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save dense native fan"), FFileHelper::SaveArrayToFile(Png,
                *(FPaths::ProjectSavedDir() / (Phase == 4 ? TEXT("GlooPrint-DenseFan.png") : Phase == 7 ? TEXT("GlooPrint-DenseBranch.png") : TEXT("GlooPrint-DenseFanCloseup.png")))));
        }
        if (Phase == 4)
        {
            Editor->SetViewLocation(FVector2f(Shared->NodePosX - 100, Shared->NodePosY - 250), 1.0f);
            Slate.SetCursorPos(FVector2D::ZeroVector); Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            const auto* Route = Cache->GetRoutes().Wires.Find(LastBranch);
            if (!Route || Route->Points.Num() < 2) { return Finish(); }
            Editor->SetViewLocation(Route->Points[0] - FVector2f(100, 180), 0.75f);
            const FVector2D PreviousCursor = Slate.GetCursorPos();
            Slate.SetCursorPos(FVector2D::ZeroVector);
            const FPointerEvent Leave(0, FVector2f::ZeroVector, PreviousCursor, TSet<FKey>(), EKeys::Invalid, 0, FModifierKeysState());
            const FWidgetPath Outside(Window->GetHittestGrid().GetBubblePath(FVector2f::ZeroVector, 0, false, 0));
            Test.TestFalse(TEXT("Branch hover starts outside the fixture graph"), Outside.ContainsWidget(Panel));
            Slate.RoutePointerMoveEvent(Outside, Leave, false);
            Phase = 6; Frames = 0; return false;
        }
        return Finish();
    }
private:
    UK2Node_CallFunction* Call(FName Name, FVector2f Position)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Fixture->Graph);
        Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Name));
        Fixture->Initialize(*Node, Position); Node->PostPlacedNewNode(); return Node;
    }
    void Link(UEdGraphPin* A, UEdGraphPin* B)
    {
        bConnectionsValid &= Test.TestTrue(TEXT("Native K2 schema accepts dense fixture link"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B));
    }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing; Settings->VerticalSpacing = OriginalSettings.VerticalSpacing;
        Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        Editor.Reset(); Window.Reset(); Restore(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_CustomEvent* Entry = nullptr;
    UK2Node_CallFunction* Shared = nullptr;
    UK2Node_CallFunction* Join = nullptr;
    TArray<UK2Node_CallFunction*> Consumers;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    FVector2D OriginalCursor;
    FVector2f FormatView;
    float FormatZoom = 0;
    int32 FormatQueue = 0;
    FGuid HoverTarget;
    FRouteKey LastBranch;
    double Deadline = 0, CaptureAfter = 0;
    int32 Phase = 0, Frames = 0, BuildsBeforeHover = 0;
    bool bOriginalEnabled = true, bRestore = false, bConnectionsValid = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDenseFanEditorTest, "GlooPrint.Editor.DenseFanAndJoin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDenseFanEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDenseFanCheck(*this)); return true;
}
}
#endif
