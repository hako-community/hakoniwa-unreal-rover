// HakoniwaRover.Build.cs — 月面ローバの実行時モジュール（ROV-15〜）。ドローンのモジュールとは分ける。

using UnrealBuildTool;

public class HakoniwaRover : ModuleRules
{
	public HakoniwaRover(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "CinematicCamera",
		});
		PrivateDependencyModuleNames.AddRange(new string[] { "Json", "Landscape", "RHI" });
	}
}
