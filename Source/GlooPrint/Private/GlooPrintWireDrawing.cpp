// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintWireDrawing.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"

#include "BlueprintConnectionDrawingPolicy.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Layout/ArrangedChildren.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateRenderer.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Styling/AppStyle.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"

namespace GlooPrint
{
FRouteCache::~FRouteCache() { Shutdown(); }

void FRouteCache::Initialize(TSharedRef<SGraphPanel> InPanel)
{
    Panel = InPanel; Graph = InPanel->GetGraphObj();
    GraphHandle = Graph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateSP(this, &FRouteCache::OnGraphChanged));
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddSP(this, &FRouteCache::OnModified);
    PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &FRouteCache::OnPropertyChanged);
    TransactionHandle = FCoreUObjectDelegates::OnObjectTransacted.AddSP(this, &FRouteCache::OnTransacted);
    SlateHandle = FSlateApplication::Get().OnInvalidateAllWidgets().AddSP(this, &FRouteCache::OnSlateInvalidated);
    const auto Fonts = FSlateApplication::Get().GetRenderer()->GetFontCache();
    FontCache = Fonts;
    FontHandle = Fonts->OnReleaseResources().AddSP(this, &FRouteCache::OnFontsReleased);
    ObserveContext();
}

void FRouteCache::Shutdown()
{
    if (bStopped) { return; }
    bStopped = true; bReady = false; Routes = {}; Capture.Reset(); Routing.Reset(); Planned.Reset();
    if (const auto Owner = Panel.Pin())
    {
        if (const auto Cache = Measurements.Pin(); Cache && Owner->GetMetaData<FMeasurementCache>() == Cache)
        {
            Owner->RemoveMetaData(Cache.ToSharedRef());
        }
        Owner->Invalidate(EInvalidateWidgetReason::Paint);
    }
    Measurements.Reset();
    if (FSlateApplication::IsInitialized()) { FSlateApplication::Get().OnPostTick().Remove(RebuildHandle); }
    RebuildHandle.Reset();
    if (UEdGraph* LiveGraph = Graph.Get()) { LiveGraph->RemoveOnGraphChangedHandler(GraphHandle); }
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactionHandle);
    if (FSlateApplication::IsInitialized()) { FSlateApplication::Get().OnInvalidateAllWidgets().Remove(SlateHandle); }
    if (const auto Fonts = FontCache.Pin()) { Fonts->OnReleaseResources().Remove(FontHandle); }
}

void FRouteCache::ObserveContext()
{
    const auto Owner = Panel.Pin();
    if (bStopped || !Owner) { return; }
    const auto Window = FSlateApplication::Get().FindWidgetWindow(Owner.ToSharedRef());
    const float CurrentScale = Window ? Window->GetDPIScaleFactor() * FSlateApplication::Get().GetApplicationScale() : 0;
    const uint16 CurrentText = FTextLocalizationManager::Get().GetTextRevision();
    if (Scale != CurrentScale || PinVisibility != Owner->GetPinVisibility() ||
        TextRevision != CurrentText || StyleIdentity != &FAppStyle::Get() ||
        WireStyle != GetDefault<UGlooPrintSettings>()->GetWireStyle())
    {
        Scale = CurrentScale; PinVisibility = Owner->GetPinVisibility();
        TextRevision = CurrentText; StyleIdentity = &FAppStyle::Get();
        Invalidate();
    }
}

void FRouteCache::Invalidate(bool bContextChanged)
{
    if (bStopped) { return; }
    if (const auto Cache = Measurements.Pin()) { Cache->Invalidate(bContextChanged); }
    bReady = false; Routes = {}; Capture.Reset(); Routing.Reset(); Planned.Reset(); ++Revision; AttemptsLeft = 3;
    WireStyle = GetDefault<UGlooPrintSettings>()->GetWireStyle();
    if (WireStyle == EGlooPrintWireStyle::Native)
    {
        FSlateApplication::Get().OnPostTick().Remove(RebuildHandle); RebuildHandle.Reset();
    }
    else { Schedule(); }
    if (const auto Owner = Panel.Pin()) { Owner->Invalidate(EInvalidateWidgetReason::Paint); }
}

void FRouteCache::Schedule()
{
    if (bStopped || RebuildHandle.IsValid()) { return; }
    RebuildHandle = FSlateApplication::Get().OnPostTick().AddSP(this, &FRouteCache::OnPostTick);
}

