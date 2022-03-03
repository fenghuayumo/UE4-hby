#include "Fusion.h"


void FDeferredShadingSceneRenderer::PrepareRayTracingRadianceCacheGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{

}


void FDeferredShadingSceneRenderer::RadianceCacheTrace(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
{

}
