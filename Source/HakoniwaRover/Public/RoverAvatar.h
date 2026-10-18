// RoverAvatar.h
// 箱庭の PLANT の走行を写すローバ（ROV-22）。部品の木（車体・懸架・操舵・車輪）は場面を組むときに作って地図に保存し、
// 実行時は共有メモリから base_link_pos（車体の位置と姿勢）と joint_state（関節）を読んで動かす。
//
// コマンドライン（-game のとき）:
//   -RoverSync                 共有メモリを読む（外から読むだけ。箱庭のアセットとしては登録しないので時間を止めない）
//   -RoverHakoConfig=<json>    共有メモリの設定（HAKO_CONFIG_PATH）。無ければ環境変数のまま
//   -RoverChase                後ろを追うカメラで見る
//   -RoverSyncShot=<png> -RoverSyncShotAfter=<秒>   受信が始まってから指定秒後に 1 枚撮る（確認用）
//
// 座標：アクタ = 車体（PLANT の chassis）の座標。体の座標（右手系 m・x 前・y 左・z 上）→ Unreal は RoverFrame。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoverAvatar.generated.h"

class UCineCameraComponent;

USTRUCT()
struct FRoverCorner
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere) FString SuspJoint;
	UPROPERTY(VisibleAnywhere) FString SteerJoint;     // 空なら操舵なし（後軸）
	UPROPERTY(VisibleAnywhere) FString WheelJoint;
	UPROPERTY(VisibleAnywhere) FVector RestPivotCm = FVector::ZeroVector;   // アクタの座標での取付位置（静止時）
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Susp;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Steer;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Wheel;
};

UCLASS()
class HAKONIWAROVER_API ARoverAvatar : public AActor
{
	GENERATED_BODY()

public:
	ARoverAvatar();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** 部品の木（場面を組むときに作る） */
	UPROPERTY(VisibleAnywhere, Category = "Rover") TObjectPtr<USceneComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "Rover") TArray<FRoverCorner> Corners;

	/** 箱庭の PDU（viewer_sync.json から） */
	UPROPERTY(EditAnywhere, Category = "Rover|Hakoniwa") FString RobotName = TEXT("LunarRover");
	UPROPERTY(EditAnywhere, Category = "Rover|Hakoniwa") int32 BaseChannel = 6;
	UPROPERTY(EditAnywhere, Category = "Rover|Hakoniwa") int32 BaseSize = 72;
	UPROPERTY(EditAnywhere, Category = "Rover|Hakoniwa") int32 JointChannel = 1;
	UPROPERTY(EditAnywhere, Category = "Rover|Hakoniwa") int32 JointSize = 4096;

	/** 体の座標の回転（右手系）を Unreal の回転にする（S·R·S、S = diag(1, −1, 1)） */
	static FQuat FluRotationToUnreal(const FMatrix& RowMajorFlu);

private:
	void ApplyBase(double X, double Y, double Z, double Ax, double Ay, double Az);
	void ApplyJoints(const TArray<FString>& Names, const TArray<double>& Positions);
	void UpdateChase(float DeltaSeconds);
	FString WheelGaps() const;
	FString FirstCornerPose() const;

	bool bSync = false;
	bool bAttached = false;
	bool bChase = false;
	double NextAttachTry = 0.0;
	int64 BaseFrames = 0;
	int64 JointFrames = 0;
	double FirstRecvTime = -1.0;
	double LastRollDeg = 0.0;
	double LastPitchDeg = 0.0;
	double LastWheelFL = 0.0;
	double LogTimer = 0.0;
	FString SyncShot;
	double SyncShotAfter = 20.0;
	bool bShotDone = false;
	TArray<uint8> Buf;

	UPROPERTY() TObjectPtr<UCineCameraComponent> ChaseCam;
};