void FRouteCache::StagePlannedRoutes(FLayoutGraph Source, FRouteSet PlannedRoutes, EGlooPrintWireStyle Style)
{
    check(IsInGameThread());
    ObserveContext();
    if (bStopped || bReady || !Graph.IsValid() || !Panel.IsValid() ||
        Style == EGlooPrintWireStyle::Native || Style != WireStyle) { return; }
    Planned.Emplace(FPlannedRoutes{MoveTemp(Source), MoveTemp(PlannedRoutes)});
    Capture.Reset(); Routing.Reset();
    Schedule();
}

static bool SameRoutingInputs(const FLayoutGraph& A, const FLayoutGraph& B)
{
    if (A.Nodes.Num() != B.Nodes.Num() || A.Pins.Num() != B.Pins.Num() || A.Edges.Num() != B.Edges.Num()) { return false; }
    for (int32 I = 0; I < A.Nodes.Num(); ++I)
    {
        const auto& X = A.Nodes[I]; const auto& Y = B.Nodes[I];
        if (X.Geometry.Id != Y.Geometry.Id || X.Geometry.Position != Y.Geometry.Position ||
            X.Geometry.BodySize != Y.Geometry.BodySize || X.Geometry.VisualBounds != Y.Geometry.VisualBounds ||
            X.Geometry.CommentHeader != Y.Geometry.CommentHeader || X.bComment != Y.bComment ||
            X.bReroute != Y.bReroute || X.FirstPin != Y.FirstPin || X.PinCount != Y.PinCount) { return false; }
    }
    for (int32 I = 0; I < A.Pins.Num(); ++I)
    {
        const auto& X = A.Pins[I]; const auto& Y = B.Pins[I];
        if (X.Id != Y.Id || X.Node != Y.Node || X.Ordinal != Y.Ordinal || X.bOutput != Y.bOutput ||
            X.Kind != Y.Kind || X.Offset != Y.Offset) { return false; }
    }
    for (int32 I = 0; I < A.Edges.Num(); ++I)
    {
        const auto& X = A.Edges[I]; const auto& Y = B.Edges[I];
        if (X.From != Y.From || X.To != Y.To || X.Kind != Y.Kind) { return false; }
    }
    return true;
}

void FRouteCache::OnPostTick(float DeltaTime)
{
    if (!Rebuild(DeltaTime))
    {
        FSlateApplication::Get().OnPostTick().Remove(RebuildHandle);
        RebuildHandle.Reset();
    }
}

