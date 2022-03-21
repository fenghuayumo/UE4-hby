
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "WRCVolume.generated.h"

class UBillboardComponent;
class UBoxComponent;
class UWRCVolumeComponent;

UCLASS(HideCategories = (Navigation, Physics, Collision, Rendering, Tags, Cooking, Replication, Input, Actor, HLOD, Mobile, LOD))
class AWRCVolume : public AActor
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GI", meta = (AllowPrivateAccess = "true"));
	UWRCVolumeComponent* WRCVolumeComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY(Transient);
	UBoxComponent* BoxComponent = nullptr;
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR
	void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override final;
	void PostEditMove(bool bFinished) override final;
#endif
};
