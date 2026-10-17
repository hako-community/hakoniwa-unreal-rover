// RoverLunarLook.h
// 月面の「写り」の約束（ROV-15・検討書 §3.5）。場面の PostProcessVolume と各カメラで同じ値を使う。
//   * 自動露出なし：物理カメラの露出（ISO 100・1/125 s・f/16 → EV100 ≈ 15）。
//     太陽 13 万 lux の下でアルベド 0.12 のレゴリスが中間よりやや暗く写る（晴天の f/16 の規則と同じ考え方）。
//   * 局所露出（影を持ち上げる効果）は切る：影の中が真っ暗に写ることを確かめたいので。

#pragma once

#include "CoreMinimal.h"
#include "Engine/Scene.h"

namespace RoverLunarLook
{
	constexpr float Iso = 100.0f;
	constexpr float ShutterSpeed = 125.0f;   // 1/125 s
	constexpr float Fstop = 16.0f;

	inline void ApplyExposure(FPostProcessSettings& S)
	{
		S.bOverride_AutoExposureMethod = true;
		S.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		S.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		S.AutoExposureApplyPhysicalCameraExposure = 1;
		S.bOverride_AutoExposureBias = true;
		S.AutoExposureBias = 0.0f;
		S.bOverride_CameraISO = true;
		S.CameraISO = Iso;
		S.bOverride_CameraShutterSpeed = true;
		S.CameraShutterSpeed = ShutterSpeed;
		S.bOverride_DepthOfFieldFstop = true;
		S.DepthOfFieldFstop = Fstop;
		S.bOverride_LocalExposureHighlightContrastScale = true;
		S.LocalExposureHighlightContrastScale = 1.0f;
		S.bOverride_LocalExposureShadowContrastScale = true;
		S.LocalExposureShadowContrastScale = 1.0f;
	}
}
