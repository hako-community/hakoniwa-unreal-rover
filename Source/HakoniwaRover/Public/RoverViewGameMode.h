// RoverViewGameMode.h
// 月面の場面を見るだけのゲームモード。プレイヤーは姿の無い観客（DefaultPawn の球が場面に映らないように）。
// 見るだけなので、マウスは捕まえない（カーソルを出し、窓の外へ出せる。ほかの作業の邪魔をしない）。

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

	virtual void PostLogin(APlayerController* NewPlayer) override;
};
