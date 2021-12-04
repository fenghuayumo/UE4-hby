// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/TestGIVolume.h"
#include "Engine/CollisionProfile.h"
#include "Components/BrushComponent.h"
#include "COmponents/TestGIComponent.h"
#include "EngineUtils.h"

ATestGIVolume::ATestGIVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GetBrushComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	// post process volume needs physics data for trace
	GetBrushComponent()->bAlwaysCreatePhysicsState = true;
	GetBrushComponent()->Mobility = EComponentMobility::Movable;

	DDGIVolumeComponent = CreateDefaultSubobject<UTestGIComponent>(TEXT("DDGI"));

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
#endif
}


void ATestGIVolume::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
#if WITH_EDITOR
	if (Ar.IsLoading())
	{

	}
#endif
}

#if WITH_EDITOR

void ATestGIVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	DDGIVolumeComponent->MarkRenderDynamicDataDirty();
}

void ATestGIVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	DDGIVolumeComponent->MarkRenderDynamicDataDirty();
}

bool ATestGIVolume::CanEditChange(const FProperty* InProperty) const
{
	return Super::CanEditChange(InProperty);
}

void ATestGIVolume::PostUnregisterAllComponents()
{
	// Route clear to super first.
	Super::PostUnregisterAllComponents();
}

void ATestGIVolume::PostRegisterAllComponents()
{
	// Route update to super first.
	Super::PostRegisterAllComponents();
}

#endif // WITH_EDITOR
