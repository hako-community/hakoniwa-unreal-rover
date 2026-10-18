// HakoniwaRoverEditor.Build.cs — 月面ローバの場面を組むエディタ用モジュール（ROV-15〜）。

using UnrealBuildTool;

public class HakoniwaRoverEditor : ModuleRules
{
	public HakoniwaRoverEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "UnrealEd", "AssetRegistry",
			"Landscape", "Json", "ImageWrapper", "MaterialEditor", "CinematicCamera",
			"InterchangeCore", "InterchangeEngine", "InterchangePipelines",
			"MeshDescription", "StaticMeshDescription", "PhysicsCore",
			"HakoniwaRover",
		});
	}
}
