// RoverAvatar.cpp

#include "RoverAvatar.h"

#include "CineCameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HakoCapiCompat.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RoverFrame.h"
#include "RoverLunarLook.h"
#include "UnrealClient.h"

#include "pdu_convertor.hpp"
#include "geometry_msgs/pdu_cpptype_conv_Twist.hpp"
// この版の hakoniwa-pdu-registry は文字列の可変長配列の変換を ros 名でだけ持つ（hakoniwa-mujoco-sensor と同じ読み替え）
#define hako_convert_pdu2cpp_array_string_varray hako_convert_pdu2ros_array_string_varray
#define hako_convert_cpp2pdu_array_string_varray hako_convert_ros2pdu_array_string_varray
#include "sensor_msgs/pdu_cpptype_conv_JointState.hpp"
#undef hako_convert_pdu2cpp_array_string_varray
#undef hako_convert_cpp2pdu_array_string_varray

DEFINE_LOG_CATEGORY_STATIC(LogRoverAvatar, Log, All);

namespace
{
	/** 右手系の回転行列（列ベクトルの約束・R[行][列]） */
	FMatrix RotX(double A) { const double c = FMath::Cos(A), s = FMath::Sin(A); FMatrix M = FMatrix::Identity; M.M[1][1] = c; M.M[1][2] = -s; M.M[2][1] = s; M.M[2][2] = c; return M; }
	FMatrix RotY(double A) { const double c = FMath::Cos(A), s = FMath::Sin(A); FMatrix M = FMatrix::Identity; M.M[0][0] = c; M.M[0][2] = s; M.M[2][0] = -s; M.M[2][2] = c; return M; }
	FMatrix RotZ(double A) { const double c = FMath::Cos(A), s = FMath::Sin(A); FMatrix M = FMatrix::Identity; M.M[0][0] = c; M.M[0][1] = -s; M.M[1][0] = s; M.M[1][1] = c; return M; }
	FMatrix Mul(const FMatrix& A, const FMatrix& B)   // 列ベクトルの約束で A·B
	{
		FMatrix R = FMatrix::Identity;
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
		{
			double v = 0.0;
			for (int k = 0; k < 3; ++k) v += A.M[i][k] * B.M[k][j];
			R.M[i][j] = v;
		}
		return R;
	}
}

ARoverAvatar::ARoverAvatar()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Chassis"));
	Body = CreateDefaultSubobject<USceneComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	ChaseCam = CreateDefaultSubobject<UCineCameraComponent>(TEXT("ChaseCam"));
	ChaseCam->SetupAttachment(RootComponent);
	ChaseCam->SetUsingAbsoluteLocation(true);
	ChaseCam->SetUsingAbsoluteRotation(true);
	ChaseCam->SetAutoActivate(false);
}

FQuat ARoverAvatar::FluRotationToUnreal(const FMatrix& R)
{
	// Unreal の座標 = S·体の座標（S = diag(1, −1, 1)）なので、回転は S·R·S。
	// FMatrix は行ベクトルの約束（v' = v·M）なので、転置して入れる
	static const double S[3] = { 1.0, -1.0, 1.0 };
	FMatrix M = FMatrix::Identity;
	for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
	{
		M.M[j][i] = S[i] * S[j] * R.M[i][j];
	}
	return FQuat(M);
}

void ARoverAvatar::BeginPlay()
{
	Super::BeginPlay();
	const TCHAR* Cmd = FCommandLine::Get();
	bSync = FParse::Param(Cmd, TEXT("RoverSync"));
	bChase = FParse::Param(Cmd, TEXT("RoverChase"));
	FParse::Value(Cmd, TEXT("-RoverSyncShot="), SyncShot);
	FParse::Value(Cmd, TEXT("-RoverSyncShotAfter="), SyncShotAfter);
	FString Config;
	if (FParse::Value(Cmd, TEXT("-RoverHakoConfig="), Config))
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("HAKO_CONFIG_PATH"), *FPaths::ConvertRelativePathToFull(Config));
	}
	Buf.SetNumZeroed(FMath::Max(BaseSize, JointSize));
	RoverLunarLook::ApplyExposure(ChaseCam->PostProcessSettings);
	ChaseCam->PostProcessBlendWeight = 1.0f;
	ChaseCam->Filmback.SensorWidth = 36.0f;
	ChaseCam->Filmback.SensorHeight = 20.25f;
	ChaseCam->SetCurrentFocalLength(24.0f);
	ChaseCam->CurrentAperture = RoverLunarLook::Fstop;
	ChaseCam->FocusSettings.FocusMethod = ECameraFocusMethod::Disable;
	if (bChase)
	{
		ChaseCam->SetActive(true);
	}
	UE_LOG(LogRoverAvatar, Log, TEXT("sync=%d chase=%d robot=%s base ch%d joints ch%d corners=%d HAKO_CONFIG_PATH=%s"),
		bSync, bChase, *RobotName, BaseChannel, JointChannel, Corners.Num(), *FPlatformMisc::GetEnvironmentVariable(TEXT("HAKO_CONFIG_PATH")));
}

