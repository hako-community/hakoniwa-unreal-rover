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
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RoverFrame.h"
#include "RoverAvatar.h"
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

	// Hapke（1981）の双方向反射率を、標準の拡散（BaseColor/π·E·μ0）の BaseColor に直したもの：
	//   r = w/(4π)·μ0/(μ0+μ)·[(1+B(g))·P(g) + H(μ0)·H(μ) − 1] → BaseColor = π·r/μ0 = w/4/(μ0+μ)·[…]
	//   B(g) = B0/(1+tan(g/2)/h)（衝効果）、P(g) は 2 項 HG（b・c）、H(x) = (1+2x)/(1+2xγ)・γ = √(1−w)
	//   定数は検討書 §3.7（南極域＝高地：w 0.32・B0 1.0・h 0.06・b 0.21・c 0.70）。μ0・μ・g は面の法線（頂点）・視線・太陽で決める。
	//   明るさのむら（±15%）を 2 つの大きさの値ノイズで掛ける。μ0 は標準の拡散が掛けるので、ここでは割った形にしてある
	const TCHAR* HapkeHlsl = TEXT(R"(
float3 n = normalize(N); float3 v = normalize(V); float3 s = normalize(S.xyz);
float mu0 = max(dot(n, s), 1e-3); float mu = max(dot(n, v), 1e-3);
float cg = clamp(dot(s, v), -1.0, 1.0); float g = acos(cg);
const float w = 0.32, B0 = 1.0, hh = 0.06, b = 0.21, c = 0.70;
float B = B0 / (1.0 + tan(min(g, 3.1) * 0.5) / hh);
float b2 = b * b;
float P = 0.5 * (1.0 + c) * (1.0 - b2) / pow(1.0 - 2.0 * b * cg + b2, 1.5)
        + 0.5 * (1.0 - c) * (1.0 - b2) / pow(1.0 + 2.0 * b * cg + b2, 1.5);
float gam = sqrt(1.0 - w);
float H0 = (1.0 + 2.0 * mu0) / (1.0 + 2.0 * mu0 * gam);
float H1 = (1.0 + 2.0 * mu) / (1.0 + 2.0 * mu * gam);
float bc = w * 0.25 / (mu0 + mu) * ((1.0 + B) * P + H0 * H1 - 1.0);
float tint = 1.0;
for (int o = 0; o < 2; o++) {
  float sc = (o == 0) ? 4000.0 : 300.0;
  float2 x = Pos.xy / sc; float2 i = floor(x); float2 f = x - i; float2 u = f * f * (3.0 - 2.0 * f);
  float a0 = frac(sin(dot(fmod(i, 1024.0), float2(127.1, 311.7))) * 43758.5453);
  float a1 = frac(sin(dot(fmod(i + float2(1, 0), 1024.0), float2(127.1, 311.7))) * 43758.5453);
  float a2 = frac(sin(dot(fmod(i + float2(0, 1), 1024.0), float2(127.1, 311.7))) * 43758.5453);
  float a3 = frac(sin(dot(fmod(i + float2(1, 1), 1024.0), float2(127.1, 311.7))) * 43758.5453);
  float nv = lerp(lerp(a0, a1, u.x), lerp(a2, a3, u.x), u.y);
  tint *= 1.0 + 0.15 * (nv - 0.5) * ((o == 0) ? 1.2 : 0.8);
}
return saturate(bc * tint).xxx;
)");

	// 地面の細かい凹凸（DEM の 5 m 格子より小さいもの）：5 段の勾配ノイズ（導関数つき）の傾きで法線を傾ける（接空間 ≒ 世界の XY）。
	//   大きさ 6〜490 cm・高さ 0.6〜23 cm。段ごとに座標を 36.9° 回して格子の向きをそろえない。
	//   画面上で細かすぎる段（大きさの 400 倍より遠い）は弱める（遠景のちらつき止め）
	const TCHAR* DetailNormalHlsl = TEXT(R"(
float2 gr = float2(0, 0);
float2 p = Pos.xy;
float2x2 rot = float2x2(0.8, -0.6, 0.6, 0.8);
float2x2 acc = float2x2(1, 0, 0, 1);
for (int o = 0; o < 5; o++) {
  float sc = 6.0 * pow(3.0, o);
  float am = 0.6 * pow(2.5, o);
  float fade = saturate(2.0 - Depth / (sc * 400.0));
  acc = mul(rot, acc);
  float2 q = mul(acc, p) / sc;
  float2 i = floor(q); float2 f = q - i;
  float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  float2 i0 = fmod(i, 1024.0); float2 i1 = fmod(i + float2(1, 0), 1024.0); float2 i2 = fmod(i + float2(0, 1), 1024.0); float2 i3 = fmod(i + float2(1, 1), 1024.0);
  float2 ga = -1.0 + 2.0 * frac(sin(float2(dot(i0, float2(127.1, 311.7)), dot(i0, float2(269.5, 183.3)))) * 43758.5453);
  float2 gb = -1.0 + 2.0 * frac(sin(float2(dot(i1, float2(127.1, 311.7)), dot(i1, float2(269.5, 183.3)))) * 43758.5453);
  float2 gc = -1.0 + 2.0 * frac(sin(float2(dot(i2, float2(127.1, 311.7)), dot(i2, float2(269.5, 183.3)))) * 43758.5453);
  float2 gd = -1.0 + 2.0 * frac(sin(float2(dot(i3, float2(127.1, 311.7)), dot(i3, float2(269.5, 183.3)))) * 43758.5453);
  float va = dot(ga, f); float vb = dot(gb, f - float2(1, 0)); float vc = dot(gc, f - float2(0, 1)); float vd = dot(gd, f - float2(1, 1));
  float k = va - vb - vc + vd;
  float2 dq = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) + du * (u.yx * k + float2(vb, vc) - va);
  gr += fade * am / sc * mul(transpose(acc), dq);
}
return normalize(float3(-gr.x, -gr.y, 1.0));
)");

	UMaterialExpressionCustom* AddCustom(UMaterial* M, const TCHAR* Code, ECustomMaterialOutputType Out, const TArray<TPair<FName, UMaterialExpression*>>& In, int32 Y)
	{
		auto* C = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCustom::StaticClass(), -400, Y));
		C->Code = Code;
		C->OutputType = Out;
		C->Inputs.Reset();
		for (const TPair<FName, UMaterialExpression*>& P : In)
		{
			FCustomInput& I = C->Inputs.AddDefaulted_GetRef();
			I.InputName = P.Key;
			I.Input.Connect(0, P.Value);
		}
		return C;
	}

	/** 太陽の向き（世界・天体へ向かう単位ベクトル）を入れるパラメータコレクション。レゴリスのマテリアルが読む */
	UMaterialParameterCollection* MakeLunarCollection(const FVector& SunUe)
	{
		const FString Name = TEXT("MPC_Lunar");
		const FString PackagePath = FString(MaterialDir) / Name;
		UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, *(PackagePath + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Mpc)
		{
			UPackage* Pkg = CreatePackage(*PackagePath);
			Mpc = NewObject<UMaterialParameterCollection>(Pkg, *Name, RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Mpc);
		}
		Mpc->VectorParameters.Reset();
		FCollectionVectorParameter& P = Mpc->VectorParameters.AddDefaulted_GetRef();
		P.ParameterName = TEXT("SunDir");
		P.DefaultValue = FLinearColor(SunUe.X, SunUe.Y, SunUe.Z, 0.0f);
		Mpc->PostEditChange();
		Mpc->MarkPackageDirty();
		return Mpc;
	}

	/** レゴリス：Hapke の反射（BaseColor）＋細かい凹凸（Normal）。鏡面は Hapke に含めるので 0 */
	UMaterial* MakeRegolithMaterial(UMaterialParameterCollection* Mpc)
	{
		const FString Name = TEXT("M_Regolith");
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
		auto* N = UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionVertexNormalWS::StaticClass(), -800, 0);
		auto* V = UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCameraVectorWS::StaticClass(), -800, 100);
		auto* Pos = UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionWorldPosition::StaticClass(), -800, 300);
		auto* S = Cast<UMaterialExpressionCollectionParameter>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCollectionParameter::StaticClass(), -800, 200));
		S->Collection = Mpc;
		S->ParameterName = TEXT("SunDir");
		S->ParameterId = Mpc->GetParameterId(S->ParameterName);
		UMaterialExpressionCustom* Hapke = AddCustom(M, HapkeHlsl, CMOT_Float3, { {TEXT("N"), N}, {TEXT("V"), V}, {TEXT("S"), S}, {TEXT("Pos"), Pos} }, 0);
		Hapke->Description = TEXT("Hapke");
		UMaterialEditingLibrary::ConnectMaterialProperty(Hapke, TEXT(""), MP_BaseColor);
		auto* Depth = UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionPixelDepth::StaticClass(), -800, 400);
		UMaterialExpressionCustom* Bump = AddCustom(M, DetailNormalHlsl, CMOT_Float3, { {TEXT("Pos"), Pos}, {TEXT("Depth"), Depth} }, 300);
		Bump->Description = TEXT("DetailNormal");
		UMaterialEditingLibrary::ConnectMaterialProperty(Bump, TEXT(""), MP_Normal);
		auto* R = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass(), -400, 500));
		R->R = 1.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(R, TEXT(""), MP_Roughness);
		auto* Sp = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass(), -400, 600));
		Sp->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Sp, TEXT(""), MP_Specular);
		const TArray<FString> Errors = UMaterialEditingLibrary::RecompileMaterial(M);
		for (const FString& E : Errors)
		{
			UE_LOG(LogRoverScene, Error, TEXT("M_Regolith: %s"), *E);
		}
		M->MarkPackageDirty();
		return M;
	}

	/** GLB を取り込む（glTF 用の既定のパイプラインを写して使う）。bCombine なら 1 つに結合、でなければ部品ごと（モデルの座標に焼き込み）。
	 *  戻り値はメッシュの名前 → スタティックメッシュ */
	TMap<FString, UStaticMesh*> ImportRoverMeshes(const FString& Glb, bool bCombine)
	{
		TMap<FString, UStaticMesh*> Out;
		UInterchangeManager& IM = UInterchangeManager::GetInterchangeManager();
		// アセットの名前は元のファイル名から付くので、一般名 rover.glb に写してから取り込む
		const FString Staged = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RoverImport/rover.glb"));
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Staged), true) || IFileManager::Get().Copy(*Staged, *Glb) != COPY_OK)
		{
			UE_LOG(LogRoverScene, Error, TEXT("cannot stage %s"), *Glb);
			return Out;
		}
		UInterchangeSourceData* Src = UInterchangeManager::CreateSourceData(Staged);
		UInterchangeGenericAssetsPipeline* Base = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"));
		if (!Src || !Base)
		{
			UE_LOG(LogRoverScene, Error, TEXT("interchange: source or default glTF pipeline missing"));
			return Out;
		}
		UInterchangeGenericAssetsPipeline* Pipe = DuplicateObject(Base, GetTransientPackage());
		Pipe->MeshPipeline->CombineStaticMeshesBehavior = bCombine ? EInterchangeCombineStaticMeshesBehavior::All : EInterchangeCombineStaticMeshesBehavior::DoNotCombine;
		Pipe->CommonMeshesProperties->bBakeMeshes = true;   // 部品の位置はモデルの座標に焼き込む（関節の軸まわりの回転は部品の木で付ける）
		FImportAssetParameters Params;
		Params.bIsAutomated = true;
		Params.bReplaceExisting = true;
		Params.OverridePipelines.Add(FSoftObjectPath(Pipe));
		Params.OverridePipelines.Add(FSoftObjectPath(TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline")));
		TArray<UObject*> Imported;
		if (!IM.ImportAsset(VehicleDir, Src, Params, Imported))
		{
			UE_LOG(LogRoverScene, Error, TEXT("interchange: import failed: %s"), *Glb);
			return Out;
		}
		for (UObject* O : Imported)
		{
			if (UStaticMesh* SM = Cast<UStaticMesh>(O))
			{
				FString N = SM->GetName();
				N.RemoveFromStart(TEXT("SM_"));
				Out.Add(N, SM);
			}
		}
		UE_LOG(LogRoverScene, Log, TEXT("interchange: %d static meshes (combine=%d)"), Out.Num(), bCombine);
		return Out;
	}

	/** 部品の木のローバ（ARoverAvatar）を組む。viewer_sync.json（hakoniwa-mujoco-rover の tools/viewer_sync.py）が契約。
	 *  アクタ = 車体（chassis）の座標。Body = GLB の原点（model_offset）。隅ごとに 懸架（直動）→ 操舵 → 車輪 の部品。
	 *  部品のメッシュはモデルの座標に焼き込んであるので、関節の部品の下では「−取付位置」だけずらして置く */
	ARoverAvatar* BuildRoverAvatar(UWorld* World, const FString& Glb, const TSharedPtr<FJsonObject>& Sync, const FVector& GroundPos, const FQuat& BodyQ)
	{
		const TMap<FString, UStaticMesh*> Meshes = ImportRoverMeshes(Glb, false);
		const FVector Off = JsonVec3(Sync, TEXT("model_offset_m"));
		const FVector OffUe = RoverFrame::LocalToUnreal(Off.X, Off.Y, Off.Z);
		ARoverAvatar* A = World->SpawnActor<ARoverAvatar>(GroundPos - BodyQ.RotateVector(OffUe), BodyQ.Rotator());
		A->SetActorLabel(TEXT("Rover"));
		A->Body->SetRelativeLocation(OffUe);
		A->RobotName = Sync->GetStringField(TEXT("robot_name"));
		const TSharedPtr<FJsonObject> Pdu = Sync->GetObjectField(TEXT("pdu"));
		A->BaseChannel = Pdu->GetObjectField(TEXT("base"))->GetIntegerField(TEXT("channel_id"));
		A->BaseSize = Pdu->GetObjectField(TEXT("base"))->GetIntegerField(TEXT("pdu_size"));
		A->JointChannel = Pdu->GetObjectField(TEXT("joints"))->GetIntegerField(TEXT("channel_id"));
		A->JointSize = Pdu->GetObjectField(TEXT("joints"))->GetIntegerField(TEXT("pdu_size"));

		auto AddScene = [A](const FString& Name, USceneComponent* Parent, const FVector& Loc)
		{
			USceneComponent* C = NewObject<USceneComponent>(A, *Name, RF_Transactional);
			C->SetMobility(EComponentMobility::Movable);
			C->SetupAttachment(Parent);
			C->SetRelativeLocation(Loc);
			A->AddInstanceComponent(C);
			C->RegisterComponent();
			return C;
		};
		TMap<FString, TPair<USceneComponent*, FVector>> WheelComp;
		for (const TSharedPtr<FJsonValue>& V : Sync->GetArrayField(TEXT("corners")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			const FVector Pm = JsonVec3(O, TEXT("pivot_m"));
			FRoverCorner C;
			C.SuspJoint = O->GetStringField(TEXT("susp"));
			C.WheelJoint = O->GetStringField(TEXT("wheel"));
			O->TryGetStringField(TEXT("steer"), C.SteerJoint);
			C.RestPivotCm = RoverFrame::LocalToUnreal(Pm.X, Pm.Y, Pm.Z);
			C.Susp = AddScene(C.SuspJoint, A->GetRootComponent(), C.RestPivotCm);
			C.Steer = C.SteerJoint.IsEmpty() ? nullptr : AddScene(C.SteerJoint, C.Susp, FVector::ZeroVector);
			C.Wheel = AddScene(C.WheelJoint, C.Steer ? C.Steer.Get() : C.Susp.Get(), FVector::ZeroVector);
			WheelComp.Add(C.WheelJoint, TPair<USceneComponent*, FVector>(C.Wheel, C.RestPivotCm));
			A->Corners.Add(C);
		}

		// 取り込みの座標（X = glTF x・Y = glTF z・Z = glTF y）→ 体の座標（Unreal）はヨー +90°（glTF の前 −z が +X に来る）
		const FQuat Fix = FRotator(0.0, 90.0, 0.0).Quaternion();
		int32 Placed = 0;
		FBox BodyBox(ForceInit);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& P : Sync->GetObjectField(TEXT("parts"))->Values)
		{
			UStaticMesh* const* SM = Meshes.Find(P.Key);
			if (!SM)
			{
				UE_LOG(LogRoverScene, Warning, TEXT("rover part %s: no imported mesh"), *P.Key);
				continue;
			}
			const FString Owner = P.Value->AsString();
			const TPair<USceneComponent*, FVector>* W = WheelComp.Find(Owner);
			USceneComponent* Parent = W ? W->Key : A->Body.Get();
			const FVector Loc = W ? OffUe - W->Value : FVector::ZeroVector;
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A, *(TEXT("Part_") + P.Key), RF_Transactional);
			C->SetMobility(EComponentMobility::Movable);
			C->SetStaticMesh(*SM);
			C->SetupAttachment(Parent);
			C->SetRelativeLocationAndRotation(Loc, Fix);
			A->AddInstanceComponent(C);
			C->RegisterComponent();
			if (!W)
			{
				BodyBox += (*SM)->GetBoundingBox().TransformBy(C->GetRelativeTransform() * FTransform(OffUe));
			}
			++Placed;
		}
		// 確認：車体の外形の中心は後ろ寄り（体の座標で x < 0）・下端は GLB の原点（地面）
		UE_LOG(LogRoverScene, Log, TEXT("rover avatar: %d parts, %d corners, body box (actor frame) min=%s max=%s  robot=%s base ch%d joints ch%d"),
			Placed, A->Corners.Num(), *BodyBox.Min.ToString(), *BodyBox.Max.ToString(), *A->RobotName, A->BaseChannel, A->JointChannel);
		return A;
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

		// 太陽・地球の向き（局所座標 → Unreal）。レゴリスのマテリアルも太陽の向きを使う
		const TSharedPtr<FJsonObject> SunO = Sun->GetObjectField(TEXT("sun"));
		const TSharedPtr<FJsonObject> EarthO = Sun->GetObjectField(TEXT("earth"));
		const FVector Sd = JsonVec3(SunO, TEXT("dir_local"));
		const FVector Ed = JsonVec3(EarthO, TEXT("dir_local"));
		const FVector SunUe = RoverFrame::LocalDirToUnreal(Sd.X, Sd.Y, Sd.Z);
		const FVector EarthUe = RoverFrame::LocalDirToUnreal(Ed.X, Ed.Y, Ed.Z);

		UMaterial* Regolith = MakeRegolithMaterial(MakeLunarCollection(SunUe));
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

		// --- 光：太陽・地球・地球照 ---
		ADirectionalLight* SunL = AddDirectionalLight(World, TEXT("Sun"), SunUe, SunO->GetNumberField(TEXT("illuminance_lux")), SunO->GetNumberField(TEXT("angular_diameter_deg")));
		// 半透明・霧などに使う主の平行光源は太陽（決めないと 2 つが競合するという警告が画面に出る）
		CastChecked<UDirectionalLightComponent>(SunL->GetLightComponent())->SetForwardShadingPriority(1);
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
		FVector Heading = FVector(EarthUe.X, EarthUe.Y, 0.0).GetSafeNormal();
		// 参照カメラ（-RoverRefCams・-RoverRef）：その構図で、太陽がカメラ側から 35° 左に寄って差すようにローバの向きを決める
		TSharedPtr<FJsonObject> RefCam;
		if (!Args.RefCams.IsEmpty() && !Args.RefKey.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Refs = LoadJson(Args.RefCams);
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Refs || !Refs->GetObjectField(TEXT("cameras"))->TryGetObjectField(Args.RefKey, Found))
			{
				UE_LOG(LogRoverScene, Error, TEXT("reference camera %s not found in %s"), *Args.RefKey, *Args.RefCams);
				return false;
			}
			RefCam = *Found;
			const FVector Cp = JsonVec3(RefCam, TEXT("position_m"));
			const double Psi = FMath::Atan2(Sd.Y, Sd.X) - FMath::Atan2(Cp.Y, Cp.X) - FMath::DegreesToRadians(35.0);
			Heading = RoverFrame::LocalDirToUnreal(FMath::Cos(Psi), FMath::Sin(Psi), 0.0);
			UE_LOG(LogRoverScene, Log, TEXT("reference %s: rover yaw %.1f deg (local)"), *Args.RefKey, FMath::RadiansToDegrees(Psi));
		}
		const FVector RoverPos = RoverFrame::LocalToUnreal(0.0, 0.0, HeightAt(0.0, 0.0));
		AActor* Rover = nullptr;
		const FQuat BodyQ = FRotationMatrix::MakeFromZX(NormalUe, Heading).ToQuat();
		const TSharedPtr<FJsonObject> Sync = Args.ViewerSync.IsEmpty() ? nullptr : LoadJson(Args.ViewerSync);
		if (!Args.Glb.IsEmpty() && Sync)
		{
			// 部品の木のローバ（ROV-22。PLANT の走行を写せる）。静止時は GLB の原点（車輪の下端）を地面に置く
			Rover = BuildRoverAvatar(World, Args.Glb, Sync, RoverPos, BodyQ);
		}
		else if (!Args.Glb.IsEmpty())
		{
			const TMap<FString, UStaticMesh*> Meshes = ImportRoverMeshes(Args.Glb, true);
			if (Meshes.Num() > 0)
			{
				UStaticMesh* Mesh = Meshes.CreateConstIterator().Value();
				// 取り込んだメッシュの前を +X にそろえる：外形の中心は後ろ寄り（glTF で +0.57 m）なので、中心が −X 側に来る回転を選ぶ
				const FBox Box = Mesh->GetBoundingBox();
				const FVector C = Box.GetCenter();
				const double YawFix = FMath::Abs(FVector2D(C.X, C.Y).Size()) > 10.0 ? 180.0 - FMath::RadiansToDegrees(FMath::Atan2(C.Y, C.X)) : 0.0;
				// ★ AStaticMeshActor の根はメッシュのコンポーネントそのもの。メッシュの向きの補正を相対回転で入れると、
				//   アクタの向き（方位・地形の傾き）を上書きしてしまう（2026-10-18 に発覚）。体の向き × 補正 を 1 つにして置く
				const FQuat MeshQ = BodyQ * FRotator(0.0, YawFix, 0.0).Quaternion();
				AStaticMeshActor* Static = World->SpawnActor<AStaticMeshActor>(RoverPos + BodyQ.RotateVector(FVector(0.0, 0.0, -Box.Min.Z)), MeshQ.Rotator());
				Static->SetActorLabel(TEXT("Rover"));
				Static->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Static->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Rover = Static;
			}
		}

		// --- カメラ（局所座標で決めて、地面からの高さを足す） ---
		const FVector2D S2 = FVector2D(Sd.X, Sd.Y).GetSafeNormal();           // 太陽の方位（局所）
		const FVector2D H2 = FVector2D(Ed.X, Ed.Y).GetSafeNormal();           // 進行方向 = 地球の方位
		const FVector2D R2(H2.Y, -H2.X);                                       // 右（上から見て時計回り 90°）
		auto At = [&](const FVector2D& P, double AboveGround) { return RoverFrame::LocalToUnreal(P.X, P.Y, HeightAt(P.X, P.Y) + AboveGround); };
		auto Rot = [](const FVector2D& V, double Deg) { const double A = FMath::DegreesToRadians(Deg); return FVector2D(V.X * FMath::Cos(A) - V.Y * FMath::Sin(A), V.X * FMath::Sin(A) + V.Y * FMath::Cos(A)); };
		const FVector RoverMid = RoverPos + FVector(0, 0, 200.0);
		if (RefCam)
		{
			// 参照カメラ：ローバの体の座標（m・前・左・上）→ ローバのアクタの座標 → 世界。画角と縦横比は元の絵に合わせる
			const FTransform Body(FRotationMatrix::MakeFromZX(NormalUe, Heading).Rotator(), RoverPos);   // 体の座標（メッシュの補正を含まない）
			const FVector P = JsonVec3(RefCam, TEXT("position_m"));
			const FVector F = JsonVec3(RefCam, TEXT("forward"));
			const FVector Uc = JsonVec3(RefCam, TEXT("up"));
			const FVector Pos = Body.TransformPosition(RoverFrame::LocalToUnreal(P.X, P.Y, P.Z));
			const FVector Fw = Body.TransformVector(RoverFrame::LocalDirToUnreal(F.X, F.Y, F.Z));
			const FVector Up = Body.TransformVector(RoverFrame::LocalDirToUnreal(Uc.X, Uc.Y, Uc.Z));
			const TArray<TSharedPtr<FJsonValue>>& Sz = RefCam->GetArrayField(TEXT("size_px"));
			const double Aspect = Sz[0]->AsNumber() / Sz[1]->AsNumber();
			const double Hfov = FMath::DegreesToRadians(RefCam->GetNumberField(TEXT("hfov_deg")));
			ACineCameraActor* Cam = AddShot(World, FString::Printf(TEXT("Shot_Ref%s"), *Args.RefKey), Pos, Pos + Fw * 100.0, 24.0f);
			Cam->SetActorRotation(FRotationMatrix::MakeFromXZ(Fw, Up).Rotator());
			UCineCameraComponent* C = Cam->GetCineCameraComponent();
			C->Filmback.SensorWidth = 36.0f;
			C->Filmback.SensorHeight = 36.0f / Aspect;
			C->SetCurrentFocalLength(18.0 / FMath::Tan(Hfov / 2.0));
			const double Gz = HeightAt(RoverFrame::UnrealToLocal(Pos).X, RoverFrame::UnrealToLocal(Pos).Y) * 100.0;
			UE_LOG(LogRoverScene, Log, TEXT("reference camera %s: %.2f m above the terrain, focal %.1f mm, aspect %.3f"),
				*Args.RefKey, (Pos.Z - Gz) / 100.0, C->CurrentFocalLength, Aspect);
		}
		else
		{
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
