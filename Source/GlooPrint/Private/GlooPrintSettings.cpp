// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#include "GlooPrintSettings.h"
#include "SNodePanel.h"

GlooPrint::FLayoutSettings UGlooPrintSettings::GetLayoutSettings() const
{
    return {
        FMath::IsFinite(HorizontalSpacing) ? FMath::Clamp(HorizontalSpacing, 16.f, 2048.f) : 96.f,
        FMath::IsFinite(VerticalSpacing) ? FMath::Clamp(VerticalSpacing, 16.f, 2048.f) : 48.f,
        FMath::IsFinite(CommentPadding) ? FMath::Clamp(CommentPadding, 8.f, 512.f) : 32.f,
        FMath::Clamp(SNodePanel::GetSnapGridSize(), 1u, 100u)
    };
}

EGlooPrintWireStyle UGlooPrintSettings::GetWireStyle() const
{
    return WireStyle == EGlooPrintWireStyle::Diagonal45 || WireStyle == EGlooPrintWireStyle::Native ?
        WireStyle : EGlooPrintWireStyle::Rounded90;
}

bool UGlooPrintSettings::NotifyChanged()
{
    const auto Layout = GetLayoutSettings();
    HorizontalSpacing = Layout.HorizontalSpacing; VerticalSpacing = Layout.VerticalSpacing; CommentPadding = Layout.CommentPadding;
    WireStyle = GetWireStyle();
    OnChanged.Broadcast();
    return true;
}

void UGlooPrintSettings::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    NotifyChanged();
    Super::PostEditChangeProperty(Event);
}

void UGlooPrintSettings::PostReloadConfig(FProperty* Property)
{
    Super::PostReloadConfig(Property);
    NotifyChanged();
}