void ARoverAvatar::ApplyBase(double X, double Y, double Z, double Ax, double Ay, double Az)
{
	// PLANT の姿勢の約束：angular = (−roll, −pitch, yaw)（viewer_sync.json の base_euler）
	const FMatrix R = Mul(RotZ(Az), Mul(RotY(-Ay), RotX(-Ax)));
	SetActorLocationAndRotation(RoverFrame::LocalToUnreal(X, Y, Z), FluRotationToUnreal(R));
	LastRollDeg = FMath::RadiansToDegrees(-Ax);    // 体の座標の右手系（左が上がると +）
	LastPitchDeg = FMath::RadiansToDegrees(-Ay);   // 機首下げが +
}

FString ARoverAvatar::FirstCornerPose() const
{
	// 最初の隅（FL）の部品に入れた値を体の座標の符号で：操舵（左へ +）・懸架（上へ +）・車輪の回転（前進で +）
	if (Corners.Num() == 0 || !Corners[0].Susp || !Corners[0].Wheel)
	{
		return FString();
	}
	const FRoverCorner& C = Corners[0];
	const double SteerDeg = C.Steer ? -C.Steer->GetRelativeRotation().Yaw : 0.0;   // Unreal のヨーは左手系（右へ +）
	const double SuspCm = C.Susp->GetRelativeLocation().Z - C.RestPivotCm.Z;
	// 車輪は回り続けるので Rotator（ピッチは ±90° で折り返す）では読めない。受信した角度と、部品の前の向きの高さで見る
	const FVector Fwd = C.Wheel->GetRelativeTransform().GetUnitAxis(EAxis::X);
	return FString::Printf(TEXT("FL steer %.1f deg susp %.1f cm wheel q %.0f deg (front z %.2f)"),
		SteerDeg, SuspCm, FMath::RadiansToDegrees(LastWheelFL), Fwd.Z);
}

FString ARoverAvatar::WheelGaps() const
{
	// 車輪の下端（車輪の中心から真下へ半径）と地面の高さの差。負なら沈んでいる。並びは corners の順（FL FR ML MR RL RR）
	// 半径 = 静止時の車輪の中心の高さ − GLB の原点（車輪の下端）の高さ
	FCollisionQueryParams Q(SCENE_QUERY_STAT(RoverWheelGap), false, this);
	TArray<FString> Out;
	for (const FRoverCorner& C : Corners)
	{
		if (!C.Wheel)
		{
			continue;
		}
		const double Radius = C.RestPivotCm.Z - Body->GetRelativeLocation().Z;
		const FVector W = C.Wheel->GetComponentLocation();
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, W + FVector(0, 0, 500.0), W - FVector(0, 0, 1000.0), ECC_Visibility, Q))
		{
			Out.Add(FString::Printf(TEXT("%.0f"), W.Z - Radius - Hit.ImpactPoint.Z));
		}
		else
		{
			Out.Add(TEXT("-"));
		}
	}
	return FString::Join(Out, TEXT(" "));
}

void ARoverAvatar::ApplyJoints(const TArray<FString>& Names, const TArray<double>& Positions)
{
	TMap<FString, double> Q;
	for (int32 i = 0; i < FMath::Min(Names.Num(), Positions.Num()); ++i)
	{
		Q.Add(Names[i], Positions[i]);
	}
	for (FRoverCorner& C : Corners)
	{
		if (const double* S = Q.Find(C.SuspJoint); S && C.Susp)
		{
			C.Susp->SetRelativeLocation(C.RestPivotCm + FVector(0.0, 0.0, *S * 100.0));   // 懸架：体の +z へ直動
		}
		if (const double* St = Q.Find(C.SteerJoint); St && C.Steer)
		{
			C.Steer->SetRelativeRotation(FluRotationToUnreal(RotZ(*St)));                 // 操舵：体の +z まわり
		}
		if (const double* W = Q.Find(C.WheelJoint); W && C.Wheel)
		{
			C.Wheel->SetRelativeRotation(FluRotationToUnreal(RotY(*W)));                  // 車輪：体の +y（左）まわり
			if (&C == &Corners[0])
			{
				LastWheelFL = *W;
			}
		}
	}
}

