// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MoviePipelineViewFamilySetting.h"
#include "MoviePipelineDLSSSetting.generated.h"

class FSceneViewFamily;

UENUM(BlueprintType)
enum class EMoviePipelineDLSSQuality : uint8
{
	EMoviePipelineDLSSQuality_UltraQuality			UMETA(DisplayName = "Ultra Quality"),
	EMoviePipelineDLSSQuality_Quality				UMETA(DisplayName = "Quality"),
	EMoviePipelineDLSSQuality_Balanced				UMETA(DisplayName = "Balanced"),
	EMoviePipelineDLSSQuality_Performance			UMETA(DisplayName = "Performance"),
	EMoviePipelineDLSSQuality_UltraPerformance		UMETA(DisplayName = "Ultra Performance"),
};

UCLASS(BlueprintType)
class DLSSMOVIEPIPELINESUPPORT_API UMoviePipelineDLSSSetting : public UMoviePipelineViewFamilySetting
{
	GENERATED_BODY()
public:
	UMoviePipelineDLSSSetting();

public:
#if WITH_EDITOR
	virtual FText GetDisplayText() const override { return NSLOCTEXT("MovieRenderPipeline", "DlssSettingDisplayName", "DLSS"); }
#endif

	/** This method is run right before rendering starts which forwards all required info to DLSS plugin. */
	virtual void SetupViewFamily(FSceneViewFamily& ViewFamily) override;

	/** For the purposes of embedding DLSS quality setting. */
	virtual void GetFormatArguments(FMoviePipelineFormatArgs& InOutFormatArgs) const override;

	virtual void ValidateStateImpl() override;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS Quality settings", DisplayName = "DLSS Quality")
	EMoviePipelineDLSSQuality DLSSQuality;
};