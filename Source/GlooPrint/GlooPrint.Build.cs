// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

using UnrealBuildTool;

public class GlooPrint : ModuleRules
{
    public GlooPrint(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "RenderCore",
            "GraphEditor", "BlueprintGraph", "UnrealEd", "InputCore", "Settings", "MaterialEditor", "ToolMenus"
        });
        if (Target.bBuildDeveloperTools)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "Kismet", "AnimGraph" });
            PrivateDependencyModuleNames.Add("AutomationDriver");
            PrivateDependencyModuleNames.Add("ApplicationCore");
        }
    }
}
