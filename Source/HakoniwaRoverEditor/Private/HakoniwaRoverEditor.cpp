// HakoniwaRoverEditor.cpp
// 場面を組む入口（ROV-15）。
//   自動：UnrealEditor.exe <uproject> -RoverBuildScene -RoverGrid=<grid.json> -RoverSun=<sun.json> [-RoverGlb=<glb>] [-RoverMap=/Game/...] [-RoverQuit]
//         エディタが立ち上がってから組み、Landscape の更新を数フレーム待って保存する（-RoverQuit で閉じる）。
//   手で：エディタのコンソールで  Rover.BuildScene -RoverGrid=... -RoverSun=... [-RoverGlb=...]

#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "RoverSceneBuilder.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoverEditor, Log, All);

namespace
{
	FRoverSceneArgs ParseArgs(const TCHAR* Cmd)
	{
		FRoverSceneArgs A;
		FParse::Value(Cmd, TEXT("-RoverGrid="), A.Grid);
		FParse::Value(Cmd, TEXT("-RoverSun="), A.Sun);
		FParse::Value(Cmd, TEXT("-RoverGlb="), A.Glb);
		FParse::Value(Cmd, TEXT("-RoverMap="), A.Map);
		FParse::Value(Cmd, TEXT("-RoverRefCams="), A.RefCams);
		FParse::Value(Cmd, TEXT("-RoverViewerSync="), A.ViewerSync);
		FParse::Value(Cmd, TEXT("-RoverRef="), A.RefKey);
		return A;
	}

	/** 組む → 数十フレーム待つ（Landscape の編集レイヤーとシェーダ）→ 保存 →（必要なら）終了。 */
	void RunScheduled(const FRoverSceneArgs& Args, bool bQuit, int32 StartDelayFrames)
	{
		struct FState { int32 Frame = 0; bool bBuilt = false; bool bOk = false; };
		TSharedRef<FState> St = MakeShared<FState>();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Args, bQuit, StartDelayFrames, St](float) -> bool
		{
			++St->Frame;
			if (!GEditor || St->Frame < StartDelayFrames)
			{
				return true;
			}
			if (!St->bBuilt)
			{
				St->bBuilt = true;
				St->bOk = RoverSceneBuilder::Build(Args);
				St->Frame = StartDelayFrames;
				if (!St->bOk)
				{
					UE_LOG(LogRoverEditor, Error, TEXT("Rover.BuildScene failed"));
					if (bQuit) { FPlatformMisc::RequestExitWithStatus(false, 1); }
					return false;
				}
				return true;
			}
			if (St->Frame < StartDelayFrames + 60)
			{
				return true;
			}
			const bool bSaved = RoverSceneBuilder::Save(Args);
			UE_LOG(LogRoverEditor, Log, TEXT("Rover.BuildScene %s"), bSaved ? TEXT("OK") : TEXT("save failed"));
			if (bQuit)
			{
				FPlatformMisc::RequestExitWithStatus(false, bSaved ? 0 : 1);
			}
			return false;
		}));
	}
}

class FHakoniwaRoverEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Command = MakeUnique<FAutoConsoleCommand>(
			TEXT("Rover.BuildScene"),
			TEXT("Build the lunar scene: -RoverGrid=<grid.json> -RoverSun=<sun.json> [-RoverGlb=<glb>] [-RoverMap=/Game/...]"),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Argv)
			{
				RunScheduled(ParseArgs(*(TEXT(" ") + FString::Join(Argv, TEXT(" ")))), false, 1);
			}));
		const TCHAR* Cmd = FCommandLine::Get();
		if (FParse::Param(Cmd, TEXT("RoverBuildScene")))
		{
			RunScheduled(ParseArgs(Cmd), FParse::Param(Cmd, TEXT("RoverQuit")), 120);
		}
	}

	virtual void ShutdownModule() override
	{
		Command.Reset();
	}

private:
	TUniquePtr<FAutoConsoleCommand> Command;
};

IMPLEMENT_MODULE(FHakoniwaRoverEditorModule, HakoniwaRoverEditor);
