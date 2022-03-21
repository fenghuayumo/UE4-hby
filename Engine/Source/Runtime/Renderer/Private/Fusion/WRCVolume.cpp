#include "WRCVolume.h"
#include "WRCVolumeComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

AWRCVolume::AWRCVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	WRCVolumeComponent = CreateDefaultSubobject<UWRCVolumeComponent>(TEXT("WRC"));

#if WITH_EDITORONLY_DATA
	BoxComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("Volume"));
	if (!IsRunningCommandlet())
	{
		if (BoxComponent != nullptr)
		{
			BoxComponent->SetBoxExtent(FVector{ 100.0f, 100.0f, 100.0f });
			BoxComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			BoxComponent->SetupAttachment(WRCVolumeComponent);
		}
	}
#endif // WITH_EDITORONLY_DATA

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
#endif
}

#if WITH_EDITOR
void AWRCVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	WRCVolumeComponent->MarkRenderDynamicDataDirty();
}

void AWRCVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	WRCVolumeComponent->MarkRenderDynamicDataDirty();
}
#endif