bool FRouteCache::Rebuild(float DeltaTime)
{
    check(IsInGameThread());
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_RebuildRoutes);
    const auto Owner = Panel.Pin();
    if (bStopped || !Owner || !Graph.IsValid() || Owner->GetGraphObj() != Graph.Get())
    {
        Capture.Reset(); Routing.Reset(); Planned.Reset();
        return false;
    }
    if (!FSlateApplication::Get().GetPressedMouseButtons().IsEmpty()) { return true; }
    ObserveContext();
    if (WireStyle == EGlooPrintWireStyle::Native) { return false; }
    const double Started = FPlatformTime::Seconds();
    FString Reason;
    if (!Routing)
    {
        if (Capture)
        {
            const auto Cache = Measurements.Pin();
            if (!Cache || Owner->GetMetaData<FMeasurementCache>() != Cache || Cache->GetRevision() != MeasurementRevision) { Capture.Reset(); }
        }
        if (!Capture)
        {
            if (!ValidateMeasurementGraph(Graph.Get(), Reason)) { Planned.Reset(); Owner->Invalidate(EInvalidateWidgetReason::Paint); return false; }
            auto Cache = Owner->GetMetaData<FMeasurementCache>();
            if (!Cache)
            {
                Cache = MakeShared<FMeasurementCache>(); Owner->AddMetadata(Cache.ToSharedRef());
            }
            Measurements = Cache;
            Cache->Begin(Graph.Get(), Scale, PinVisibility); MeasurementRevision = Cache->GetRevision();
            FMeasurementOptions Options; Options.PinVisibility = PinVisibility; Options.Cache = Cache.Get();
            Capture = MakeUnique<FGraphCaptureJob>(Graph.Get(), Scale, TSet<FGuid>(), Options);
            RoutingRevision = Revision; ++BuildCount;
        }
        const uint64 RequestRevision = RoutingRevision;
        const uint64 RequestMeasurementRevision = MeasurementRevision;
        auto Job = MoveTemp(Capture);
        const bool bFinished = Job->Advance(Started + 0.008);
        if (bStopped) { return false; }
        if (Revision != RequestRevision) { return true; }
        const auto CurrentMeasurements = Measurements.Pin();
        if (!CurrentMeasurements || Owner->GetMetaData<FMeasurementCache>() != CurrentMeasurements ||
            CurrentMeasurements->GetRevision() != RequestMeasurementRevision) { return true; }
        if (!bFinished) { Capture = MoveTemp(Job); return true; }
        FLayoutGraph Snapshot;
        bool bRetry = false;
        if (!Job->TakeResult(Snapshot, Reason, &bRetry))
        {
            if (bRetry && AttemptsLeft-- > 0) { return true; }
            Planned.Reset();
            Owner->Invalidate(EInvalidateWidgetReason::Paint);
            return false;
        }
        if (Planned && SameRoutingInputs(Snapshot, Planned->Source))
        {
            Routes = MoveTemp(Planned->Routes); Planned.Reset(); bReady = true; ++ReusedPlanCount;
            Owner->Invalidate(EInvalidateWidgetReason::Paint);
            return false;
        }
        Planned.Reset();
        Routing = MakeUnique<FRoutingJob>(MoveTemp(Snapshot), WireStyle);
        RoutingRevision = RequestRevision;
    }
    auto Job = MoveTemp(Routing);
    const uint64 JobRevision = RoutingRevision;
    const bool bFinished = Job->Advance(Started + 0.004);
    if (Revision != JobRevision) { return true; }
    if (!bFinished) { Routing = MoveTemp(Job); return true; }
    FRouteSet Result;
    if (Job->TakeResult(Result, Reason)) { Routes = MoveTemp(Result); bReady = true; }
    Owner->Invalidate(EInvalidateWidgetReason::Paint);
    return false;
}

void FRouteCache::OnGraphChanged(const FEdGraphEditAction& Action) { Invalidate(false); }
void FRouteCache::OnModified(UObject* Object)
{
    UEdGraph* LiveGraph = Graph.Get();
    if (Object && LiveGraph && (Object == LiveGraph || Object->IsIn(LiveGraph) || LiveGraph->IsIn(Object)))
    {
        Invalidate(!Object->IsA<UEdGraphNode>());
    }
}
void FRouteCache::OnPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
    if (const auto Cache = Measurements.Pin()) { Cache->Invalidate(); }
    OnModified(Object);
}
void FRouteCache::OnTransacted(UObject* Object, const FTransactionObjectEvent& Event) { OnModified(Object); }
void FRouteCache::OnSlateInvalidated(bool bClearResources) { Invalidate(); }
void FRouteCache::OnFontsReleased(const FSlateFontCache& Fonts) { Invalidate(); }

namespace
{
class FRouteDrawingPolicy final : public FKismetConnectionDrawingPolicy
{
public:
    FRouteDrawingPolicy(int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip,
        FSlateWindowElementList& Elements, UEdGraph* Graph, TWeakPtr<const FWireDrawing> InFactory)
        : FKismetConnectionDrawingPolicy(BackLayer, FrontLayer, Zoom, Clip, Elements, Graph), Factory(InFactory) {}

