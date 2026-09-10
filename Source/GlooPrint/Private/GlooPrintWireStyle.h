// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlooPrintWireStyle.generated.h"

UENUM()
enum class EGlooPrintWireStyle : uint8
{
    Rounded90 UMETA(DisplayName = "Rounded 90 degrees"),
    Diagonal45 UMETA(DisplayName = "45 degrees"),
    Native UMETA(DisplayName = "Native Unreal splines")
};