void ARoverAvatar::UpdateChase(float DeltaSeconds)
{
	// 後ろ 18 m・上 6 m から、車体の少し上を見る。向きは車体の方位だけ（傾きは追わない）でなめらかに
	const FVector Fwd = FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0).GetSafeNormal();
	const FVector Target = GetActorLocation() + FVector(0, 0, 150.0);
	const FVector Want = GetActorLocation() - Fwd * 1800.0 + FVector(0, 0, 600.0);
	const float A = 1.0f - FMath::Exp(-DeltaSeconds * 2.0f);
	const FVector Pos = ChaseCam->GetComponentLocation().IsNearlyZero() ? Want : FMath::Lerp(ChaseCam->GetComponentLocation(), Want, A);
	ChaseCam->SetWorldLocationAndRotation(Pos, (Target - Pos).Rotation());
}

void ARoverAvatar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Now = GetWorld()->GetTimeSeconds();
	if (bChase)
	{
		UpdateChase(DeltaSeconds);
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0); PC && PC->GetViewTarget() != this)
		{
			PC->SetViewTarget(this);
		}
	}
	if (!bSync)
	{
		return;
	}
	if (!bAttached)
	{
		if (Now >= NextAttachTry)
		{
			NextAttachTry = Now + 2.0;
			bAttached = hako_asset_init();
			UE_LOG(LogRoverAvatar, Log, TEXT("hako_asset_init: %s"), bAttached ? TEXT("attached") : TEXT("not yet (master running?)"));
		}
		return;
	}
	if (!hako_asset_is_pdu_created())
	{
		return;
	}
	const FTCHARToUTF8 Robot(*RobotName);
	if (hako_asset_read_pdu_nolock(Robot.Get(), BaseChannel, reinterpret_cast<char*>(Buf.GetData()), BaseSize))
	{
		HakoCpp_Twist T {};
		hako::pdu::PduConvertor<HakoCpp_Twist, hako::pdu::msgs::geometry_msgs::Twist> Conv;
		if (Conv.pdu2cpp(reinterpret_cast<char*>(Buf.GetData()), T))
		{
			ApplyBase(T.linear.x, T.linear.y, T.linear.z, T.angular.x, T.angular.y, T.angular.z);
			++BaseFrames;
		}
	}
	if (hako_asset_read_pdu_nolock(Robot.Get(), JointChannel, reinterpret_cast<char*>(Buf.GetData()), JointSize))
	{
		HakoCpp_JointState J {};
		hako::pdu::PduConvertor<HakoCpp_JointState, hako::pdu::msgs::sensor_msgs::JointState> Conv;
		if (Conv.pdu2cpp(reinterpret_cast<char*>(Buf.GetData()), J) && !J.name.empty())
		{
			TArray<FString> Names;
			TArray<double> Pos;
			for (size_t i = 0; i < J.name.size(); ++i)
			{
				Names.Add(UTF8_TO_TCHAR(J.name[i].c_str()));
				Pos.Add(i < J.position.size() ? J.position[i] : 0.0);
			}
			ApplyJoints(Names, Pos);
			++JointFrames;
		}
	}
	if (BaseFrames > 0 && FirstRecvTime < 0.0)
	{
		FirstRecvTime = Now;
	}
	LogTimer += DeltaSeconds;
	if (LogTimer >= 5.0)
	{
		LogTimer = 0.0;
		const FVector L = RoverFrame::UnrealToLocal(GetActorLocation());
		UE_LOG(LogRoverAvatar, Log, TEXT("recv base=%lld joints=%lld  pos (%.2f, %.2f, %.2f) m  roll %.1f pitch %.1f deg  fps %.0f  wheel gap [cm] %s  %s"),
			BaseFrames, JointFrames, L.X, L.Y, L.Z, LastRollDeg, LastPitchDeg, DeltaSeconds > 0 ? 1.0 / DeltaSeconds : 0.0, *WheelGaps(), *FirstCornerPose());
	}
	if (!SyncShot.IsEmpty() && !bShotDone && FirstRecvTime >= 0.0 && Now - FirstRecvTime >= SyncShotAfter)
	{
		bShotDone = true;
		FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(SyncShot), false, false);
		const FVector L = RoverFrame::UnrealToLocal(GetActorLocation());
		UE_LOG(LogRoverAvatar, Log, TEXT("sync shot -> %s  (pos %.2f, %.2f, %.2f m)"), *SyncShot, L.X, L.Y, L.Z);
	}
}