    virtual void Draw(TMap<TSharedRef<SWidget>, FArrangedWidget>& Geometries, FArrangedChildren& Nodes) override
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_WirePaint);
        HitPieces.Reset();
        if (Nodes.Num() > 0)
        {
            const auto Node = StaticCastSharedRef<SGraphNode>(Nodes[0].Widget);
            Panel = Node->GetOwnerPanel();
            PaintOrigin = Nodes[0].Geometry.GetAbsolutePosition() - Node->GetPosition2f() * ZoomFactor;
            const auto Owner = Panel.Pin();
            if (const auto LiveFactory = Factory.Pin(); LiveFactory && Owner)
            {
                Cache = LiveFactory->GetCache(Owner.ToSharedRef());
                if (Cache) { Cache->ObserveContext(); }
            }
        }
        FKismetConnectionDrawingPolicy::Draw(Geometries, Nodes);
    }

    virtual bool IsConnectionCulled(const FArrangedWidget& Start, const FArrangedWidget& End) const override
    {
        return Cache && Cache->IsReady() ? false : FKismetConnectionDrawingPolicy::IsConnectionCulled(Start, End);
    }

    virtual void DrawSplineWithArrow(const FGeometry& Start, const FGeometry& End, const FConnectionParams& Params) override
    {
        const TGuardValue<bool> StartGuard(bSynthesizedStart, Start.GetLocalSize().IsZero());
        const TGuardValue<bool> EndGuard(bSynthesizedEnd, End.GetLocalSize().IsZero());
        FKismetConnectionDrawingPolicy::DrawSplineWithArrow(Start, End, Params);
    }

    virtual void DrawConnection(int32 Layer, const FVector2f& Start, const FVector2f& End, const FConnectionParams& Params) override
    {
        const auto Owner = Panel.Pin();
        const FWireRoute* Route = nullptr;
        if (Owner && Cache && Cache->IsReady() && Params.AssociatedPin1 && Params.AssociatedPin2 &&
            Params.StartDirection == EGPD_Output && Params.EndDirection == EGPD_Input)
        {
            const FRouteKey Key{Params.AssociatedPin1->GetOwningNode()->NodeGuid, Params.AssociatedPin1->PinId,
                Params.AssociatedPin2->GetOwningNode()->NodeGuid, Params.AssociatedPin2->PinId};
            Route = Cache->GetRoutes().Wires.Find(Key);
        }
        if (!Route || Route->Curves.IsEmpty())
        {
            AddHitPiece(Start, End, Params);
            FKismetConnectionDrawingPolicy::DrawConnection(Layer, Start, End, Params); return;
        }
        const float Scale = ZoomFactor;
        const auto Transform = [this](FVector2f P)
        {
            return PaintOrigin + P * ZoomFactor;
        };
        const FVector2f PinStart = bSynthesizedStart ? Route->Curves[0].Start : (Start + FVector2f(4, 0) - PaintOrigin) / Scale;
        const FVector2f PinEnd = bSynthesizedEnd ? Route->Curves.Last().End : (End - FVector2f(4, 0) - PaintOrigin) / Scale;
        const auto Contains = [](const FBox2f& Region, FVector2f Point)
        {
            return Region.bIsValid && Point.X >= Region.Min.X && Point.X <= Region.Max.X &&
                Point.Y >= Region.Min.Y && Point.Y <= Region.Max.Y;
        };
        const bool bSingle = Route->Curves.Num() == 1;
        if (!Contains(Route->StartRegion, PinStart) || !Contains(Route->EndRegion, PinEnd) || (bSingle && PinStart.X > PinEnd.X))
        {
            AddHitPiece(Start, End, Params);
            FKismetConnectionDrawingPolicy::DrawConnection(Layer, Start, End, Params); return;
        }
        FRouteCurve First = Route->Curves[0], Last = Route->Curves.Last();
        First.Start = PinStart;
        if (bSingle) { First.End = PinEnd; }
        First.StartTangent = First.EndTangent = FVector2f(First.End.X - First.Start.X, 0);
        Last.End = PinEnd;
        Last.StartTangent = Last.EndTangent = FVector2f(Last.End.X - Last.Start.X, 0);
        const FVector2f Min = Transform(Route->Bounds.Min), Max = Transform(Route->Bounds.Max);
        if (!FSlateRect::DoRectanglesIntersect(FSlateRect(Min.X, Min.Y, Max.X, Max.Y).ExtendBy(FMargin(Params.WireThickness + 8)), ClippingRect)) { return; }
        const float BeforeDistance = SplineOverlapResult.GetDistanceSquared();
        const int32 BeforeSlice = ConnectionsIntersectingSliceLine.Num();
        const FSlateBrush* SavedMidpoint = MidpointImage;
        MidpointImage = nullptr;
        for (int32 Index = 0; Index < Route->Curves.Num(); ++Index)
        {
            const auto& Curve = Index == 0 ? First : Index == Route->Curves.Num() - 1 ? Last : Route->Curves[Index];
            FConnectionParams Piece = Params;
            Piece.StartTangent = Curve.StartTangent * Scale; Piece.EndTangent = Curve.EndTangent * Scale;
            Piece.bDrawBubbles = false;
            FVector2f A = Transform(Curve.Start), B = Transform(Curve.End);
            if (Index == 0) { A.X -= 4; }
            if (Index == Route->Curves.Num() - 1) { B.X += 4; }
            if (Index == 0 || Index == Route->Curves.Num() - 1) { Piece.StartTangent = Piece.EndTangent = FVector2f(B.X - A.X, 0); }
            AddHitPiece(A, B, Piece);
            FKismetConnectionDrawingPolicy::DrawConnection(Layer, A, B, Piece);
        }
        MidpointImage = SavedMidpoint;
        CorrectPinDistances(SplineOverlapResult, BeforeDistance, Params, Transform(PinStart), Transform(PinEnd), AbsoluteMousePosition);
        if (ConnectionsIntersectingSliceLine.Num() > BeforeSlice + 1)
        {
            ConnectionsIntersectingSliceLine.SetNum(BeforeSlice + 1, EAllowShrinking::No);
        }
        float Length = Route->Length;
        if (Params.bDrawBubbles || MidpointImage)
        {
            MeasureRouteCurve(First);
            if (bSingle) { Length = First.Length; }
            else
            {
                MeasureRouteCurve(Last);
                Length += First.Length - Route->Curves[0].Length + Last.Length - Route->Curves.Last().Length;
            }
        }
        const auto Evaluate = [&](float Distance, FVector2f* Direction = nullptr)
        {
            Distance = FMath::Clamp(Distance, 0.f, Length);
            if (bSingle || Distance <= First.Length) { return EvaluateRouteCurve(First, Distance, Direction); }
            if (Distance >= Length - Last.Length) { return EvaluateRouteCurve(Last, Distance - (Length - Last.Length), Direction); }
            return EvaluateRoute(*Route, Distance - First.Length + Route->Curves[0].Length, Direction);
        };
        if (Params.bDrawBubbles)
        {
            const float Spacing = 64.f;
            const float Offset = FMath::Fmod(float(FPlatformTime::Seconds() - GStartTime) * 192.f, Spacing);
            const FVector2f Size = BubbleImage->ImageSize * ZoomFactor * 0.2f * Params.WireThickness;
            for (float Distance = Offset; Distance < Length; Distance += Spacing)
            {
                const FVector2f Position = Transform(Evaluate(Distance)) - Size * 0.5f;
                FSlateDrawElement::MakeBox(DrawElementsList, Layer, FPaintGeometry(Position, Size, ZoomFactor),
                    BubbleImage, ESlateDrawEffect::None, Params.WireColor);
            }
        }
        if (MidpointImage)
        {
            FVector2f Direction;
            const FVector2f Position = Transform(Evaluate(Length * 0.5f, &Direction)) - MidpointRadius;
            FSlateDrawElement::MakeRotatedBox(DrawElementsList, Layer,
                FPaintGeometry(Position, MidpointImage->ImageSize * ZoomFactor, ZoomFactor), MidpointImage,
                ESlateDrawEffect::None, FMath::Atan2(Direction.Y, Direction.X), TOptional<FVector2f>(),
                FSlateDrawElement::RelativeToElement, Params.WireColor);
        }
    }

    virtual bool HaveConnectionsGraphicallyChanged(const SGraphPanel& GraphPanel, const FVector2f& Mouse) const override
    {
        FGraphSplineOverlapResult Overlap;
        for (const auto& Piece : HitPieces)
        {
            FConnectionParams Params = Piece.Params;
            Params.AssociatedPin1 = Piece.StartPinHandle.GetPinObj(GraphPanel);
            Params.AssociatedPin2 = Piece.EndPinHandle.GetPinObj(GraphPanel);
            if (!Params.AssociatedPin1 || !Params.AssociatedPin2) { return true; }
            CheckSplineConnectionOverlapWithCursor(Mouse, Piece.StartPoint, Piece.EndPoint, Params, Overlap);
        }
        TSharedPtr<SGraphPin> A, B;
        Overlap.GetPinWidgets(GraphPanel, A, B);
        TSet<FEdGraphPinReference> Hovered;
        if (A && A->GetPinObj()) { Hovered.Add(A->GetPinObj()); }
        if (B && B->GetPinObj()) { Hovered.Add(B->GetPinObj()); }
        return FKismetConnectionDrawingPolicy::HaveConnectionsGraphicallyChanged(GraphPanel, Hovered);
    }

