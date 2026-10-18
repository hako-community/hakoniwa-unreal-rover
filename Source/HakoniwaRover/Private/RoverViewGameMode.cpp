// RoverViewGameMode.cpp

#include "RoverViewGameMode.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"

ARoverViewGameMode::ARoverViewGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}

void ARoverViewGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	if (NewPlayer)
	{
		// Config/DefaultInput.ini の NoCapture・DoNotLock と合わせて、カーソルを出したままにする
		NewPlayer->bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		NewPlayer->SetInputMode(Mode);
	}
}
