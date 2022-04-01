
#pragma once

#include "CoreMinimal.h"
#include "ScreenSpaceDenoise.h"

class FFusionDenoiser : public IScreenSpaceDenoiser
{
public:
	const TCHAR* GetDebugName() const override
	{
		return TEXT("FusionDenoiser");
	}

public:
	FFusionDenoiser(const IScreenSpaceDenoiser* InWrappedDenoiser);

	// Inherited via IScreenSpaceDenoiser
	virtual EShadowRequirements GetShadowRequirements(const FViewInfo& View, const FLightSceneInfo& LightSceneInfo, const FShadowRayTracingConfig& RayTracingConfig) const final;
	virtual void DenoiseShadowVisibilityMasks(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const TStaticArray<FShadowVisibilityParameters, IScreenSpaceDenoiser::kMaxBatchSize>& InputParameters, const int32 InputParameterCount, TStaticArray<FShadowVisibilityOutputs, IScreenSpaceDenoiser::kMaxBatchSize>& Outputs) const final;
	virtual FPolychromaticPenumbraOutputs DenoisePolychromaticPenumbraHarmonics(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FPolychromaticPenumbraHarmonics& Inputs) const final;
	virtual FReflectionsOutputs DenoiseReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const final;
	virtual FReflectionsOutputs DenoiseWaterReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const override;
	virtual FAmbientOcclusionOutputs DenoiseAmbientOcclusion(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FAmbientOcclusionInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	virtual FDiffuseIndirectOutputs DenoiseDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	virtual FDiffuseIndirectOutputs DenoiseSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	virtual FDiffuseIndirectOutputs DenoiseReflectedSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	virtual FDiffuseIndirectHarmonic DenoiseDiffuseIndirectHarmonic(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectHarmonic& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	virtual bool SupportsScreenSpaceDiffuseIndirectDenoiser(EShaderPlatform Platform) const final;
	virtual FDiffuseIndirectOutputs DenoiseScreenSpaceDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const final;
	const IScreenSpaceDenoiser* GetWrappedDenoiser() const;

    static IScreenSpaceDenoiser*   GetDenoiser();
private:

	const IScreenSpaceDenoiser* WrappedDenoiser;
};