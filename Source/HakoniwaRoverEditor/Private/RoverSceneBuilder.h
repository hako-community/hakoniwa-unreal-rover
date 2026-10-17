// RoverSceneBuilder.h
// 月面の 1 場面を、地形タイルの契約（grid.json・16bit 高さ画像）と位置暦（sun.json）から組む（ROV-15）。
//
//   地形   ：16bit 高さ画像 → Landscape（倍率は grid.json の unreal。局所座標の点と同じ高さになるように置く）
//   光     ：太陽（平行光・sun.json の向きと照度・視直径）、地球（遠くの球・太陽で照らされて満ち欠けが出る）と地球照
//   空     ：空の光・大気・霧を置かない（黒い空・影の中は地球照と照り返しだけ）
//   写り   ：自動露出なしの物理カメラ（RoverLunarLook）
//   ローバ ：ローバの GLB を 1 つのメッシュとして取り込み、原点に地形の傾きに合わせて置く（関節の同期は ROV-22）
//   カメラ ：撮影用のシネカメラ（タグ RoverShot）と撮影係（ARoverShotDirector）

#pragma once

#include "CoreMinimal.h"

struct FRoverSceneArgs
{
	FString Grid;   // <name>.grid.json
	FString Sun;    // <name>.sun.json
	FString Glb;    // ローバの GLB（空なら置かない）
	FString Map = TEXT("/Game/Rover/Levels/LunarSouthPole");
};

namespace RoverSceneBuilder
{
	/** 新しい地図に場面を組む。保存は Save()（Landscape の更新を待ってから）。 */
	bool Build(const FRoverSceneArgs& Args);
	bool Save(const FRoverSceneArgs& Args);
}
