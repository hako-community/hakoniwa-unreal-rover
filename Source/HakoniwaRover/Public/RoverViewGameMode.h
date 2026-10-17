// RoverViewGameMode.h
// 月面の場面を見るだけのゲームモード。プレイヤーは姿の無い観客（DefaultPawn の球が場面に映らないように）。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RoverViewGameMode.generated.h"

UCLASS()
class HAKONIWAROVER_API ARoverViewGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARoverViewGameMode();
};
