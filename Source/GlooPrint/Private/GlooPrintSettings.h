// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "GlooPrintLayout.h"
#include "GlooPrintWireStyle.h"
#include "UObject/Object.h"
#include "GlooPrintSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings)
class UGlooPrintSettings final : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Config, Category = "Formatting", meta = (DisplayName = "Formatting enabled"))
    bool bFormattingEnabled = true;

    UPROPERTY(EditAnywhere, Config, Category = "Wires")
    EGlooPrintWireStyle WireStyle = EGlooPrintWireStyle::Rounded90;

    UPROPERTY(EditAnywhere, Config, Category = "Formatting", meta = (ClampMin = "16", ClampMax = "2048", EditCondition = "bFormattingEnabled"))
    float HorizontalSpacing = 96;

    UPROPERTY(EditAnywhere, Config, Category = "Formatting", meta = (ClampMin = "16", ClampMax = "2048", EditCondition = "bFormattingEnabled"))
    float VerticalSpacing = 48;

    UPROPERTY(EditAnywhere, Config, Category = "Formatting", meta = (ClampMin = "8", ClampMax = "512", EditCondition = "bFormattingEnabled"))
    float CommentPadding = 32;

    GlooPrint::FLayoutSettings GetLayoutSettings() const;
    EGlooPrintWireStyle GetWireStyle() const;
    bool NotifyChanged();
    FSimpleMulticastDelegate OnChanged;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
    virtual void PostReloadConfig(FProperty* Property) override;
};
