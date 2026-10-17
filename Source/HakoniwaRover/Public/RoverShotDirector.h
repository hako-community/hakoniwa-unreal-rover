// RoverShotDirector.h
// 月面の場面の「撮影係」と「地形の高さの検査係」（ROV-15）。場面に 1 つ置く。
//
// コマンドライン（-game で起動したとき）:
//   -RoverShot=<フォルダ>     タグ RoverShot のシネカメラを名前順に切り替え、暖機のあと 1 枚ずつ撮って終了する。
//                             撮影直前のフレーム時間の平均を <フォルダ>/shots.json に書く（G3 の目安）。
//   -RoverProbeIn=<csv>       局所座標 x,y [m] の点で、地形（Landscape の当たり判定）へ鉛直にレイを当てる。
//   -RoverProbeOut=<csv>      その高さ z [m]（局所座標）を書く。tools/check_terrain.py --unreal で比べる。
// 引数が無いとき（PIE など）は、最初のカメラを視点にするだけ。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoverShotDirector.generated.h"

class ACineCameraActor;

UCLASS()
class HAKONIWAROVER_API ARoverShotDirector : public AActor
{
	GENERATED_BODY()

public:
	ARoverShotDirector();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** 1 つのカメラで撮る前に待つフレーム数（Lumen・仮想シャドウマップが落ち着くまで）。 */
	UPROPERTY(EditAnywhere, Category = "Rover")
	int32 WarmupFrames = 180;

	/** フレーム時間を平均するフレーム数（撮影の直前）。 */
	UPROPERTY(EditAnywhere, Category = "Rover")
	int32 MeasureFrames = 90;

private:
	void RunProbe();
	void FinishAndExit();

	TArray<TObjectPtr<ACineCameraActor>> Cameras;
	FString ShotDir;
	FString ProbeIn;
	FString ProbeOut;
	bool bProbeDone = false;
	int32 CameraIndex = 0;
	int32 FrameInShot = 0;
	int32 TotalFrames = 0;
	double MeasureSum = 0.0;
	int32 MeasureCount = 0;
	TArray<TSharedPtr<class FJsonValue>> Results;
};
