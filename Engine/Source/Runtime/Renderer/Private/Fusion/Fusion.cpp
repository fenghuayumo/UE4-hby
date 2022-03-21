#include "Fusion.h"

void FDeferredShadingSceneRenderer::FusionGI(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
{
    //SurfelGI(GraphBuilder, SceneTextures,  View, RayTracingConfig, UpscaleFactor, OutDenoiserInputs);
}