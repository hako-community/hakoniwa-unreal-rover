// RoverSceneBuilder.cpp

#include "RoverSceneBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeManager.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RoverFrame.h"
#include "RoverLunarLook.h"
#include "RoverShotDirector.h"
#include "RoverViewGameMode.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoverScene, Log, All);

namespace
{
	constexpr int32 QuadsPerSection = 63;            // Landscape の区画（63 × 63 の四角形 = 1 コンポーネント）
	constexpr double EarthDistanceCm = 5.0e6;        // 地球の球を置く距離（50 km。視直径が合えば距離は見た目に効かない）
	const TCHAR* MaterialDir = TEXT("/Game/Rover/Materials");
	// ★ ローバのメッシュは git に入れない（.gitignore）。各自の PC で GLB から作る
	const TCHAR* VehicleDir = TEXT("/Game/Rover/Vehicles");

	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Obj;
		if (!FFileHelper::LoadFileToString(Text, *Path) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Obj) || !Obj.IsValid())
		{
			UE_LOG(LogRoverScene, Error, TEXT("cannot read json: %s"), *Path);
			return nullptr;
		}
		return Obj;
	}

	FVector JsonVec3(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>& A = O->GetArrayField(Field);
		return FVector(A[0]->AsNumber(), A[1]->AsNumber(), A.Num() > 2 ? A[2]->AsNumber() : 0.0);
	}

	/** 16bit の灰色 PNG を読む（行 0 = 局所座標の +y 側）。 */
	bool LoadHeight16(const FString& Png, int32& OutW, int32& OutH, TArray<uint16>& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Png))
		{
			UE_LOG(LogRoverScene, Error, TEXT("cannot read %s"), *Png);
			return false;
		}
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Img = M.CreateImageWrapper(EImageFormat::PNG);
		if (!Img.IsValid() || !Img->SetCompressed(Bytes.GetData(), Bytes.Num()) || Img->GetBitDepth() != 16)
		{
			UE_LOG(LogRoverScene, Error, TEXT("not a 16-bit PNG: %s"), *Png);
			return false;
		}
		TArray64<uint8> Raw;
		if (!Img->GetRaw(ERGBFormat::Gray, 16, Raw))
		{
			return false;
		}
		OutW = Img->GetWidth();
		OutH = Img->GetHeight();
		Out.SetNumUninitialized(OutW * OutH);
		FMemory::Memcpy(Out.GetData(), Raw.GetData(), Out.Num() * sizeof(uint16));
		return true;
	}

	/** 色・粗さ・鏡面の定数だけの標準のマテリアル（あれば作り直す）。 */
	UMaterial* MakeLitMaterial(const FString& Name, const FLinearColor& Base, float Roughness, float Specular)
	{
		const FString PackagePath = FString(MaterialDir) / Name;
		UMaterial* M = LoadObject<UMaterial>(nullptr, *(PackagePath + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (M)
		{
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(M);
		}
		else
		{
			UPackage* Pkg = CreatePackage(*PackagePath);
			M = NewObject<UMaterial>(Pkg, *Name, RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(M);
		}
		auto* C = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant3Vector::StaticClass(), -300, 0));
		C->Constant = Base;
		UMaterialEditingLibrary::ConnectMaterialProperty(C, TEXT(""), MP_BaseColor);
		auto* R = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass(), -300, 150));
		R->R = Roughness;
		UMaterialEditingLibrary::ConnectMaterialProperty(R, TEXT(""), MP_Roughness);
		auto* S = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass(), -300, 250));
		S->R = Specular;
		UMaterialEditingLibrary::ConnectMaterialProperty(S, TEXT(""), MP_Specular);
		UMaterialEditingLibrary::RecompileMaterial(M);
		M->MarkPackageDirty();
		return M;
	}

	/** GLB を 1 つのスタティックメッシュとして取り込む（glTF 用の既定のパイプラインを写して、メッシュを結合する設定に）。 */
	UStaticMesh* ImportRoverMesh(const FString& Glb)
	{
		UInterchangeManager& IM = UInterchangeManager::GetInterchangeManager();
		// アセットの名前は元のファイル名から付くので、一般名 rover.glb に写してから取り込む
		const FString Staged = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RoverImport/rover.glb"));
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Staged), true) || IFileManager::Get().Copy(*Staged, *Glb) != COPY_OK)
		{
			UE_LOG(LogRoverScene, Error, TEXT("cannot stage %s"), *Glb);
			return nullptr;
		}
		UInterchangeSourceData* Src = UInterchangeManager::CreateSourceData(Staged);
		UInterchangeGenericAssetsPipeline* Base = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"));
		if (!Src || !Base)
		{
			UE_LOG(LogRoverScene, Error, TEXT("interchange: source or default glTF pipeline missing"));
			return nullptr;
		}
		UInterchangeGenericAssetsPipeline* Pipe = DuplicateObject(Base, GetTransientPackage());
		Pipe->MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
		FImportAssetParameters Params;
		Params.bIsAutomated = true;
		Params.bReplaceExisting = true;
		Params.OverridePipelines.Add(FSoftObjectPath(Pipe));
		Params.OverridePipelines.Add(FSoftObjectPath(TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline")));
		TArray<UObject*> Imported;
		if (!IM.ImportAsset(VehicleDir, Src, Params, Imported))
		{
			UE_LOG(LogRoverScene, Error, TEXT("interchange: import failed: %s"), *Glb);
			return nullptr;
		}
		for (UObject* O : Imported)
		{
			if (UStaticMesh* SM = Cast<UStaticMesh>(O))
			{
				return SM;
			}
		}
		UE_LOG(LogRoverScene, Error, TEXT("interchange: no static mesh among %d objects"), Imported.Num());
		return nullptr;
	}

	ACineCameraActor* AddShot(UWorld* World, const FString& Name, const FVector& Pos, const FVector& LookAt, float FocalMm)
	{
		ACineCameraActor* Cam = World->SpawnActor<ACineCameraActor>(Pos, (LookAt - Pos).Rotation());
		Cam->SetActorLabel(Name);
		Cam->Rename(*Name);
		Cam->Tags.Add(TEXT("RoverShot"));
		UCineCameraComponent* C = Cam->GetCineCameraComponent();
		C->Filmback.SensorWidth = 36.0f;
		C->Filmback.SensorHeight = 20.25f;   // 16:9
		C->SetCurrentFocalLength(FocalMm);
		C->CurrentAperture = RoverLunarLook::Fstop;
		C->FocusSettings.FocusMethod = ECameraFocusMethod::Disable;
		RoverLunarLook::ApplyExposure(C->PostProcessSettings);
		C->PostProcessBlendWeight = 1.0f;
		return Cam;
	}

	ADirectionalLight* AddDirectionalLight(UWorld* World, const FString& Name, const FVector& ToBodyUe, float Lux, float AngleDeg)
	{
		// ★ ADirectionalLight は生成時に光のコンポーネントへピッチ −46° を入れていて、スポーンの回転に足される。
		//   回転は生成のあとで直接決める（これを忘れて太陽の仰角が 1.8° でなく 47.8° になった）
		ADirectionalLight* L = World->SpawnActor<ADirectionalLight>();
		L->SetActorRotation((-ToBodyUe).Rotation());
		L->SetActorLabel(Name);
		UDirectionalLightComponent* C = CastChecked<UDirectionalLightComponent>(L->GetLightComponent());
		C->SetMobility(EComponentMobility::Movable);
		C->SetIntensity(Lux);
		C->SetLightSourceAngle(AngleDeg);
		C->SetAtmosphereSunLight(false);
		C->SetCastShadows(true);
		const double Elev = FMath::RadiansToDegrees(FMath::Asin(-C->GetDirection().Z));
		const double Want = FMath::RadiansToDegrees(FMath::Asin(ToBodyUe.Z));
		UE_LOG(LogRoverScene, Log, TEXT("light %s: source elevation %.3f deg (want %.3f)  %.2f lux"), *Name, Elev, Want, Lux);
		check(FMath::Abs(Elev - Want) < 0.01);
		return L;
	}
}

