#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptInterface.h"
#include "GameFramework/Volume.h"
#include "Engine/BlendableInterface.h"
#include "Engine/Scene.h"
#include "TestGIVolume.generated.h"



class UTestGIComponent;

// for FTestSettings
UCLASS(autoexpandcategories = TestVolume, hidecategories = (Advanced, Collision, Volume, Brush, Attachment))
class ENGINE_API ATestGIVolume : public AVolume
{
	GENERATED_UCLASS_BODY()


	//~ Begin AActor Interface
	virtual void PostUnregisterAllComponents(void) override;

protected:
	virtual void PostRegisterAllComponents() override;
	//~ End AActor Interface

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GI", meta = (AllowPrivateAccess = "true"));
	UTestGIComponent* DDGIVolumeComponent;
public:

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual bool CanEditChange(const FProperty* InProperty) const override;
	void PostEditMove(bool bFinished) override final;
#endif // WITH_EDITOR
	virtual void Serialize(FArchive& Ar);
	//~ End UObject Interface

	/** Adds an Blendable (implements IBlendableInterface) to the array of Blendables (if it doesn't exist) and update the weight */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	void AddOrUpdateBlendable(TScriptInterface<IBlendableInterface> InBlendableObject, float InWeight = 1.0f) {  }
};