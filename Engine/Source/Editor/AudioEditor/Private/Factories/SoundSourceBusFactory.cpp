// Copyright Epic Games, Inc. All Rights Reserved.

#include "Factories/SoundSourceBusFactory.h"
#include "Sound/SoundSourceBus.h"
#include "AudioDeviceManager.h"
#include "Engine/Engine.h"
#include "Sound/AudioSettings.h"

USoundSourceBusFactory::USoundSourceBusFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SupportedClass = USoundSourceBus::StaticClass();
	bCreateNew = true;
	bEditorImport = false;
	bEditAfterNew = true;
}

UObject* USoundSourceBusFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	USoundSourceBus* SoundSourceBus = NewObject<USoundSourceBus>(InParent, InName, Flags);

	return SoundSourceBus;
}

bool USoundSourceBusFactory::CanCreateNew() const
{
	return true;
}