private:
    void AddHitPiece(FVector2f Start, FVector2f End, const FConnectionParams& Params)
    {
        if (!Params.AssociatedPin1 || !Params.AssociatedPin2) { return; }
        FSimpleConnectionData Piece;
        Piece.StartPinHandle = FGraphPinHandle(Params.AssociatedPin1); Piece.EndPinHandle = FGraphPinHandle(Params.AssociatedPin2);
        Piece.StartPoint = Start; Piece.EndPoint = End; Piece.Params = Params;
        Piece.Params.AssociatedPin1 = nullptr; Piece.Params.AssociatedPin2 = nullptr;
        HitPieces.Add(MoveTemp(Piece));
    }
    static void CorrectPinDistances(FGraphSplineOverlapResult& Result, float PreviousDistance, const FConnectionParams& Params,
        FVector2f Start, FVector2f End, FVector2f Mouse)
    {
        if (Result.GetDistanceSquared() < PreviousDistance)
        {
            Result = FGraphSplineOverlapResult(Params.AssociatedPin1, Params.AssociatedPin2, Result.GetDistanceSquared(),
                (Start - Mouse).SizeSquared(), (End - Mouse).SizeSquared(), true);
        }
    }
    TWeakPtr<const FWireDrawing> Factory;
    TWeakPtr<SGraphPanel> Panel;
    TSharedPtr<FRouteCache> Cache;
    TArray<FSimpleConnectionData> HitPieces;
    FVector2f PaintOrigin = FVector2f::ZeroVector;
    bool bSynthesizedStart = false, bSynthesizedEnd = false;
};
}