namespace RoverSceneBuilder
{
	bool Build(const FRoverSceneArgs& Args)
	{
		const TSharedPtr<FJsonObject> Grid = LoadJson(Args.Grid);
		const TSharedPtr<FJsonObject> Sun = LoadJson(Args.Sun);
		if (!Grid || !Sun || !GEditor)
		{
			return false;
		}
		const FString TileName = Grid->GetStringField(TEXT("name"));
		const TSharedPtr<FJsonObject> G = Grid->GetObjectField(TEXT("grid"));
		const TSharedPtr<FJsonObject> U = Grid->GetObjectField(TEXT("unreal"));
		const double Half = G->GetNumberField(TEXT("half_extent_m"));
		const double Res = G->GetNumberField(TEXT("resolution_m"));
		const double ZMin = G->GetNumberField(TEXT("z_local_min_m"));
		const double ZMax = G->GetNumberField(TEXT("z_local_max_m"));

		// --- 地形：16bit 高さ画像 ---
		int32 W = 0, H = 0;
		TArray<uint16> Src;
		if (!LoadHeight16(FPaths::GetPath(Args.Grid) / U->GetStringField(TEXT("file")), W, H, Src) || W != H || W != U->GetIntegerField(TEXT("points")))
		{
			UE_LOG(LogRoverScene, Error, TEXT("height image size mismatch (%dx%d)"), W, H);
			return false;
		}
		const int32 N = W;
		const int32 Center = (N - 1) / 2;
		UE_LOG(LogRoverScene, Log, TEXT("height16 %s: %dx%d  [0,0]=%u  center=%u  [N-1,N-1]=%u"),
			*TileName, W, H, Src[0], Src[Center * N + Center], Src[N * N - 1]);

		// 局所座標 (x, y) の高さ [m]（画像の双線形。カメラの高さとローバの傾きに使う）
		auto HeightAt = [&](double X, double Y) -> double
		{
			const double C = FMath::Clamp((X + Half) / Res, 0.0, N - 1.001);
			const double R = FMath::Clamp((Half - Y) / Res, 0.0, N - 1.001);
			const int32 C0 = FMath::FloorToInt(C), R0 = FMath::FloorToInt(R);
			const double Tx = C - C0, Ty = R - R0;
			auto Q = [&](int32 Rr, int32 Cc) { return double(Src[Rr * N + Cc]); };
			const double V = (1 - Tx) * (1 - Ty) * Q(R0, C0) + Tx * (1 - Ty) * Q(R0, C0 + 1) + (1 - Tx) * Ty * Q(R0 + 1, C0) + Tx * Ty * Q(R0 + 1, C0 + 1);
			return ZMin + V / 65535.0 * (ZMax - ZMin);
		};

		// ★ Landscape はヨー 90° で置く。四角形を割る対角線の向きを MuJoCo の HField とそろえるため
		//   （回さないと、局所座標で逆の対角線になり、急な所で物理と絵の地面が最大 0.35 m ずれた）。
		//   回転後の頂点 (I, J) → Unreal (X0 − J·res, Y0 + I·res) → 画像の (行 I, 列 N−1−J)。
		// 大きさは「63 の倍数 + 1」。足りない分は端の値を延ばす（DEM の外なので検査はしない）
		const int32 Comps = FMath::DivideAndRoundUp(N - 1, QuadsPerSection);
		const int32 Size = Comps * QuadsPerSection + 1;
		TArray<uint16> Heights;
		Heights.SetNumUninitialized(Size * Size);
		for (int32 J = 0; J < Size; ++J)
		{
			for (int32 I = 0; I < Size; ++I)
			{
				Heights[J * Size + I] = Src[FMath::Min(I, N - 1) * N + FMath::Max(N - 1 - J, 0)];
			}
		}

		UWorld* World = GEditor->NewMap(false);
		if (!World)
		{
			return false;
		}
		World->GetWorldSettings()->DefaultGameMode = ARoverViewGameMode::StaticClass();

		UMaterial* Regolith = MakeLitMaterial(TEXT("M_Regolith"), FLinearColor(0.12f, 0.12f, 0.12f), 1.0f, 0.3f);
		UMaterial* EarthMat = MakeLitMaterial(TEXT("M_Earth"), FLinearColor(0.22f, 0.30f, 0.45f), 0.6f, 0.5f);

		// 頂点 (0, 0) = 局所 (+half, +half) = Unreal (+half, −half)
		const FVector LandLoc(Half * 100.0, -Half * 100.0, U->GetNumberField(TEXT("location_z_cm")));
		const FVector LandScale(U->GetNumberField(TEXT("xy_scale_cm")), U->GetNumberField(TEXT("xy_scale_cm")), U->GetNumberField(TEXT("z_scale_percent")));
		ALandscape* Land = World->SpawnActor<ALandscape>(LandLoc, FRotator(0.0, 90.0, 0.0));
		Land->LandscapeMaterial = Regolith;
		Land->SetActorRelativeScale3D(LandScale);
		Land->StaticLightingLOD = 0;
		TMap<FGuid, TArray<uint16>> HeightPerLayer;
		HeightPerLayer.Add(FGuid(), Heights);
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialPerLayer;
		MaterialPerLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Land->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, 1, QuadsPerSection, HeightPerLayer, nullptr, MaterialPerLayer,
			ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
		Land->SetActorLabel(FString::Printf(TEXT("Terrain_%s"), *TileName));
		if (ULandscapeInfo* Info = Land->GetLandscapeInfo())
		{
			Info->UpdateLayerInfoMap(Land);
		}
		UE_LOG(LogRoverScene, Log, TEXT("landscape %dx%d (%d comps)  loc=%s scale=%s"), Size, Size, Comps * Comps, *LandLoc.ToString(), *LandScale.ToString());

		// --- 光：太陽・地球・地球照（向きは局所座標 → Unreal） ---
		const TSharedPtr<FJsonObject> SunO = Sun->GetObjectField(TEXT("sun"));
		const TSharedPtr<FJsonObject> EarthO = Sun->GetObjectField(TEXT("earth"));
		const FVector Sd = JsonVec3(SunO, TEXT("dir_local"));
		const FVector Ed = JsonVec3(EarthO, TEXT("dir_local"));
		const FVector SunUe = RoverFrame::LocalDirToUnreal(Sd.X, Sd.Y, Sd.Z);
		const FVector EarthUe = RoverFrame::LocalDirToUnreal(Ed.X, Ed.Y, Ed.Z);
		AddDirectionalLight(World, TEXT("Sun"), SunUe, SunO->GetNumberField(TEXT("illuminance_lux")), SunO->GetNumberField(TEXT("angular_diameter_deg")));
		ADirectionalLight* Shine = AddDirectionalLight(World, TEXT("Earthshine"), EarthUe, EarthO->GetNumberField(TEXT("earthshine_lux")), EarthO->GetNumberField(TEXT("angular_diameter_deg")));
		Shine->GetLightComponent()->SetAffectTranslucentLighting(false);

		const double EarthAng = FMath::DegreesToRadians(EarthO->GetNumberField(TEXT("angular_diameter_deg")));
		AStaticMeshActor* Earth = World->SpawnActor<AStaticMeshActor>(EarthUe * EarthDistanceCm, FRotator::ZeroRotator);
		Earth->SetActorLabel(TEXT("Earth"));
		UStaticMeshComponent* EarthC = Earth->GetStaticMeshComponent();
		EarthC->SetMobility(EComponentMobility::Movable);
		EarthC->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		EarthC->SetMaterial(0, EarthMat);
		EarthC->SetCastShadow(false);
		EarthC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Earth->SetActorScale3D(FVector(2.0 * EarthDistanceCm * FMath::Sin(EarthAng / 2.0) / 100.0));   // 球のメッシュは直径 100 cm

		// --- 写り：自動露出なし・空の光なし ---
		APostProcessVolume* Ppv = World->SpawnActor<APostProcessVolume>();
		Ppv->SetActorLabel(TEXT("LunarExposure"));
		Ppv->bUnbound = true;
		RoverLunarLook::ApplyExposure(Ppv->Settings);

		// --- ローバ：原点に、地形の傾きに合わせ、地球の方位を向けて置く ---
		const double Dz = 5.0;
		const FVector NormalLocal = FVector(-(HeightAt(Dz, 0) - HeightAt(-Dz, 0)) / (2 * Dz), -(HeightAt(0, Dz) - HeightAt(0, -Dz)) / (2 * Dz), 1.0).GetSafeNormal();
		const FVector NormalUe = RoverFrame::LocalDirToUnreal(NormalLocal.X, NormalLocal.Y, NormalLocal.Z);
		const FVector Heading = FVector(EarthUe.X, EarthUe.Y, 0.0).GetSafeNormal();
		const FVector RoverPos = RoverFrame::LocalToUnreal(0.0, 0.0, HeightAt(0.0, 0.0));
		if (!Args.Glb.IsEmpty())
		{
			if (UStaticMesh* Mesh = ImportRoverMesh(Args.Glb))
			{
				// 取り込んだメッシュの前を +X にそろえる：外形の中心は後ろ寄り（glTF で +0.57 m）なので、中心が −X 側に来る回転を選ぶ
				const FBox Box = Mesh->GetBoundingBox();
				const FVector C = Box.GetCenter();
				const double YawFix = FMath::Abs(FVector2D(C.X, C.Y).Size()) > 10.0 ? 180.0 - FMath::RadiansToDegrees(FMath::Atan2(C.Y, C.X)) : 0.0;
				UE_LOG(LogRoverScene, Log, TEXT("rover mesh bounds min=%s max=%s center=%s -> yaw fix %.1f deg"), *Box.Min.ToString(), *Box.Max.ToString(), *C.ToString(), YawFix);
				AStaticMeshActor* Rover = World->SpawnActor<AStaticMeshActor>(RoverPos, FRotationMatrix::MakeFromZX(NormalUe, Heading).Rotator());
				Rover->SetActorLabel(TEXT("Rover"));
				UStaticMeshComponent* RC = Rover->GetStaticMeshComponent();
				RC->SetMobility(EComponentMobility::Movable);
				RC->SetStaticMesh(Mesh);
				const FRotator Fix(0.0, YawFix, 0.0);
				RC->SetRelativeRotation(Fix);
				RC->SetRelativeLocation(FVector(0.0, 0.0, -Box.Min.Z));   // 車輪の下端を地面に
			}
		}

		// --- カメラ（局所座標で決めて、地面からの高さを足す） ---
		const FVector2D S2 = FVector2D(Sd.X, Sd.Y).GetSafeNormal();           // 太陽の方位（局所）
		const FVector2D H2 = FVector2D(Ed.X, Ed.Y).GetSafeNormal();           // 進行方向 = 地球の方位
		const FVector2D R2(H2.Y, -H2.X);                                       // 右（上から見て時計回り 90°）
		auto At = [&](const FVector2D& P, double AboveGround) { return RoverFrame::LocalToUnreal(P.X, P.Y, HeightAt(P.X, P.Y) + AboveGround); };
		auto Rot = [](const FVector2D& V, double Deg) { const double A = FMath::DegreesToRadians(Deg); return FVector2D(V.X * FMath::Cos(A) - V.Y * FMath::Sin(A), V.X * FMath::Sin(A) + V.Y * FMath::Cos(A)); };
		const FVector RoverMid = RoverPos + FVector(0, 0, 200.0);
		{
			// 01：後ろ右から、ローバと地平線の地球（地球の方位から少し左を向く）
			const FVector2D P = -H2 * 24.0 + R2 * 10.0;
			const FVector Pos = At(P, 2.5);
			const FVector Aim = Pos + RoverFrame::LocalDirToUnreal(Rot(H2, 11.0).X, Rot(H2, 11.0).Y, FMath::Tan(FMath::DegreesToRadians(1.0)));
			AddShot(World, TEXT("Shot_01_Earth"), Pos, Aim, 24.0f);
		}
		{
			// 02：太陽を横から受ける 3/4 の構図（長い影）
			const FVector2D P = Rot(S2, 110.0) * 18.0;
			AddShot(World, TEXT("Shot_02_Side"), At(P, 2.0), RoverMid, 24.0f);
		}
		{
			// 03：太陽に向かって（逆光・パネルの縁が光る）
			const FVector2D P = -S2 * 20.0 + Rot(S2, 90.0) * 4.0;
			AddShot(World, TEXT("Shot_03_Backlit"), At(P, 1.8), RoverMid, 24.0f);
		}
		{
			// 04：上空から斜めに（地形の起伏と影の伸び）
			const FVector2D P = Rot(S2, 90.0) * 400.0;
			AddShot(World, TEXT("Shot_04_Overview"), At(P, 150.0), RoverPos, 35.0f);
		}

		ARoverShotDirector* Director = World->SpawnActor<ARoverShotDirector>();
		Director->SetActorLabel(TEXT("RoverShotDirector"));
		UE_LOG(LogRoverScene, Log, TEXT("scene built: sun el %.2f az %.1f (%.0f lux)  earth el %.2f az %.1f"),
			SunO->GetNumberField(TEXT("elevation_deg")), SunO->GetNumberField(TEXT("azimuth_deg")), SunO->GetNumberField(TEXT("illuminance_lux")),
			EarthO->GetNumberField(TEXT("elevation_deg")), EarthO->GetNumberField(TEXT("azimuth_deg")));
		return true;
	}

	bool Save(const FRoverSceneArgs& Args)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return false;
		}
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			It->ForceUpdateLayersContent();
		}
		const bool bMap = UEditorLoadingAndSavingUtils::SaveMap(World, Args.Map);
		const bool bRest = UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
		UE_LOG(LogRoverScene, Log, TEXT("saved map %s (%d) and content (%d)"), *Args.Map, bMap, bRest);
		return bMap && bRest;
	}
}
