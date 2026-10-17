// RoverViewGameMode.cpp

#include "RoverViewGameMode.h"

#include "GameFramework/SpectatorPawn.h"

ARoverViewGameMode::ARoverViewGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}
