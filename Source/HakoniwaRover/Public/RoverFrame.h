// RoverFrame.h
// 座標の約束（1 か所にまとめる）。
//   局所座標（地形タイルの grid.json・MuJoCo の世界・FLU）：右手系、m、x 前・y 左・z 上
//   Unreal：左手系、cm、X 前・Y 右・Z 上
//   → Unreal = (100·x, −100·y, 100·z)。向き（単位ベクトル）は (x, −y, z)。

#pragma once

#include "CoreMinimal.h"

namespace RoverFrame
{
	inline FVector LocalToUnreal(double X, double Y, double Z) { return FVector(X * 100.0, -Y * 100.0, Z * 100.0); }
	inline FVector LocalDirToUnreal(double X, double Y, double Z) { return FVector(X, -Y, Z).GetSafeNormal(); }
	inline FVector UnrealToLocal(const FVector& U) { return FVector(U.X / 100.0, -U.Y / 100.0, U.Z / 100.0); }
}