FWireDrawing::FWireDrawing() : WireStyle(GetDefault<UGlooPrintSettings>()->GetWireStyle()) {}

void FWireDrawing::OnSettingsChanged()
{
    const auto CurrentStyle = GetDefault<UGlooPrintSettings>()->GetWireStyle();
    if (bStopped || WireStyle == CurrentStyle) { return; }
    WireStyle = CurrentStyle;
    for (const auto& Weak : Caches) { if (const auto Cache = Weak.Pin()) { Cache->Invalidate(); } }
    FSlateApplication::Get().InvalidateAllWidgets(false);
}

FConnectionDrawingPolicy* FWireDrawing::CreateConnectionPolicy(const UEdGraphSchema* Schema,
    int32 BackLayer, int32 FrontLayer, float Zoom, const FSlateRect& Clip, FSlateWindowElementList& Elements, UEdGraph* Graph) const
{
    if (bStopped || GetDefault<UGlooPrintSettings>()->GetWireStyle() == EGlooPrintWireStyle::Native ||
        !Schema || Schema->GetClass() != UEdGraphSchema_K2::StaticClass() || !Graph) { return nullptr; }
    return new FRouteDrawingPolicy(BackLayer, FrontLayer, Zoom, Clip, Elements, Graph, StaticCastSharedRef<const FWireDrawing>(AsShared()));
}

TSharedPtr<FRouteCache> FWireDrawing::GetCache(TSharedRef<SGraphPanel> Panel) const
{
    if (bStopped || GetDefault<UGlooPrintSettings>()->GetWireStyle() == EGlooPrintWireStyle::Native) { return nullptr; }
    auto Cache = Panel->GetMetaData<FRouteCache>();
    if (Cache && Cache->GetGraph() != Panel->GetGraphObj())
    {
        Cache->Shutdown(); Panel->RemoveMetaData(Cache.ToSharedRef()); Cache.Reset();
    }
    if (!Cache)
    {
        Cache = MakeShared<FRouteCache>();
        Panel->AddMetadata(Cache.ToSharedRef());
        Caches.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
        Caches.Add(Cache);
        Cache->Initialize(Panel);
    }
    return Cache;
}

void FWireDrawing::Shutdown()
{
    bStopped = true;
    for (const auto& Weak : Caches)
    {
        if (const auto Cache = Weak.Pin())
        {
            Cache->Shutdown();
            if (const auto Panel = Cache->GetPanel()) { Panel->RemoveMetaData(Cache.ToSharedRef()); }
        }
    }
    Caches.Reset();
}
}
