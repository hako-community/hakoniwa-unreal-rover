// RoverShotDirector.cpp

#include "RoverShotDirector.h"

#include "CineCameraActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "LandscapeProxy.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RoverFrame.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoverShot, Log, All);

ARoverShotDirector::ARoverShotDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ARoverShotDirector::BeginPlay()
{
	Super::BeginPlay();

	for (TActorIterator<ACineCameraActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TEXT("RoverShot")))
		{
			Cameras.Add(*It);
		}
	}
	Cameras.Sort([](const ACineCameraActor& A, const ACineCameraActor& B) { return A.GetName() < B.GetName(); });

	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("-RoverShot="), ShotDir);
	FParse::Value(Cmd, TEXT("-RoverProbeIn="), ProbeIn);
	FParse::Value(Cmd, TEXT("-RoverProbeOut="), ProbeOut);
	bProbeDone = ProbeIn.IsEmpty();
	// 岩の検査：場面の岩（HISM）は当たり判定を切って保存してあるので、ここで入れる（数フレーム後に RunProbe が当てる）
	if (FParse::Param(Cmd, TEXT("RoverProbeRocks")))
	{
		int32 N = 0;
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			TArray<UInstancedStaticMeshComponent*> Comps;
			It->GetComponents(Comps);
			for (UInstancedStaticMeshComponent* C : Comps)
			{
				C->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				N += C->GetInstanceCount();
			}
		}
		UE_LOG(LogRoverShot, Log, TEXT("probe: rock collision on (%d instances)"), N);
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (PC && Cameras.Num() > 0)
	{
		PC->SetViewTarget(Cameras[0]);
	}
	UE_LOG(LogRoverShot, Log, TEXT("cameras=%d shot=%s probe=%s"), Cameras.Num(), *ShotDir, *ProbeIn);
	// 光の確認用：平行光源の向き（光の進む向き）・仰角・強さ
	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		const UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		const FVector F = C->GetDirection();
		UE_LOG(LogRoverShot, Log, TEXT("light %s: travel=%s  source elevation=%.2f deg  intensity=%.1f lux  angle=%.3f deg"),
			*It->GetActorNameOrLabel(), *F.ToString(), FMath::RadiansToDegrees(FMath::Asin(-F.Z)), C->Intensity, C->LightSourceAngle);
	}
}

void ARoverShotDirector::RunProbe()
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ProbeIn))
	{
		UE_LOG(LogRoverShot, Error, TEXT("probe: cannot read %s"), *ProbeIn);
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	TArray<FString> Out = { TEXT("x,y,z") };
	int32 Missed = 0, OnRock = 0;
	// -RoverProbeRocks：岩（場面の生成で HISM に並べたもの）への当たりも採用する（岩の検査。地形の検査では外す）
	const bool bRocks = FParse::Param(FCommandLine::Get(), TEXT("RoverProbeRocks"));
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		TArray<FString> Cols;
		Lines[i].ParseIntoArray(Cols, TEXT(","));
		if (Cols.Num() < 2)
		{
			continue;
		}
		const double X = FCString::Atod(*Cols[0]);
		const double Y = FCString::Atod(*Cols[1]);
		// 地形（Landscape）だけに当てる。ローバなど他のものに当たったら、それを外して当て直す
		FCollisionQueryParams Params(TEXT("RoverProbe"), true);
		const FVector Start = RoverFrame::LocalToUnreal(X, Y, 2000.0);
		const FVector End = RoverFrame::LocalToUnreal(X, Y, -2000.0);
		FString Z = TEXT("nan");
		for (int32 Try = 0; Try < 8; ++Try)
		{
			FHitResult Hit;
			if (!GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Params))
			{
				break;
			}
			const bool bRock = bRocks && Hit.GetComponent() && Hit.GetComponent()->IsA<UInstancedStaticMeshComponent>();
			if ((Hit.GetActor() && Hit.GetActor()->IsA<ALandscapeProxy>()) || bRock)
			{
				Z = FString::Printf(TEXT("%.4f"), RoverFrame::UnrealToLocal(Hit.ImpactPoint).Z);
				OnRock += bRock ? 1 : 0;
				break;
			}
			Params.AddIgnoredActor(Hit.GetActor());
		}
		Missed += Z == TEXT("nan") ? 1 : 0;
		Out.Add(FString::Printf(TEXT("%s,%s,%s"), *Cols[0].TrimStartAndEnd(), *Cols[1].TrimStartAndEnd(), *Z));
	}
	FFileHelper::SaveStringArrayToFile(Out, *ProbeOut);
	UE_LOG(LogRoverShot, Log, TEXT("probe: %d points -> %s (missed %d, on rocks %d)"), Out.Num() - 1, *ProbeOut, Missed, OnRock);
}

void ARoverShotDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	++TotalFrames;

	// 当たり判定が登録されてから（数フレーム待つ）
	if (!bProbeDone && TotalFrames > 5)
	{
		RunProbe();
		bProbeDone = true;
		if (ShotDir.IsEmpty())
		{
			FinishAndExit();
			return;
		}
	}
	if (ShotDir.IsEmpty() || !bProbeDone)
	{
		return;
	}
	if (CameraIndex >= Cameras.Num())
	{
		FinishAndExit();
		return;
	}

	ACineCameraActor* Cam = Cameras[CameraIndex];
	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (FrameInShot == 0 && PC)
	{
		PC->SetViewTarget(Cam);
		MeasureSum = 0.0;
		MeasureCount = 0;
	}
	++FrameInShot;
	if (FrameInShot > WarmupFrames - MeasureFrames && FrameInShot <= WarmupFrames)
	{
		MeasureSum += FApp::GetDeltaTime();
		++MeasureCount;
	}
	if (FrameInShot == WarmupFrames)
	{
		const FString File = FPaths::ConvertRelativePathToFull(ShotDir / Cam->GetName() + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(File, false, false);
		const double Fps = MeasureCount > 0 ? MeasureCount / MeasureSum : 0.0;
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("camera"), Cam->GetName());
		R->SetStringField(TEXT("file"), File);
		R->SetNumberField(TEXT("fps_mean"), Fps);
		R->SetNumberField(TEXT("frames_measured"), MeasureCount);
		Results.Add(MakeShared<FJsonValueObject>(R));
		UE_LOG(LogRoverShot, Log, TEXT("shot %s  %.1f fps -> %s"), *Cam->GetName(), Fps, *File);
	}
	if (FrameInShot >= WarmupFrames + 5)   // 撮影が書き出されるまで待つ
	{
		++CameraIndex;
		FrameInShot = 0;
	}
}

void ARoverShotDirector::FinishAndExit()
{
	if (!ShotDir.IsEmpty())
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("shots"), Results);
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
			Root->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), Size.X, Size.Y));
		}
		Root->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT(""));
		FString Json;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		FFileHelper::SaveStringToFile(Json, *(ShotDir / TEXT("shots.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	UE_LOG(LogRoverShot, Log, TEXT("done; exiting"));
	SetActorTickEnabled(false);
	FPlatformMisc::RequestExit(false);
}
