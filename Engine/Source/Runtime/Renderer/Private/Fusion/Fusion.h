#pragma once
#include "../DeferredShadingRenderer.h"
#ifdef RHI_RAYTRACING
#include "RayTracing/RayTracingSkyLight.h"
#include "PathTracing.h"
extern float GRayTracingGlobalIlluminationMaxRayDistance;
extern float GRayTracingGlobalIlluminationMaxShadowDistance;
extern TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationMaxBounces;

extern int32 GRayTracingGlobalIlluminationNextEventEstimationSamples;

extern float GRayTracingGlobalIlluminationDiffuseThreshold;
extern int32 GRayTracingGlobalIlluminationEvalSkyLight;
extern int32 GRayTracingGlobalIlluminationUseRussianRoulette;
// extern float GRayTracingGlobalIlluminationScreenPercentage;

extern TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry;
extern TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationEnableTransmission;
extern TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFireflySuppression;
extern TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationAccumulateEmissive;
extern void SetupLightParameters(
	FScene* Scene,
	const FViewInfo& View, FRDGBuilder& GraphBuilder,
	FRDGBufferSRV** OutLightBuffer, uint32* OutLightCount, FPathTracingSkylight* SkylightParameters, FPathTracingLightGrid* LightGridParameters = nullptr);

//extern bool IsFusionGIEnabled(const FViewInfo& View);
extern bool IsRestirGIEnabled(const FViewInfo& View);
extern bool IsSurfelGIEnabled(const FViewInfo& View);

#endif
