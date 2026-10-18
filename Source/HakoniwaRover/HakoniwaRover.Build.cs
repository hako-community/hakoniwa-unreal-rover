// HakoniwaRover.Build.cs — 月面ローバの実行時モジュール（ROV-15〜）。ドローンのモジュールとは分ける。

using UnrealBuildTool;

public class HakoniwaRover : ModuleRules
{
	public HakoniwaRover(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bEnableExceptions = true;   // hakoniwa-pdu-registry の C++ の変換（std::string・std::vector）

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "CinematicCamera",
		});
		PrivateDependencyModuleNames.AddRange(new string[] { "Json", "Landscape", "RHI", "HakoniwaPdu" });
		// 箱庭のアセット（HakoniwaShmClient。hakoniwa-unreal-drone と同じ）：PduManager は公開ヘッダに出る・ゲームパッドを捨てる入力の前処理
		PublicDependencyModuleNames.AddRange(new string[] { "HakoniwaPdu", "InputCore" });
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
	}
}
