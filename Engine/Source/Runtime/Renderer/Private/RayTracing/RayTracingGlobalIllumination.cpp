// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeferredShadingRenderer.h"

#if RHI_RAYTRACING

#include "RayTracingSkyLight.h"
#include "ScenePrivate.h"
#include "SceneRenderTargets.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "RHIResources.h"
#include "UniformBuffer.h"
#include "RayGenShaderUtils.h"
#include "SceneTextureParameters.h"
#include "ScreenSpaceDenoise.h"
#include "ClearQuad.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"
#include "Raytracing/RaytracingOptions.h"
#include "BlueNoise.h"
#include "SceneTextureParameters.h"
#include "RayTracingDefinitions.h"
#include "RayTracingDeferredMaterials.h"
#include "RayTracingTypes.h"
#include "PathTracingDefinitions.h"
#include "PathTracing.h"
#include "RayTracingDDGIUpdate.h"
#include "Components/TestGIComponent.h"
#include "LightCutRendering.h"
#include "LightTreeTypes.h"
#include <limits>
static TAutoConsoleVariable<int32> CVarRayTracingGlobalIllumination(
	TEXT("r.RayTracing.GlobalIllumination"),
	-1,
	TEXT("-1: Value driven by postprocess volume (default) \n")
	TEXT(" 0: ray tracing global illumination off \n")
	TEXT(" 1: ray tracing restir global illumination off \n")
	TEXT(" 2: ray tracing global illumination enabled (brute force) \n")
	TEXT(" 3: ray tracing global illumination enabled (final gather)")
	TEXT(" 4: ray tracing ddgi\n")
	TEXT(" 4: ray tracing surfelGI\n"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingGlobalIlluminationSamplesPerPixel = -1;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationSamplesPerPixel(
	TEXT("r.RayTracing.GlobalIllumination.SamplesPerPixel"),
	GRayTracingGlobalIlluminationSamplesPerPixel,
	TEXT("Samples per pixel (default = -1 (driven by postprocesing volume))"),
	ECVF_RenderThreadSafe
);

float GRayTracingGlobalIlluminationMaxRayDistance = 1.0e27;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationMaxRayDistance(
	TEXT("r.RayTracing.GlobalIllumination.MaxRayDistance"),
	GRayTracingGlobalIlluminationMaxRayDistance,
	TEXT("Max ray distance (default = 1.0e27)")
);

float GRayTracingGlobalIlluminationMaxShadowDistance = -1.0;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationMaxShadowDistance(
	TEXT("r.RayTracing.GlobalIllumination.MaxShadowDistance"),
	GRayTracingGlobalIlluminationMaxShadowDistance,
	TEXT("Max shadow distance (default = -1.0, distance adjusted automatically so shadow rays do not hit the sky sphere) ")
);

TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationMaxBounces(
	TEXT("r.RayTracing.GlobalIllumination.MaxBounces"),
	-1,
	TEXT("Max bounces (default = -1 (driven by postprocesing volume))"),
	ECVF_RenderThreadSafe
);

int32 GRayTracingGlobalIlluminationNextEventEstimationSamples = 2;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationNextEventEstimationSamples(
	TEXT("r.RayTracing.GlobalIllumination.NextEventEstimationSamples"),
	GRayTracingGlobalIlluminationNextEventEstimationSamples,
	TEXT("Number of sample draws for next-event estimation (default = 2)")
	TEXT("NOTE: This parameter is experimental")
);

float GRayTracingGlobalIlluminationDiffuseThreshold = 0.01;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationDiffuseThreshold(
	TEXT("r.RayTracing.GlobalIllumination.DiffuseThreshold"),
	GRayTracingGlobalIlluminationDiffuseThreshold,
	TEXT("Diffuse luminance threshold for evaluating global illumination")
	TEXT("NOTE: This parameter is experimental")
);

static int32 GRayTracingGlobalIlluminationDenoiser = 1;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationDenoiser(
	TEXT("r.RayTracing.GlobalIllumination.Denoiser"),
	GRayTracingGlobalIlluminationDenoiser,
	TEXT("Denoising options (default = 1)")
);

int32 GRayTracingGlobalIlluminationEvalSkyLight = 0;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationEvalSkyLight(
	TEXT("r.RayTracing.GlobalIllumination.EvalSkyLight"),
	GRayTracingGlobalIlluminationEvalSkyLight,
	TEXT("Evaluate SkyLight multi-bounce contribution")
	TEXT("NOTE: This parameter is experimental")
);

int32 GRayTracingGlobalIlluminationUseRussianRoulette = 0;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationUseRussianRoulette(
	TEXT("r.RayTracing.GlobalIllumination.UseRussianRoulette"),
	GRayTracingGlobalIlluminationUseRussianRoulette,
	TEXT("Perform Russian Roulette to only cast diffuse rays on surfaces with brighter albedos (default = 0)")
	TEXT("NOTE: This parameter is experimental")
);

static float GRayTracingGlobalIlluminationScreenPercentage = 50;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationScreenPercentage(
	TEXT("r.RayTracing.GlobalIllumination.ScreenPercentage"),
	GRayTracingGlobalIlluminationScreenPercentage,
	TEXT("Screen percentage for ray tracing global illumination (default = 50)")
);

TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry(
	TEXT("r.RayTracing.GlobalIllumination.EnableTwoSidedGeometry"),
	1,
	TEXT("Enables two-sided geometry when tracing GI rays (default = 1)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationEnableTransmission(
	TEXT("r.RayTracing.GlobalIllumination.EnableTransmission"),
	1,
	TEXT("Enables transmission when tracing GI rays (default = 1)"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingGlobalIlluminationRenderTileSize = 0;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationRenderTileSize(
	TEXT("r.RayTracing.GlobalIllumination.RenderTileSize"),
	GRayTracingGlobalIlluminationRenderTileSize,
	TEXT("Render ray traced global illumination in NxN pixel tiles, where each tile is submitted as separate GPU command buffer, allowing high quality rendering without triggering timeout detection. (default = 0, tiling disabled)")
);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationMaxLightCount(
	TEXT("r.RayTracing.GlobalIllumination.MaxLightCount"),
	RAY_TRACING_LIGHT_COUNT_MAXIMUM,
	TEXT("Enables two-sided geometry when tracing GI rays (default = 256)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFireflySuppression(
	TEXT("r.RayTracing.GlobalIllumination.FireflySuppression"),
	0,
	TEXT("Applies tonemap operator to suppress potential fireflies (default = 0). "),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherIterations(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.Iterations"),
	1,
	TEXT("Determines the number of iterations for gather point creation\n"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherFilterWidth(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.FilterWidth"),
	0,
	TEXT("Determines the local neighborhood for sample stealing (default = 0)\n"),
	ECVF_RenderThreadSafe
);

static float GRayTracingGlobalIlluminationFinalGatherDistance = 10.0;
static FAutoConsoleVariableRef CVarRayTracingGlobalIlluminationFinalGatherDistance(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.Distance"),
	GRayTracingGlobalIlluminationFinalGatherDistance,
	TEXT("Maximum screen-space distance for valid, reprojected final gather points (default = 10)")
);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherSortMaterials(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.SortMaterials"),
	1,
	TEXT("Sets whether refected materials will be sorted before shading\n")
	TEXT("0: Disabled\n ")
	TEXT("1: Enabled, using Trace->Sort->Trace (Default)\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherSortTileSize(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.SortTileSize"),
	64,
	TEXT("Size of pixel tiles for sorted global illumination (default = 64)\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherSortSize(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.SortSize"),
	5,
	TEXT("Size of horizon for material ID sort\n")
	TEXT("0: Disabled\n")
	TEXT("1: 256 Elements\n")
	TEXT("2: 512 Elements\n")
	TEXT("3: 1024 Elements\n")
	TEXT("4: 2048 Elements\n")
	TEXT("5: 4096 Elements (Default)\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingDeferedGlobalIllumination(
	TEXT("r.RayTracing.GlobalIllumination.ExperimentalDeferred"),
	0,
	TEXT("Sets whether use defered gi shading\n")
	TEXT("0: Disabled\n ")
	TEXT("1: Enabled\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationFinalGatherEnableNeighborVisbilityTest(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.EnableNeighborVisibilityTest"),
	0,
	TEXT("Enables neighbor visibility tests when FilterWidth > 0 (default = 0)")
);

static TAutoConsoleVariable<float> CVarRayTracingGlobalIlluminationFinalGatherDepthRejectionKernel(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.DepthRejectionKernel"),
	1.0e-2,
	TEXT("Gather point relative Z-depth rejection tolerance (default = 1.0e-2)\n")
);

static TAutoConsoleVariable<float> CVarRayTracingGlobalIlluminationFinalGatherNormalRejectionKernel(
	TEXT("r.RayTracing.GlobalIllumination.FinalGather.NormalRejectionKernel"),
	0.2,
	TEXT("Gather point WorldNormal rejection tolerance (default = 1.0e-2)\n")
);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationDirectionalLight(
	TEXT("r.RayTracing.GlobalIllumination.Lights.DirectionalLight"),
	1,
	TEXT("Enables DirectionalLight sampling for global illumination (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationSkyLight(
	TEXT("r.RayTracing.GlobalIllumination.Lights.SkyLight"),
	1,
	TEXT("Enables SkyLight sampling for global illumination (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationPointLight(
	TEXT("r.RayTracing.GlobalIllumination.Lights.PointLight"),
	1,
	TEXT("Enables PointLight sampling for global illumination (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationSpotLight(
	TEXT("r.RayTracing.GlobalIllumination.Lights.SpotLight"),
	1,
	TEXT("Enables SpotLight sampling for global illumination (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationRectLight(
	TEXT("r.RayTracing.GlobalIllumination.Lights.RectLight"),
	1,
	TEXT("Enables RectLight sampling for global illumination (default = 1)"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationAccumulateEmissive(
	TEXT("r.RayTracing.GlobalIllumination.AccumulateEmissive"),
	1,
	TEXT("Adds emissive component from surfaces (default = 1)"),
	ECVF_RenderThreadSafe);
		
static TAutoConsoleVariable<int32> CVarRayTracingGlobalIlluminationTiming(
	TEXT("r.RayTracing.GlobalIllumination.Timing"),
	0,
	TEXT("Time cost of ray traced global illumination\n")
	TEXT("0: Off (default)\n")
	TEXT("1: Shaded\n")
	TEXT("2: Material gather\n")
	TEXT("3: Final gather\n")
	TEXT("4: All\n"),
	ECVF_RenderThreadSafe);


static TAutoConsoleVariable<float> CVarRayGuidingCellSize(
	TEXT("r.RayTracing.RayGuidingCellSize"),
	1,
	TEXT("Set Spatial Cell Size (default = 1)"),
	ECVF_RenderThreadSafe);

//LightSamping Related
static TAutoConsoleVariable<int> CVarLightSamplingType(
	TEXT("r.RayTracing.LightSamplingType"),
	1,
	TEXT(" 0:	BrtuteForce \n")
	TEXT(" 1:	LightGrid \n")
	TEXT(" 2:	LightTree \n")
	TEXT(" 3:	LightCut \n")
	TEXT(" 4:	Uniform \n")
	TEXT(" 5:	ReGIR \n"),
	ECVF_RenderThreadSafe);

// static TAutoConsoleVariable<float> CVarReGIRLightGridCellSize(
// 	TEXT("r.RayTracing.ReGIR.LightGridCellSize"),
// 	50.0,
// 	TEXT("Set to ReGIR GridCell Size"),
// 	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarReGIRLightGridResolution(
	TEXT("r.RayTracing.ReGIR.LightGridResolution"),
	32,
	TEXT("Set to ReGIR GridCell Dim"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNumLightSlotsPerCell(
	TEXT("r.RayTracing.ReGIR.NumLightSlotsPerCell"),
	1024,
	TEXT("Set to ReGIR NumLightSlotsPerCell"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNumCandidatePerLightSlot(
	TEXT("r.RayTracing.ReGIR.NumCandidatePerLightSlot"),
	2,
	TEXT("Set to ReGIR NumCandidatePerLightSlot"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNumCandidatePerCell(
	TEXT("r.RayTracing.ReGIR.NumCandidatePerCell"),
	4,
	TEXT("Set to ReGIR NumCandidatePerCell"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarReGIRTemporalMaxHistory(
	TEXT("r.RayTracing.ReGIR.MaxHistory"),
	64,
	TEXT("Maximum temporal history for samples (default 10)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarReGIREnaleTemporal(
	TEXT("r.RayTracing.ReGIR.TemporalEnable"),
	1,
	TEXT("Enable Temporal ReGIR"),
	ECVF_RenderThreadSafe);

DECLARE_GPU_STAT_NAMED(RayTracingGIBruteForce, TEXT("Ray Tracing GI: Brute Force"));
DECLARE_GPU_STAT_NAMED(RayTracingGIFinalGather, TEXT("Ray Tracing GI: Final Gather"));
DECLARE_GPU_STAT_NAMED(RayTracingGICreateGatherPoints, TEXT("Ray Tracing GI: Create Gather Points"));
DECLARE_GPU_STAT_NAMED(RayTracingDDGI, TEXT("Ray Tracing GI: DDGI"));
DECLARE_GPU_STAT_NAMED(RTDDGI_Update, TEXT("Ray TracingDDGI Update"));
DECLARE_GPU_STAT_NAMED(RayTracingFGRestir, TEXT("MeshLight ReGIR: RestirReGIR"));


extern void PrepareLightGrid(FRDGBuilder& GraphBuilder, FPathTracingLightGrid* LightGridParameters, const FPathTracingLight* Lights, uint32 NumLights, uint32 NumInfiniteLights, FRDGBufferSRV* LightsSRV);
void SetupLightParameters(
	FScene* Scene,
	const FViewInfo& View, FRDGBuilder& GraphBuilder,
	FRDGBufferSRV** OutLightBuffer, uint32* OutLightCount, FPathTracingSkylight* SkylightParameters, FPathTracingLightGrid* LightGridParameters = nullptr)
{
	FPathTracingLight Lights[RAY_TRACING_LIGHT_COUNT_MAXIMUM];
	unsigned LightCount = 0;

	// Get the SkyLight color

	FSkyLightSceneProxy* SkyLight = Scene->SkyLight;

	const bool bUseMISCompensation = true;
	const bool bSkylightEnabled = SkyLight && SkyLight->bAffectGlobalIllumination && CVarRayTracingGlobalIlluminationSkyLight.GetValueOnRenderThread() != 0;
	uint32 NumLights = 0;

	// Prepend SkyLight to light buffer since it is not part of the regular light list
	const float Inf = std::numeric_limits<float>::infinity();
	// Prepend SkyLight to light buffer (if it is active)
	if (PrepareSkyTexture(GraphBuilder, Scene, View, bSkylightEnabled, bUseMISCompensation, SkylightParameters))
	{
		FPathTracingLight& DestLight = Lights[LightCount];

		DestLight.Color = FVector(1.0f, 1.0f, 1.0f);
		DestLight.Flags = SkyLight->bTransmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
		// SkyLight does not have a LightingChannelMask
		DestLight.Flags |= PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
		DestLight.Flags |= PATHTRACING_LIGHT_SKY;
		DestLight.BoundMin = FVector(-Inf, -Inf, -Inf);
		DestLight.BoundMax = FVector(Inf, Inf, Inf);
		LightCount++;
	}
	for (auto Light : Scene->Lights)
	{
		ELightComponentType LightComponentType = (ELightComponentType)Light.LightSceneInfo->Proxy->GetLightType();

		if (LightComponentType != LightType_Directional)
		{
			continue;
		}

		FLightShaderParameters LightParameters;
		Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);

		if (LightParameters.Color.IsZero())
		{
			continue;
		}

		FPathTracingLight& DestLight = Lights[LightCount++];
		uint32 Transmission = Light.LightSceneInfo->Proxy->Transmission();
		uint8 LightingChannelMask = Light.LightSceneInfo->Proxy->GetLightingChannelMask();

		DestLight.Flags = Transmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
		DestLight.Flags |= LightingChannelMask & PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
		DestLight.Flags |= Light.LightSceneInfo->Proxy->CastsDynamicShadow() ? PATHTRACER_FLAG_CAST_SHADOW_MASK : 0;
		DestLight.IESTextureSlice = -1;
		DestLight.RectLightTextureIndex = -1;

		// these mean roughly the same thing across all light types
		DestLight.Color = LightParameters.Color;
		DestLight.Position = LightParameters.Position;
		DestLight.Normal = -LightParameters.Direction;
		DestLight.dPdu = FVector::CrossProduct(LightParameters.Tangent, LightParameters.Direction);
		DestLight.dPdv = LightParameters.Tangent;
		DestLight.Attenuation = LightParameters.InvRadius;
		DestLight.FalloffExponent = 0;

		DestLight.Normal = LightParameters.Direction;
		DestLight.Dimensions = FVector(LightParameters.SourceRadius, LightParameters.SoftSourceRadius, 0.0f);
		DestLight.Flags |= PATHTRACING_LIGHT_DIRECTIONAL;

		DestLight.BoundMin = FVector(-Inf, -Inf, -Inf);
		DestLight.BoundMax = FVector(Inf, Inf, Inf);
	}

	uint32 InfiniteLights = LightCount;
	
	const uint32 MaxLightCount = FMath::Min(CVarRayTracingGlobalIlluminationMaxLightCount.GetValueOnRenderThread(), RAY_TRACING_LIGHT_COUNT_MAXIMUM);
	for (auto Light : Scene->Lights)
	{
		if (LightCount >= MaxLightCount) break;

		ELightComponentType LightComponentType = (ELightComponentType)Light.LightSceneInfo->Proxy->GetLightType();
		if ((LightComponentType == LightType_Directional) /* already handled by the loop above */)
			continue;

		if (Light.LightSceneInfo->Proxy->HasStaticLighting() && Light.LightSceneInfo->IsPrecomputedLightingValid()) continue;
		if (!Light.LightSceneInfo->Proxy->AffectGlobalIllumination()) continue;

		FPathTracingLight& DestLight = Lights[LightCount]; // don't increment LightCount yet -- we might still skip this light

		FLightShaderParameters LightShaderParameters;
		Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightShaderParameters);

		uint32 Transmission = Light.LightSceneInfo->Proxy->Transmission();
		uint8 LightingChannelMask = Light.LightSceneInfo->Proxy->GetLightingChannelMask();
		DestLight.Flags  = Transmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
		DestLight.Flags |= LightingChannelMask & PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
		DestLight.Flags |= Light.LightSceneInfo->Proxy->IsInverseSquared() ? 0 : PATHTRACER_FLAG_NON_INVERSE_SQUARE_FALLOFF_MASK;

		DestLight.FalloffExponent = LightShaderParameters.FalloffExponent;
		DestLight.Attenuation = LightShaderParameters.InvRadius;
		DestLight.IESTextureSlice = -1; // not used by this path at the moment

		switch (LightComponentType)
		{

		case LightType_Rect:
		{
			if (CVarRayTracingGlobalIlluminationRectLight.GetValueOnRenderThread() == 0) continue;

			DestLight.Position = LightShaderParameters.Position;
			DestLight.Normal = -LightShaderParameters.Direction;
			DestLight.dPdu = FVector::CrossProduct(LightShaderParameters.Direction, LightShaderParameters.Tangent);
			DestLight.dPdv = LightShaderParameters.Tangent;
			DestLight.Color = LightShaderParameters.Color;
			DestLight.Dimensions = FVector(2.0f * LightShaderParameters.SourceRadius, 2.0f * LightShaderParameters.SourceLength, 0.0f);
			DestLight.Shaping = FVector2D(LightShaderParameters.RectLightBarnCosAngle, LightShaderParameters.RectLightBarnLength);
			DestLight.Flags |= PATHTRACING_LIGHT_RECT;

			float Radius = 1.0f / LightShaderParameters.InvRadius;
			FVector Center = DestLight.Position;
			FVector Normal = DestLight.Normal;
			FVector Disc = FVector(
				FMath::Sqrt(FMath::Clamp(1 - Normal.X * Normal.X, 0.0f, 1.0f)),
				FMath::Sqrt(FMath::Clamp(1 - Normal.Y * Normal.Y, 0.0f, 1.0f)),
				FMath::Sqrt(FMath::Clamp(1 - Normal.Z * Normal.Z, 0.0f, 1.0f))
			);
			// quad bbox is the bbox of the disc +  the tip of the hemisphere
			// TODO: is it worth trying to account for barndoors? seems unlikely to cut much empty space since the volume _inside_ the barndoor receives light
			FVector Tip = Center + Normal * Radius;
			DestLight.BoundMin = Tip.ComponentMin(Center - Radius * Disc);
			DestLight.BoundMax = Tip.ComponentMax(Center + Radius * Disc);

			break;
		}
		case LightType_Point:
		default:
		{
			if (CVarRayTracingGlobalIlluminationPointLight.GetValueOnRenderThread() == 0) continue;

			DestLight.Position = LightShaderParameters.Position;
			// #dxr_todo: UE-72556 define these differences from Lit..
			DestLight.Color = LightShaderParameters.Color;
			float SourceRadius = 0.0; // LightShaderParameters.SourceRadius causes too much noise for little pay off at this time
			DestLight.Dimensions = FVector(SourceRadius, 0.0, 0.0);
			DestLight.Flags |= PATHTRACING_LIGHT_POINT;

			float Radius = 1.0f / LightShaderParameters.InvRadius;
			FVector Center = DestLight.Position;
			// simple sphere of influence
			DestLight.BoundMin = Center - FVector(Radius, Radius, Radius);
			DestLight.BoundMax = Center + FVector(Radius, Radius, Radius);
			DestLight.Normal = FVector(0,1,0);
			break;
		}
		case LightType_Spot:
		{
			if (CVarRayTracingGlobalIlluminationSpotLight.GetValueOnRenderThread() == 0) continue;

			DestLight.Position = LightShaderParameters.Position;
			DestLight.Normal = -LightShaderParameters.Direction;
			// #dxr_todo: UE-72556 define these differences from Lit..
			DestLight.Color = LightShaderParameters.Color;
			float SourceRadius = 0.0; // LightShaderParameters.SourceRadius causes too much noise for little pay off at this time
			DestLight.Dimensions = FVector(SourceRadius, 0.0, 0.0);
			DestLight.Shaping = LightShaderParameters.SpotAngles;
			DestLight.Flags |= PATHTRACING_LIGHT_SPOT;
			float Radius = 1.0f / LightShaderParameters.InvRadius;
			FVector Center = DestLight.Position;
			FVector Normal = DestLight.Normal;
			FVector Disc = FVector(
				FMath::Sqrt(FMath::Clamp(1 - Normal.X * Normal.X, 0.0f, 1.0f)),
				FMath::Sqrt(FMath::Clamp(1 - Normal.Y * Normal.Y, 0.0f, 1.0f)),
				FMath::Sqrt(FMath::Clamp(1 - Normal.Z * Normal.Z, 0.0f, 1.0f))
			);
			// box around ray from light center to tip of the cone
			FVector Tip = Center + Normal * Radius;
			DestLight.BoundMin = Center.ComponentMin(Tip);
			DestLight.BoundMax = Center.ComponentMax(Tip);
			// expand by disc around the farthest part of the cone

			float CosOuter = LightShaderParameters.SpotAngles.X;
			float SinOuter = FMath::Sqrt(1.0f - CosOuter * CosOuter);

			DestLight.BoundMin = DestLight.BoundMin.ComponentMin(Center + Radius * (Normal * CosOuter - Disc * SinOuter));
			DestLight.BoundMax = DestLight.BoundMax.ComponentMax(Center + Radius * (Normal * CosOuter + Disc * SinOuter));
			break;
		}
		};

		DestLight.Color *= Light.LightSceneInfo->Proxy->GetIndirectLightingScale();

		// we definitely added the light if we reach this point
		LightCount++;
	}
	{
		size_t DataSize = sizeof(FPathTracingLight) * FMath::Max(LightCount, 1u);
		*OutLightBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CreateStructuredBuffer(GraphBuilder, TEXT("RTGILightsBuffer"), sizeof(FPathTracingLight), FMath::Max(LightCount, 1u), Lights, DataSize)));
		*OutLightCount = LightCount;
	}
	if(LightGridParameters)
		PrepareLightGrid(GraphBuilder, LightGridParameters, Lights, LightCount, InfiniteLights, *OutLightBuffer);
}

int32 GetRayTracingGlobalIlluminationSamplesPerPixel(const FViewInfo& View)
{
	int32 SamplesPerPixel = GRayTracingGlobalIlluminationSamplesPerPixel > -1 ? GRayTracingGlobalIlluminationSamplesPerPixel : View.FinalPostProcessSettings.RayTracingGISamplesPerPixel;
	return SamplesPerPixel;
}

bool ShouldRenderRayTracingGlobalIllumination(const FViewInfo& View)
{
	if (GetRayTracingGlobalIlluminationSamplesPerPixel(View) <= 0)
	{
		return false;
	}

	const int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();

	const bool bEnabled = CVarRayTracingGlobalIlluminationValue >= 0
		? CVarRayTracingGlobalIlluminationValue > 0
		: View.FinalPostProcessSettings.RayTracingGIType > ERayTracingGlobalIlluminationType::Disabled;

	return ShouldRenderRayTracingEffect(bEnabled);
}

bool IsRestirGIEnabled(const FViewInfo& View)
{
	int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	if (CVarRayTracingGlobalIlluminationValue >= 0)
	{
		return CVarRayTracingGlobalIlluminationValue == 1;
	}

	return View.FinalPostProcessSettings.RayTracingGIType == ERayTracingGlobalIlluminationType::RestirGI;
}

bool IsSurfelGIEnabled(const FViewInfo& View)
{
	int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	if (CVarRayTracingGlobalIlluminationValue >= 0)
	{
		return CVarRayTracingGlobalIlluminationValue == 5;
	}

	return View.FinalPostProcessSettings.RayTracingGIType == ERayTracingGlobalIlluminationType::SurfelGI;
}

bool ShouldScreenDoise(const FViewInfo& View)
{
	if( IsRestirGIEnabled(View) || IsSurfelGIEnabled(View))
		return false;
	return true;
}

bool IsFinalGatherEnabled(const FViewInfo& View)
{
	int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	if (CVarRayTracingGlobalIlluminationValue >= 0)
	{
		return CVarRayTracingGlobalIlluminationValue == 3;
	}

	return View.FinalPostProcessSettings.RayTracingGIType == ERayTracingGlobalIlluminationType::FinalGather;
}

bool IsDeferedGIEnabled(const FViewInfo& View)
{
	int32 CVarDeferedGIValue = CVarRayTracingDeferedGlobalIllumination.GetValueOnRenderThread();
	//int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();

	return CVarDeferedGIValue == 1; 
}

bool IsRayTracingDDGI(const FViewInfo& View)
{
	int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	if (CVarRayTracingGlobalIlluminationValue >= 0)
	{
		return CVarRayTracingGlobalIlluminationValue == 4;
	}

	return View.FinalPostProcessSettings.RayTracingGIType == ERayTracingGlobalIlluminationType::DDGIPlus;
}

enum class ELightSamplingType
{
	BrtuteForce = 0,
	LIGHT_GRID = 1,
	LIGHT_TREE = 2,
	LIGHT_CUT = 3,
	Light_UNIFORM = 4,
	LIGHT_REGIR = 5,
	MAX,
};
struct ReGIR_PackedReservoir
{
	FIntVector4 HitGeometry;
	FIntVector4 LightInfo;
};

BEGIN_SHADER_PARAMETER_STRUCT(FReGIRCommonParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<ReGIR_PackedReservoir>, RWLightReservoirUAV)
SHADER_PARAMETER(FIntVector, GridCellDim)
SHADER_PARAMETER(uint32, NumLightSlotsPerCell)
SHADER_PARAMETER(int32, NumCandidatesPerLightSlot)
SHADER_PARAMETER(int32, NumCandidatesPerCell)
SHADER_PARAMETER(FVector, LightBoundMin)
SHADER_PARAMETER(FVector, LightBoundMax)


END_SHADER_PARAMETER_STRUCT()


class FBuildCellReservoirCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildCellReservoirCS)
	SHADER_USE_PARAMETER_STRUCT(FBuildCellReservoirCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, LightGridAxis)
		SHADER_PARAMETER(uint32, LightGridResolution)

		SHADER_PARAMETER_SRV(StructuredBuffer<FVector>, MeshLightVertexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, MeshLightIndexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstanceTriangle>, MeshLightInstancePrimitiveBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstance>, MeshLightInstanceBuffer)
		SHADER_PARAMETER(uint32, NumLightTriangles)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReGIRCommonParameters, CommonParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER(int32, OutputSlice)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FBuildCellReservoirCS, "/Engine/Private/ReGIR/BuildCellReservoirs.usf", "BuildCellReservoirCS", SF_Compute);

class FReGIRTemporalResamplingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReGIRTemporalResamplingCS)
	SHADER_USE_PARAMETER_STRUCT(FReGIRTemporalResamplingCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, LightGridAxis)
		SHADER_PARAMETER(uint32, LightGridResolution)

		SHADER_PARAMETER_SRV(StructuredBuffer<FVector>, MeshLightVertexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, MeshLightIndexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstanceTriangle>, MeshLightInstancePrimitiveBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstance>, MeshLightInstanceBuffer)
		SHADER_PARAMETER(uint32, NumLightTriangles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<ReGIR_PackedReservoir>, LightReservoirHistory)
		SHADER_PARAMETER(uint32, MaxTemporalHistory)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReGIRCommonParameters, CommonParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReGIRTemporalResamplingCS, "/Engine/Private/ReGIR/BuildCellReservoirs.usf", "BuildCellReservoirTemporalResamplingCS", SF_Compute);


class FWriteHistoryReservoirCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FWriteHistoryReservoirCS)
	SHADER_USE_PARAMETER_STRUCT(FWriteHistoryReservoirCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
	}

	static uint32 GetThreadBlockSize()
	{
		return 1024;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<ReGIR_PackedReservoir>, RWLightReservoirHistoryUAV)
		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReGIRCommonParameters, CommonParameters)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FWriteHistoryReservoirCS, "/Engine/Private/ReGIR/BuildCellReservoirs.usf", "WriteHistoryReservoirCS", SF_Compute);

class FGlobalIlluminationRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGlobalIlluminationRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FGlobalIlluminationRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	class FLightSamplingTypeDim : SHADER_PERMUTATION_ENUM_CLASS("LIGHT_SAMPLING_TYPE", ELightSamplingType);
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim, FLightSamplingTypeDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// We need the skylight to do its own form of MIS because RTGI doesn't do its own
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);
		//OutEnvironment.SetDefine(TEXT("LIGHT_SAMPLING_TYPE"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, RenderTileOffsetX)
		SHADER_PARAMETER(uint32, RenderTileOffsetY)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

		// Timing support
		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWGlobalIlluminationRayDistanceUAV)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		SHADER_PARAMETER_STRUCT_INCLUDE(FLightCutCommonParameter, LightCutCommonParameters)
		SHADER_PARAMETER(uint32, DistanceType)
		SHADER_PARAMETER(uint32, LeafStartIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLightNode>, NodesBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, LightCutBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, MeshLightCutBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLightNode>, MeshLightNodesBuffer)
		SHADER_PARAMETER(uint32, MeshLightLeafStartIndex)
		// SHADER_PARAMETER_SRV(StructuredBuffer<FVector>, MeshLightVertexBuffer)
		// SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, MeshLightIndexBuffer)
		// SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstanceTriangle>, MeshLightInstancePrimitiveBuffer)
		// SHADER_PARAMETER_SRV(StructuredBuffer<MeshLightInstance>, MeshLightInstanceBuffer)
		// SHADER_PARAMETER(uint32, NumLightTriangles)
		SHADER_PARAMETER_STRUCT_INCLUDE(FMeshLightCommonParameter, MeshLightCommonParam)
		//SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<ReGIR_PackedReservoir>, RWLightReservoirHistoryUAV)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReGIRCommonParameters, ReGIRCommonParameters)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGlobalIlluminationRGS, "/Engine/Private/RayTracing/RayTracingGlobalIlluminationRGS.usf", "GlobalIlluminationRGS", SF_RayGen);

// Note: This constant must match the definition in RayTracingGatherPoints.ush
constexpr int32 MAXIMUM_GATHER_POINTS_PER_PIXEL = 32;

struct FGatherPoint
{
	FVector CreationPoint;
	FVector Position;
	FIntPoint Irradiance;
};

class FRayTracingGlobalIlluminationCreateGatherPointsRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingGlobalIlluminationCreateGatherPointsRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingGlobalIlluminationCreateGatherPointsRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FDeferredMaterialMode : SHADER_PERMUTATION_ENUM_CLASS("DIM_DEFERRED_MATERIAL_MODE", EDeferredMaterialMode);
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);

	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FDeferredMaterialMode, FEnableTransmissionDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// We need the skylight to do its own form of MIS because RTGI doesn't do its own
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GatherSamplesPerPixel)
		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, GatherPointIteration)
		SHADER_PARAMETER(uint32, GatherFilterWidth)
		SHADER_PARAMETER(uint32, SampleIndex)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(uint32, RenderTileOffsetX)
		SHADER_PARAMETER(uint32, RenderTileOffsetY)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

		// Timing support
		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		// Scene data
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Light data
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)

		// Shading data
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		SHADER_PARAMETER(FIntPoint, GatherPointsResolution)
		SHADER_PARAMETER(FIntPoint, TileAlignedResolution)
		SHADER_PARAMETER(int32, SortTileSize)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GatherPoints>, RWGatherPointsBuffer)
		// Optional indirection buffer used for sorted materials
		SHADER_PARAMETER_RDG_BUFFER_UAV(StructuredBuffer<FDeferredMaterialPayload>, MaterialBuffer)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingGlobalIlluminationCreateGatherPointsRGS, "/Engine/Private/RayTracing/RayTracingCreateGatherPointsRGS.usf", "RayTracingCreateGatherPointsRGS", SF_RayGen);

// Auxillary gather point data for reprojection
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FGatherPointData, )
	SHADER_PARAMETER(uint32, Count)
	SHADER_PARAMETER_ARRAY(FMatrix, ViewMatrices, [MAXIMUM_GATHER_POINTS_PER_PIXEL])
END_GLOBAL_SHADER_PARAMETER_STRUCT()
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FGatherPointData, "GatherPointData");

class FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FDeferredMaterialMode : SHADER_PERMUTATION_ENUM_CLASS("DIM_DEFERRED_MATERIAL_MODE", EDeferredMaterialMode);

	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FDeferredMaterialMode>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GatherSamplesPerPixel)
		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, GatherPointIteration)
		SHADER_PARAMETER(uint32, GatherFilterWidth)
		SHADER_PARAMETER(uint32, SampleIndex)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(uint32, RenderTileOffsetX)
		SHADER_PARAMETER(uint32, RenderTileOffsetY)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

		// Timing support
		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		// Scene data
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Light data
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)

		// Shading data
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		SHADER_PARAMETER(FIntPoint, GatherPointsResolution)
		SHADER_PARAMETER(FIntPoint, TileAlignedResolution)
		SHADER_PARAMETER(int32, SortTileSize)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GatherPoints>, RWGatherPointsBuffer)
		// Optional indirection buffer used for sorted materials
		SHADER_PARAMETER_RDG_BUFFER_UAV(StructuredBuffer<FDeferredMaterialPayload>, MaterialBuffer)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS, "/Engine/Private/RayTracing/RayTracingCreateGatherPointsRGS.usf", "RayTracingCreateGatherPointsTraceRGS", SF_RayGen);

class FRayTracingGlobalIlluminationFinalGatherRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingGlobalIlluminationFinalGatherRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingGlobalIlluminationFinalGatherRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableNeighborVisibilityTestDim : SHADER_PERMUTATION_BOOL("USE_NEIGHBOR_VISIBILITY_TEST");

	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableNeighborVisibilityTestDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SampleIndex)
		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, GatherPointIterations)
		SHADER_PARAMETER(uint32, GatherFilterWidth)
		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(uint32, RenderTileOffsetX)
		SHADER_PARAMETER(uint32, RenderTileOffsetY)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(float, FinalGatherDistance)
		SHADER_PARAMETER(float, DepthRejectionKernel)
		SHADER_PARAMETER(float, NormalRejectionKernel)

		// Reprojection data
		SHADER_PARAMETER_STRUCT_REF(FGatherPointData, GatherPointData)
		
		// Timing support
		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		// Scene data
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Shading data
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		// Gather points
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GatherPoints>, GatherPointsBuffer)
		SHADER_PARAMETER(FIntPoint, GatherPointsResolution)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWGlobalIlluminationRayDistanceUAV)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingGlobalIlluminationFinalGatherRGS, "/Engine/Private/RayTracing/RayTracingFinalGatherRGS.usf", "RayTracingFinalGatherRGS", SF_RayGen);

BEGIN_SHADER_PARAMETER_STRUCT(FVolumeData, )
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbeIrradiance)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbeDistance)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbeOffsets)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ProbeStates)
SHADER_PARAMETER(FVector, Position)
SHADER_PARAMETER(FVector4, Rotation)
SHADER_PARAMETER(FVector, Radius)
SHADER_PARAMETER(FVector, ProbeGridSpacing)
SHADER_PARAMETER(FIntVector, ProbeGridCounts)
SHADER_PARAMETER(FIntVector, ProbeScrollOffsets)
SHADER_PARAMETER(uint32, LightingChannelMask)
SHADER_PARAMETER(int, ProbeNumIrradianceTexels)
SHADER_PARAMETER(int, ProbeNumDistanceTexels)
SHADER_PARAMETER(float, ProbeIrradianceEncodingGamma)
SHADER_PARAMETER(float, NormalBias)
SHADER_PARAMETER(float, ViewBias)
SHADER_PARAMETER(float, BlendDistance)
SHADER_PARAMETER(float, BlendDistanceBlack)
SHADER_PARAMETER(float, ApplyLighting)
SHADER_PARAMETER(float, IrradianceScalar)
SHADER_PARAMETER(int, ProbeNumRays)
SHADER_PARAMETER(FMatrix, RayRotationTransform)
END_SHADER_PARAMETER_STRUCT()


#define NUM_X	16
#define NUM_Y   8
#define NUM_XY  (NUM_X * NUM_Y)
struct GuidingEntry
{
	uint32_t Luminance[NUM_XY];
};

class FGlobalIlluminationDDGIRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGlobalIlluminationDDGIRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FGlobalIlluminationDDGIRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	//class FLightingChannelsDim : SHADER_PERMUTATION_BOOL("USE_LIGHTING_CHANNELS");
	class FEnableRelocation : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_PROBE_RELOCATION");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");
	class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim, FEnableRelocation, FEnableScrolling,
		FFormatRadiance, FFormatIrradiance>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// We need the skylight to do its own form of MIS because RTGI doesn't do its own
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);

		FString volumeMacroList;
		for (int i = 0; i < FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_MAX_SHADING_VOLUMES; ++i)
			volumeMacroList += FString::Printf(TEXT(" VOLUME_ENTRY(%i)"), i);
		OutEnvironment.SetDefine(TEXT("VOLUME_LIST"), volumeMacroList.GetCharArray().GetData());

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, RenderTileOffsetX)
		SHADER_PARAMETER(uint32, RenderTileOffsetY)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWGlobalIlluminationRayDistanceUAV)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)
		//SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightingChannelsTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(int32, NumVolumes)
		SHADER_PARAMETER_STRUCT_ARRAY(FVolumeData, DDGIVolume, [FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_MAX_SHADING_VOLUMES])
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDebugUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeRayDataUAV)
		//SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GuidingEntry>, RWRayGuidingEntries)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWRayGuidingMaxQ)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGlobalIlluminationDDGIRGS, "/Engine/Private/RayTracing/RayTracingDDGIRGS.usf", "GlobalIlluminationRGS", SF_RayGen);

struct SceneBounds
{
	FVector		pMin;
	FVector		PMax;
};

bool ShouldRenderRayTracingRestirGI(const FViewInfo& View)
{
	const int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	return ShouldRenderRayTracingEffect(CVarRayTracingGlobalIlluminationValue == 1);
}

void FDeferredShadingSceneRenderer::PrepareRayTracingGlobalIllumination(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (!ShouldRenderRayTracingGlobalIllumination(View))
	{
		return;
	}

	const bool bSortMaterials = CVarRayTracingGlobalIlluminationFinalGatherSortMaterials.GetValueOnRenderThread() != 0;
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();
	//const bool bEnableAdaptive = CVarGlobalIlluminationAdaptiveSamplingEnable.GetValueOnRenderThread();
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		for(uint32 i= 0; i < (uint32)(ELightSamplingType::MAX);i++)
		{
			FGlobalIlluminationRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FGlobalIlluminationRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FGlobalIlluminationRGS::FEnableTransmissionDim>(EnableTransmission);
			PermutationVector.Set< FGlobalIlluminationRGS::FLightSamplingTypeDim>((ELightSamplingType)i);
			//PermutationVector.Set<FGlobalIlluminationRGS::FEnableAdaptiveDim>(bEnableAdaptive);
			TShaderMapRef<FGlobalIlluminationRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}

		{
			FGlobalIlluminationDDGIRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableTransmissionDim>(EnableTransmission);
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableRelocation>(true);
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableScrolling>(false);
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FFormatIrradiance>(false);
			PermutationVector.Set<FGlobalIlluminationDDGIRGS::FFormatRadiance>(false);
			//PermutationVector.Set<FGlobalIlluminationDDGIRGS::FLightingChannelsDim>(false);
			TShaderMapRef<FGlobalIlluminationDDGIRGS> RayGenerationShader1(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader1.GetRayTracingShader());
		}
		if (bSortMaterials)
		{
			// Gather
			{
				FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FPermutationDomain CreateGatherPointsPermutationVector;
				CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
				CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
				TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS> CreateGatherPointsRayGenerationShader(View.ShaderMap, CreateGatherPointsPermutationVector);
				OutRayGenShaders.Add(CreateGatherPointsRayGenerationShader.GetRayTracingShader());
			}

			// Shade
			{
				FRayTracingGlobalIlluminationCreateGatherPointsRGS::FPermutationDomain CreateGatherPointsPermutationVector;
				CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
				CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
				CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTransmissionDim>(EnableTransmission);
				TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsRGS> CreateGatherPointsRayGenerationShader(View.ShaderMap, CreateGatherPointsPermutationVector);
				OutRayGenShaders.Add(CreateGatherPointsRayGenerationShader.GetRayTracingShader());
			}
		}
		else
		{
			FRayTracingGlobalIlluminationCreateGatherPointsRGS::FPermutationDomain CreateGatherPointsPermutationVector;
			CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FDeferredMaterialMode>(EDeferredMaterialMode::None);
			CreateGatherPointsPermutationVector.Set < FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTransmissionDim>(EnableTransmission);
			TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsRGS> CreateGatherPointsRayGenerationShader(View.ShaderMap, CreateGatherPointsPermutationVector);
			OutRayGenShaders.Add(CreateGatherPointsRayGenerationShader.GetRayTracingShader());
		}

		for (int EnableNeighborVisibilityTest = 0; EnableNeighborVisibilityTest < 2; ++EnableNeighborVisibilityTest)
		{
			FRayTracingGlobalIlluminationFinalGatherRGS::FPermutationDomain GatherPassPermutationVector;
			GatherPassPermutationVector.Set<FRayTracingGlobalIlluminationFinalGatherRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			GatherPassPermutationVector.Set<FRayTracingGlobalIlluminationFinalGatherRGS::FEnableNeighborVisibilityTestDim>(EnableNeighborVisibilityTest == 1);
			TShaderMapRef<FRayTracingGlobalIlluminationFinalGatherRGS> GatherPassRayGenerationShader(View.ShaderMap, GatherPassPermutationVector);
			OutRayGenShaders.Add(GatherPassRayGenerationShader.GetRayTracingShader());
		}
	}
	//if (!ShouldRenderRayTracingRestirGI())
	//{
	//	return;
	//}

	//PrepareRayTracingRestirGI(View, OutRayGenShaders);
	PrepareRayTracingDeferedGI(View, OutRayGenShaders);
	RayTracingDDIGIUpdate::PrepareRayTracingShaders(View, OutRayGenShaders);
}

void FDeferredShadingSceneRenderer::PrepareRayTracingGlobalIlluminationDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	PrepareRayTracingDeferredGIDeferredMaterial(View, OutRayGenShaders);
	
	const bool bSortMaterials = CVarRayTracingGlobalIlluminationFinalGatherSortMaterials.GetValueOnRenderThread() != 0;
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();

	if (!bSortMaterials)
	{
		return;
	}

	// Declare all RayGen shaders that require material closest hit shaders to be bound
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		FGlobalIlluminationRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGlobalIlluminationRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
		PermutationVector.Set<FGlobalIlluminationRGS::FEnableTransmissionDim>(EnableTransmission);
		TShaderMapRef<FGlobalIlluminationRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());

		// Gather
		{
			FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FPermutationDomain CreateGatherPointsPermutationVector;
			CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			CreateGatherPointsPermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
			TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS> CreateGatherPointsRayGenerationShader(View.ShaderMap, CreateGatherPointsPermutationVector);
			OutRayGenShaders.Add(CreateGatherPointsRayGenerationShader.GetRayTracingShader());
		}

	}
}

#endif // RHI_RAYTRACING

bool FDeferredShadingSceneRenderer::RenderRayTracingGlobalIllumination(
	FRDGBuilder& GraphBuilder, 
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig* OutRayTracingConfig,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
	FSurfelBufResources* SurfelRes,
	FRadianceVolumeProbeConfigs* RadianceProbeConfig)
#if RHI_RAYTRACING
{
	if (!View.ViewState) return false;

	int32 RayTracingGISamplesPerPixel = GetRayTracingGlobalIlluminationSamplesPerPixel(View);
	if (RayTracingGISamplesPerPixel <= 0) return false;

	OutRayTracingConfig->ResolutionFraction = 1.0;
	if (GRayTracingGlobalIlluminationDenoiser != 0)
	{
		OutRayTracingConfig->ResolutionFraction = FMath::Clamp(GRayTracingGlobalIlluminationScreenPercentage / 100.0, 0.25, 1.0);
	}

	OutRayTracingConfig->RayCountPerPixel = RayTracingGISamplesPerPixel;

	int32 UpscaleFactor = int32(1.0 / OutRayTracingConfig->ResolutionFraction);

	// Allocate input for the denoiser.
	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			SceneTextures.SceneDepthTexture->Desc.Extent / UpscaleFactor,
			PF_FloatRGBA,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);

		OutDenoiserInputs->Color = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingDiffuseIndirect"));

		Desc.Format = PF_G16R16;
		OutDenoiserInputs->RayHitDistance = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingDiffuseIndirectHitDistance"));
	}
	//int32 CVarRayTracingGlobalIlluminationValue = CVarRayTracingGlobalIllumination.GetValueOnRenderThread();
	if (IsRestirGIEnabled(View))
	{
		RenderRestirGI(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs);
	}
	else if (IsSurfelGIEnabled(View))
	{
		SurfelGI(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs, *SurfelRes);

		RenderWRC(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs, *RadianceProbeConfig);
	
		RenderRestirGI(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs,SurfelRes, RadianceProbeConfig);
	}
	// Ray generation pass
	else if (IsFinalGatherEnabled(View))
	{
		RenderRayTracingGlobalIlluminationFinalGather(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs);
	}
	else if (IsRayTracingDDGI(View))
	{
		RenderTestDDGI(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs);
	}
	else
	{
		if (IsDeferedGIEnabled(View))
		{
			RenderRayTracingDeferedGI(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs);
		}
		else
			RenderRayTracingGlobalIlluminationBruteForce(GraphBuilder, SceneTextures, View, *OutRayTracingConfig, UpscaleFactor, OutDenoiserInputs);
	}
	return true;
}
#else
{
	unimplemented();
	return false;
}
#endif // RHI_RAYTRACING

#if RHI_RAYTRACING
void CopyGatherPassParameters(
	const FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters& PassParameters,
	FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FParameters* NewParameters
)
{
	NewParameters->GatherSamplesPerPixel = PassParameters.GatherSamplesPerPixel;
	NewParameters->SamplesPerPixel = PassParameters.SamplesPerPixel;
	NewParameters->GatherPointIteration = PassParameters.GatherPointIteration;
	NewParameters->GatherFilterWidth = PassParameters.GatherFilterWidth;
	NewParameters->SampleIndex = PassParameters.SampleIndex;
	NewParameters->MaxBounces = PassParameters.MaxBounces;
	NewParameters->UpscaleFactor = PassParameters.UpscaleFactor;
	NewParameters->RenderTileOffsetX = PassParameters.RenderTileOffsetX;
	NewParameters->RenderTileOffsetY = PassParameters.RenderTileOffsetY;
	NewParameters->MaxRayDistanceForGI = PassParameters.MaxRayDistanceForGI;
	NewParameters->MaxShadowDistance = PassParameters.MaxShadowDistance;
	NewParameters->NextEventEstimationSamples = PassParameters.NextEventEstimationSamples;
	NewParameters->DiffuseThreshold = PassParameters.DiffuseThreshold;
	NewParameters->MaxNormalBias = PassParameters.MaxNormalBias;
	NewParameters->EvalSkyLight = PassParameters.EvalSkyLight;
	NewParameters->UseRussianRoulette = PassParameters.UseRussianRoulette;

	NewParameters->AccumulateTime = PassParameters.AccumulateTime;
	NewParameters->CumulativeTime = PassParameters.CumulativeTime;

	NewParameters->TLAS = PassParameters.TLAS;
	NewParameters->ViewUniformBuffer = PassParameters.ViewUniformBuffer;

	NewParameters->SceneLights = PassParameters.SceneLights;
	NewParameters->SceneLightCount = PassParameters.SceneLightCount;
	NewParameters->SkylightParameters = PassParameters.SkylightParameters;

	NewParameters->SceneTextures = PassParameters.SceneTextures;
	NewParameters->SSProfilesTexture = PassParameters.SSProfilesTexture;
	NewParameters->TransmissionProfilesLinearSampler = PassParameters.TransmissionProfilesLinearSampler;

	NewParameters->GatherPointsResolution = PassParameters.GatherPointsResolution;
	NewParameters->TileAlignedResolution = PassParameters.TileAlignedResolution;
	NewParameters->SortTileSize = PassParameters.SortTileSize;

	NewParameters->RWGatherPointsBuffer = PassParameters.RWGatherPointsBuffer;
	NewParameters->MaterialBuffer = PassParameters.MaterialBuffer;
}

void CopyGatherPassParameters(
	const FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters& PassParameters,
	FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters* NewParameters
)
{
	NewParameters->GatherSamplesPerPixel = PassParameters.GatherSamplesPerPixel;
	NewParameters->SamplesPerPixel = PassParameters.SamplesPerPixel;
	NewParameters->GatherPointIteration = PassParameters.GatherPointIteration;
	NewParameters->GatherFilterWidth = PassParameters.GatherFilterWidth;
	NewParameters->SampleIndex = PassParameters.SampleIndex;
	NewParameters->MaxBounces = PassParameters.MaxBounces;
	NewParameters->UpscaleFactor = PassParameters.UpscaleFactor;
	NewParameters->RenderTileOffsetX = PassParameters.RenderTileOffsetX;
	NewParameters->RenderTileOffsetY = PassParameters.RenderTileOffsetY;
	NewParameters->MaxRayDistanceForGI = PassParameters.MaxRayDistanceForGI;
	NewParameters->MaxShadowDistance = PassParameters.MaxShadowDistance;
	NewParameters->NextEventEstimationSamples = PassParameters.NextEventEstimationSamples;
	NewParameters->DiffuseThreshold = PassParameters.DiffuseThreshold;
	NewParameters->MaxNormalBias = PassParameters.MaxNormalBias;
	NewParameters->EvalSkyLight = PassParameters.EvalSkyLight;
	NewParameters->UseRussianRoulette = PassParameters.UseRussianRoulette;

	NewParameters->AccumulateTime = PassParameters.AccumulateTime;
	NewParameters->CumulativeTime = PassParameters.CumulativeTime;

	NewParameters->TLAS = PassParameters.TLAS;
	NewParameters->ViewUniformBuffer = PassParameters.ViewUniformBuffer;

	NewParameters->SceneLightCount = PassParameters.SceneLightCount;
	NewParameters->SceneLights = PassParameters.SceneLights;
	NewParameters->SkylightParameters = PassParameters.SkylightParameters;

	NewParameters->SceneTextures = PassParameters.SceneTextures;
	NewParameters->SSProfilesTexture = PassParameters.SSProfilesTexture;
	NewParameters->TransmissionProfilesLinearSampler = PassParameters.TransmissionProfilesLinearSampler;

	NewParameters->GatherPointsResolution = PassParameters.GatherPointsResolution;
	NewParameters->TileAlignedResolution = PassParameters.TileAlignedResolution;
	NewParameters->SortTileSize = PassParameters.SortTileSize;

	NewParameters->RWGatherPointsBuffer = PassParameters.RWGatherPointsBuffer;
	NewParameters->MaterialBuffer = PassParameters.MaterialBuffer;

	NewParameters->AccumulateTime = PassParameters.AccumulateTime;
	NewParameters->CumulativeTime = PassParameters.CumulativeTime;
}
#endif // RHI_RAYTRACING

void FDeferredShadingSceneRenderer::RayTracingGlobalIlluminationCreateGatherPoints(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	int32 UpscaleFactor,
	int32 SampleIndex,
	FRDGBufferRef& GatherPointsBuffer,
	FIntVector& GatherPointsResolution,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs
)
#if RHI_RAYTRACING
{	
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingGICreateGatherPoints);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: Create Gather Points");

	int32 GatherSamples = FMath::Min(GetRayTracingGlobalIlluminationSamplesPerPixel(View), MAXIMUM_GATHER_POINTS_PER_PIXEL);
	int32 SamplesPerPixel = 1;

	// Determine the local neighborhood for a shared sample sequence
	int32 GatherFilterWidth = FMath::Max(CVarRayTracingGlobalIlluminationFinalGatherFilterWidth.GetValueOnRenderThread(), 0);
	GatherFilterWidth = GatherFilterWidth * 2 + 1;


	float MaxShadowDistance = 1.0e27;
	if (GRayTracingGlobalIlluminationMaxShadowDistance > 0.0)
	{
		MaxShadowDistance = GRayTracingGlobalIlluminationMaxShadowDistance;
	}
	else if (Scene->SkyLight)
	{
		// Adjust ray TMax so shadow rays do not hit the sky sphere 
		MaxShadowDistance = FMath::Max(0.0, 0.99 * Scene->SkyLight->SkyDistanceThreshold);
	}

	FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters>();
	PassParameters->SampleIndex = SampleIndex;
	PassParameters->GatherSamplesPerPixel = GatherSamples;
	PassParameters->GatherPointIteration = 0;
	PassParameters->SamplesPerPixel = SamplesPerPixel;
	PassParameters->GatherFilterWidth = GatherFilterWidth;
	PassParameters->MaxBounces = 1;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	PassParameters->MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
	PassParameters->MaxShadowDistance = MaxShadowDistance;
	PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->RenderTileOffsetX = 0;
	PassParameters->RenderTileOffsetY = 0;
	PassParameters->AccumulateEmissive = FMath::Clamp(CVarRayTracingGlobalIlluminationAccumulateEmissive.GetValueOnRenderThread(), 0, 1);

	// Timing
	const int32 GITiming = VisualizeRayTracingTiming(View) ? CVarRayTracingGlobalIlluminationTiming.GetValueOnRenderThread() : 0;
	PassParameters->AccumulateTime = 0; // set per pass
	if (VisualizeRayTracingTiming(View) && (GITiming != 0))
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming));
	}
	else
	{
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance); // bogus UAV to just keep validation happy as it is dynamically unused
	}

	// Global
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	// Light data
	SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
	PassParameters->SceneTextures = SceneTextures;

	// Shading data
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Output
	FIntPoint DispatchResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);
	FIntVector LocalGatherPointsResolution(DispatchResolution.X, DispatchResolution.Y, GatherSamples);
	if (GatherPointsResolution != LocalGatherPointsResolution)
	{
		GatherPointsResolution = LocalGatherPointsResolution;
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGatherPoint), GatherPointsResolution.X * GatherPointsResolution.Y * GatherPointsResolution.Z);
		GatherPointsBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("GatherPointsBuffer"), ERDGBufferFlags::MultiFrame);
	}
	else
	{
		GatherPointsBuffer = GraphBuilder.RegisterExternalBuffer(((FSceneViewState*)View.State)->GatherPointsBuffer, TEXT("GatherPointsBuffer"));
	}
	PassParameters->GatherPointsResolution = FIntPoint(GatherPointsResolution.X, GatherPointsResolution.Y);
	PassParameters->RWGatherPointsBuffer = GraphBuilder.CreateUAV(GatherPointsBuffer, EPixelFormat::PF_R32_UINT);

	// When deferred materials are used, two passes are invoked:
	// 1) Gather ray-hit data and sort by hit-shader ID
	// 2) Re-trace "short" ray and shade
	const bool bSortMaterials = CVarRayTracingGlobalIlluminationFinalGatherSortMaterials.GetValueOnRenderThread() != 0;
	if (!bSortMaterials)
	{
		FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters* GatherPassParameters = PassParameters;
		//if (GatherPointIteration != 0)
		if (false)
		{
			GatherPassParameters = GraphBuilder.AllocParameters<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters>();
			CopyGatherPassParameters(*PassParameters, GatherPassParameters);
		}

		FRayTracingGlobalIlluminationCreateGatherPointsRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
		TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, GatherPassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GatherPoints %d%d", GatherPointsResolution.X, GatherPointsResolution.Y),
			GatherPassParameters,
			ERDGPassFlags::Compute,
			[GatherPassParameters, this, &View, RayGenerationShader, GatherPointsResolution, GITiming](FRHICommandList& RHICmdList)
		{
			if ( GITiming == 1 || GITiming == 4)
			{
				GatherPassParameters->AccumulateTime = 1;
			}
			
			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenerationShader, *GatherPassParameters);
			RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, GatherPointsResolution.X, GatherPointsResolution.Y);
		});
	}
	else
	{
		// Determines tile-size for sorted-deferred path
		const int32 SortTileSize = CVarRayTracingGlobalIlluminationFinalGatherSortTileSize.GetValueOnRenderThread();
		FIntPoint TileAlignedResolution = FIntPoint(GatherPointsResolution.X, GatherPointsResolution.Y);
		if (SortTileSize)
		{
			TileAlignedResolution = FIntPoint::DivideAndRoundUp(TileAlignedResolution, SortTileSize) * SortTileSize;
		}
		PassParameters->TileAlignedResolution = TileAlignedResolution;
		PassParameters->SortTileSize = SortTileSize;

		FRDGBufferRef DeferredMaterialBuffer = nullptr;
		const uint32 DeferredMaterialBufferNumElements = TileAlignedResolution.X * TileAlignedResolution.Y;

		// Gather pass
		{
			FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FParameters* GatherPassParameters = GraphBuilder.AllocParameters<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FParameters>();
			CopyGatherPassParameters(*PassParameters, GatherPassParameters);

			if (GITiming == 2 || GITiming == 4)
			{
				GatherPassParameters->AccumulateTime = 1;
			}

			FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDeferredMaterialPayload), DeferredMaterialBufferNumElements);
			DeferredMaterialBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("RayTracingGlobalIlluminationMaterialBuffer"));
			GatherPassParameters->MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);

			FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
			PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
			TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsTraceRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);

			ClearUnusedGraphResources(RayGenerationShader, GatherPassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GlobalIlluminationRayTracingGatherMaterials %dx%d", TileAlignedResolution.X, TileAlignedResolution.Y),
				GatherPassParameters,
				ERDGPassFlags::Compute,
				[GatherPassParameters, this, &View, RayGenerationShader, TileAlignedResolution](FRHICommandList& RHICmdList)
			{
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialGatherPipeline;

				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *GatherPassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, TileAlignedResolution.X, TileAlignedResolution.Y);
			});
		}

		// Sort by hit-shader ID
		const uint32 SortSize = CVarRayTracingGlobalIlluminationFinalGatherSortSize.GetValueOnRenderThread();
		SortDeferredMaterials(GraphBuilder, View, SortSize, DeferredMaterialBufferNumElements, DeferredMaterialBuffer);

		// Shade pass
		{
			FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters* GatherPassParameters = PassParameters;
			//if (GatherPointIteration != 0)
			if (false)
			{
				GatherPassParameters = GraphBuilder.AllocParameters<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FParameters>();
				CopyGatherPassParameters(*PassParameters, GatherPassParameters);
			}

			GatherPassParameters->MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);

			if (GITiming == 1 || GITiming == 4)
			{
				GatherPassParameters->AccumulateTime = 1;
			}

			FRayTracingGlobalIlluminationCreateGatherPointsRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
			PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
			PermutationVector.Set<FRayTracingGlobalIlluminationCreateGatherPointsRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
			TShaderMapRef<FRayTracingGlobalIlluminationCreateGatherPointsRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
			ClearUnusedGraphResources(RayGenerationShader, GatherPassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GlobalIlluminationRayTracingShadeMaterials %d", DeferredMaterialBufferNumElements),
				GatherPassParameters,
				ERDGPassFlags::Compute,
				[GatherPassParameters, this, &View, RayGenerationShader, DeferredMaterialBufferNumElements](FRHICommandList& RHICmdList)
			{
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *GatherPassParameters);

				// Shading pass for sorted materials uses 1D dispatch over all elements in the material buffer.
				// This can be reduced to the number of output pixels if sorting pass guarantees that all invalid entries are moved to the end.
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DeferredMaterialBufferNumElements, 1);
			});
		}
	}
}
#else
{
	unimplemented();
}
#endif

void FDeferredShadingSceneRenderer::RenderRayTracingGlobalIlluminationFinalGather(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	// Output
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
#if RHI_RAYTRACING
{
	int32 SamplesPerPixel = FMath::Min(GetRayTracingGlobalIlluminationSamplesPerPixel(View), MAXIMUM_GATHER_POINTS_PER_PIXEL);

	int32 GatherPointIterations = FMath::Max(CVarRayTracingGlobalIlluminationFinalGatherIterations.GetValueOnRenderThread(), 1);
	GatherPointIterations = FMath::Min(GatherPointIterations, SamplesPerPixel);

	// Generate gather points
	FRDGBufferRef GatherPointsBuffer;
	FSceneViewState* SceneViewState = (FSceneViewState*)View.State;
	int32 SampleIndex = View.ViewState->FrameIndex % ((SamplesPerPixel - 1) / GatherPointIterations + 1);
	SampleIndex *= GatherPointIterations;

	int GatherPointIteration = 0;
	do
	{
		int32 MultiSampleIndex = (SampleIndex + GatherPointIteration) % SamplesPerPixel;
		RayTracingGlobalIlluminationCreateGatherPoints(GraphBuilder, SceneTextures, View, UpscaleFactor, MultiSampleIndex, GatherPointsBuffer, SceneViewState->GatherPointsResolution, OutDenoiserInputs);
		GatherPointIteration++;
	} while (GatherPointIteration < GatherPointIterations);

	// Perform gather
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingGIFinalGather);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: Final Gather");

	FRayTracingGlobalIlluminationFinalGatherRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingGlobalIlluminationFinalGatherRGS::FParameters>();
	PassParameters->SampleIndex = SampleIndex;
	PassParameters->SamplesPerPixel = SamplesPerPixel;
	PassParameters->GatherPointIterations = GatherPointIterations;

	// Determine the local neighborhood for a shared sample sequence
	int32 GatherFilterWidth = FMath::Max(CVarRayTracingGlobalIlluminationFinalGatherFilterWidth.GetValueOnRenderThread(), 0);
	GatherFilterWidth = GatherFilterWidth * 2 + 1;
	PassParameters->GatherFilterWidth = GatherFilterWidth;
	PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;

	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	PassParameters->FinalGatherDistance = GRayTracingGlobalIlluminationFinalGatherDistance;
	PassParameters->DepthRejectionKernel = CVarRayTracingGlobalIlluminationFinalGatherDepthRejectionKernel.GetValueOnRenderThread();
	PassParameters->NormalRejectionKernel = CVarRayTracingGlobalIlluminationFinalGatherNormalRejectionKernel.GetValueOnRenderThread();
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->RenderTileOffsetX = 0;
	PassParameters->RenderTileOffsetY = 0;

	// Cache current view matrix for gather point reprojection
	for (int GatherPointIterationLocal = 0; GatherPointIterationLocal < GatherPointIterations; ++GatherPointIterationLocal)
	{
		int32 EntryIndex = (SampleIndex + GatherPointIterationLocal) % SamplesPerPixel;
		View.ViewState->GatherPointsViewHistory[EntryIndex] = View.ViewMatrices.GetViewProjectionMatrix();
	}

	// Build gather point reprojection buffer
	FGatherPointData GatherPointData;
	GatherPointData.Count = SamplesPerPixel;
	for (int ViewHistoryIndex = 0; ViewHistoryIndex < MAXIMUM_GATHER_POINTS_PER_PIXEL; ViewHistoryIndex++)
	{
		GatherPointData.ViewMatrices[ViewHistoryIndex] = View.ViewState->GatherPointsViewHistory[ViewHistoryIndex];
	}
	PassParameters->GatherPointData = CreateUniformBufferImmediate(GatherPointData, EUniformBufferUsage::UniformBuffer_SingleDraw);
	
	const int32 GITiming = CVarRayTracingGlobalIlluminationTiming.GetValueOnRenderThread();
	if (VisualizeRayTracingTiming(View) && (GITiming == 3 || GITiming == 4))
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
		PassParameters->AccumulateTime = 1;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming));
	}
	else
	{
		PassParameters->AccumulateTime = 0;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance); // bogus UAV to just keep validation happy as it is dynamically unused
	}

	// Scene data
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	// Shading data
	PassParameters->SceneTextures = SceneTextures;
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Gather points
	PassParameters->GatherPointsResolution = FIntPoint(SceneViewState->GatherPointsResolution.X, SceneViewState->GatherPointsResolution.Y);
	PassParameters->GatherPointsBuffer = GraphBuilder.CreateSRV(GatherPointsBuffer);

	// Output
	PassParameters->RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
	PassParameters->RWGlobalIlluminationRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);

	FRayTracingGlobalIlluminationFinalGatherRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRayTracingGlobalIlluminationFinalGatherRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
	PermutationVector.Set<FRayTracingGlobalIlluminationFinalGatherRGS::FEnableNeighborVisibilityTestDim>(CVarRayTracingGlobalIlluminationFinalGatherEnableNeighborVisbilityTest.GetValueOnRenderThread() != 0);
	TShaderMapRef<FRayTracingGlobalIlluminationFinalGatherRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
	ClearUnusedGraphResources(RayGenerationShader, PassParameters);

	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GlobalIlluminationRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, this, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
	{
		FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;

		FRayTracingShaderBindingsWriter GlobalResources;
		SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);
		RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
	});


	GraphBuilder.QueueBufferExtraction(GatherPointsBuffer, &SceneViewState->GatherPointsBuffer, ERHIAccess::SRVMask);
}
#else
{
	unimplemented();
}
#endif
extern LightTree GTree;
extern MeshLightTree	MeshTree;

DECLARE_GPU_STAT_NAMED(BuilCellReservoir, TEXT("BuilCellReservoir"));

void SetupMeshLightParamters(FScene* Scene,const FViewInfo& View, FRDGBuilder& GraphBuilder, FMeshLightCommonParameter* PassParameters,float UpscaleFactor,
	FBox& OutMeshLightBounds)
{
	///mesh light
	/// The following code should be excuted on the case of scene has been changed for efficiency, such as add meshlight, removemeshlight etc. like path tracer
	/// When the scene has many meshlight ,the construction of the whole position buffer will be expensive
	static TArray<FVector>	Positions;
	static TArray<uint32>	IndexList;
	static TArray<MeshLightInstanceTriangle>	meshLightTriangles;
	static TArray<MeshLightInstance>	meshLights;
	uint32 InstID = 0, indexOffset = 0;
	uint32 VertexOffset = 0;
	const float Inf = std::numeric_limits<float>::infinity();
	static FBox MeshLightBounds;
	if (Scene->MeshLightChaged)
	{
		Positions.Empty();
		IndexList.Empty();
		meshLightTriangles.Empty();
		meshLights.Empty();

		MeshLightBounds.Max = FVector(-Inf, -Inf, -Inf);
		MeshLightBounds.Min = FVector(Inf, Inf, Inf);
		FEmissiveLightMesh* CurrentEMesh = nullptr;
		for (auto LightProxIndex = 0; LightProxIndex < Scene->EmissiveLightProxies.Num(); LightProxIndex++)
		{
			const auto& lightProxy = Scene->EmissiveLightProxies[LightProxIndex];
			MeshLightInstance lightInstance;
			lightInstance.Transform = lightProxy->Transform;
			lightInstance.Emission = lightProxy->Emission;
			meshLights.Add(lightInstance);

			MeshLightBounds += lightProxy->Bounds;
			if (CurrentEMesh != lightProxy->EmissiveMesh)
			{
				CurrentEMesh = lightProxy->EmissiveMesh;
				for (const auto& p : CurrentEMesh->Positions)
				{
					Positions.Add(p);
				}
				for (int32 idx = 0; idx < CurrentEMesh->IndexList.Num(); idx++)
				{
					auto idx0 = CurrentEMesh->IndexList[idx] + VertexOffset;
					IndexList.Add(idx0);
				}

			}
			for (int32 i = 0; i < CurrentEMesh->IndexList.Num() / 3; i++)
			{
				MeshLightInstanceTriangle tri;
				tri.IndexOffset = i * 3 + indexOffset;
				tri.InstanceID = InstID;
				meshLightTriangles.Add(tri);
			}
			if (LightProxIndex < (Scene->EmissiveLightProxies.Num() - 1) && CurrentEMesh != Scene->EmissiveLightProxies[LightProxIndex + 1]->EmissiveMesh)
			{
				indexOffset += CurrentEMesh->IndexList.Num();
				VertexOffset += CurrentEMesh->Positions.Num();
			}
			InstID++;
		}
		Scene->MeshLightChaged = false;
	}
	static FStructuredBufferRHIRef MeshTriBuffer = nullptr;
	static FStructuredBufferRHIRef MeshPosBuffer = nullptr;
	static FStructuredBufferRHIRef MeshIndexBuffer = nullptr;
	static FStructuredBufferRHIRef MeshInstanceBuffer = nullptr;
	//if (Scene->MeshLightChaged)
	{
		{
			// Upload the buffer of lights to the GPU (send at least one)
			//meshTRi
			FRHIResourceCreateInfo CreateInfo_MeshLightTri;
			uint32 dataSize = FMath::Max<uint32>(meshLightTriangles.Num(), 1u) * sizeof(MeshLightInstanceTriangle);
			MeshTriBuffer = RHICreateStructuredBuffer(sizeof(MeshLightInstanceTriangle), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightTri);
			uint32 OffsetFloat = 0;
			void* BasePtr = RHILockStructuredBuffer(MeshTriBuffer, OffsetFloat, dataSize, RLM_WriteOnly);
			if (meshLightTriangles.Num() > 0)
				FPlatformMemory::Memcpy(BasePtr, meshLightTriangles.GetData(), dataSize);

			RHIUnlockStructuredBuffer(MeshTriBuffer);
		}

		{
			//meshPos
			FRHIResourceCreateInfo CreateInfo_MeshLightPos;
			uint32 dataSize = FMath::Max<uint32>(Positions.Num(), 1u) * sizeof(FVector);
			MeshPosBuffer = RHICreateStructuredBuffer(sizeof(FVector), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightPos);
			uint32 OffsetFloat = 0;
			void* BasePtr = RHILockStructuredBuffer(MeshPosBuffer, OffsetFloat, dataSize, RLM_WriteOnly);
			if (Positions.Num() > 0)
				FPlatformMemory::Memcpy(BasePtr, Positions.GetData(), dataSize);
			RHIUnlockStructuredBuffer(MeshPosBuffer);
		}

		{
			//Mesh Index
			FRHIResourceCreateInfo CreateInfo_MeshLightIndex;
			uint32 dataSize = FMath::Max<uint32>(IndexList.Num(), 1u) * sizeof(uint32);
			MeshIndexBuffer = RHICreateStructuredBuffer(sizeof(uint32), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightIndex);
			uint32 OffsetFloat = 0;
			void* BasePtr = RHILockStructuredBuffer(MeshIndexBuffer, OffsetFloat, dataSize, RLM_WriteOnly);
			if (IndexList.Num() > 0)
				FPlatformMemory::Memcpy(BasePtr, IndexList.GetData(), dataSize);
			RHIUnlockStructuredBuffer(MeshIndexBuffer);

		}

		{
			//Mesh Inst
			FRHIResourceCreateInfo CreateInfo_MeshLightInstance;
			uint32 dataSize = FMath::Max<uint32>(meshLights.Num(), 1u) * sizeof(MeshLightInstance);
			MeshInstanceBuffer = RHICreateStructuredBuffer(sizeof(MeshLightInstance), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightInstance);
			uint32 OffsetFloat = 0;
			void* BasePtr = RHILockStructuredBuffer(MeshInstanceBuffer, OffsetFloat, dataSize, RLM_WriteOnly);
			if (meshLights.Num() > 0)
				FPlatformMemory::Memcpy(BasePtr, meshLights.GetData(), dataSize);
			RHIUnlockStructuredBuffer(MeshInstanceBuffer);
		}

		//Scene->MeshLightChaged = false;
	}
	if ( !MeshTriBuffer)
	{
		{
			FRHIResourceCreateInfo CreateInfo_MeshLightTri;
			uint32 dataSize = sizeof(MeshLightInstanceTriangle);
			MeshTriBuffer = RHICreateStructuredBuffer(sizeof(MeshLightInstanceTriangle), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightTri);
		}
		{
			FRHIResourceCreateInfo CreateInfo_MeshLightPos;
			uint32 dataSize = 1u * sizeof(FVector);
			MeshPosBuffer = RHICreateStructuredBuffer(sizeof(FVector), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightPos);
		}
		{
			FRHIResourceCreateInfo CreateInfo_MeshLightIndex;
			uint32 dataSize = 1u * sizeof(uint32);
			MeshIndexBuffer = RHICreateStructuredBuffer(sizeof(uint32), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightIndex);
		}
		{
			FRHIResourceCreateInfo CreateInfo_MeshLightInstance;
			uint32 dataSize =  sizeof(MeshLightInstance);
			MeshInstanceBuffer = RHICreateStructuredBuffer(sizeof(MeshLightInstance), dataSize, BUF_Transient | BUF_FastVRAM | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo_MeshLightInstance);
		}
	}
	PassParameters->MeshLightInstancePrimitiveBuffer = RHICreateShaderResourceView(MeshTriBuffer);
	PassParameters->MeshLightInstanceBuffer = RHICreateShaderResourceView(MeshInstanceBuffer);
	PassParameters->MeshLightIndexBuffer = RHICreateShaderResourceView(MeshIndexBuffer);
	PassParameters->MeshLightVertexBuffer = RHICreateShaderResourceView(MeshPosBuffer);
	PassParameters->NumLightTriangles = meshLightTriangles.Num();

	MeshTree.Build(GraphBuilder,
		meshLightTriangles.Num(),
		MeshLightBounds.Min,
		MeshLightBounds.Max,
		PassParameters->MeshLightIndexBuffer,
		PassParameters->MeshLightVertexBuffer,
		PassParameters->MeshLightInstanceBuffer,
		PassParameters->MeshLightInstancePrimitiveBuffer);

	MeshTree.FindLightCuts(*Scene, View, GraphBuilder, MeshLightBounds.Min, MeshLightBounds.Max, 1.0 / UpscaleFactor);
	OutMeshLightBounds = MeshLightBounds;

}
void FDeferredShadingSceneRenderer::RenderRayTracingGlobalIlluminationBruteForce(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
#if RHI_RAYTRACING
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingGIBruteForce);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: Brute Force");

	int32 RayTracingGISamplesPerPixel = GetRayTracingGlobalIlluminationSamplesPerPixel(View);

	float MaxShadowDistance = 1.0e27;
	if (GRayTracingGlobalIlluminationMaxShadowDistance > 0.0)
	{
		MaxShadowDistance = GRayTracingGlobalIlluminationMaxShadowDistance;
	}
	else if (Scene->SkyLight)
	{
		// Adjust ray TMax so shadow rays do not hit the sky sphere 
		MaxShadowDistance = FMath::Max(0.0, 0.99 * Scene->SkyLight->SkyDistanceThreshold);
	}

	FGlobalIlluminationRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGlobalIlluminationRGS::FParameters>();
	PassParameters->SamplesPerPixel = RayTracingGISamplesPerPixel;
	int32 CVarRayTracingGlobalIlluminationMaxBouncesValue = CVarRayTracingGlobalIlluminationMaxBounces.GetValueOnRenderThread();
	PassParameters->MaxBounces = CVarRayTracingGlobalIlluminationMaxBouncesValue > -1? CVarRayTracingGlobalIlluminationMaxBouncesValue : View.FinalPostProcessSettings.RayTracingGIMaxBounces;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	float MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
	if (MaxRayDistanceForGI == -1.0)
	{
		MaxRayDistanceForGI = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	}
	PassParameters->MaxRayDistanceForGI = MaxRayDistanceForGI;
	PassParameters->MaxRayDistanceForAO = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	PassParameters->MaxShadowDistance = MaxShadowDistance;
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters,
		&PassParameters->LightGridParameters);

	GTree.Build(GraphBuilder, PassParameters->SceneLightCount, PassParameters->LightGridParameters.SceneInfiniteLightCount,PassParameters->LightGridParameters.SceneLightsBoundMin, PassParameters->LightGridParameters.SceneLightsBoundMax, PassParameters->SceneLights);
	
	GTree.FindLightCuts(*Scene, View, GraphBuilder, PassParameters->LightGridParameters.SceneLightsBoundMin, PassParameters->LightGridParameters.SceneLightsBoundMax,
		1.0 / UpscaleFactor);

	FMeshLightCommonParameter MeshLightCommonParam;
	FBox MeshLightBounds;
	SetupMeshLightParamters(Scene, View, GraphBuilder, &MeshLightCommonParam, UpscaleFactor, MeshLightBounds);
	PassParameters->MeshLightCommonParam = MeshLightCommonParam;

	FReGIRCommonParameters	CommonParameter;

	CommonParameter.LightBoundMax = MeshLightBounds.Max;
	CommonParameter.LightBoundMin = MeshLightBounds.Min;
	CommonParameter.NumCandidatesPerLightSlot = CVarNumCandidatePerLightSlot.GetValueOnRenderThread();
	CommonParameter.NumCandidatesPerCell = CVarNumCandidatePerCell.GetValueOnRenderThread();
	int32 NumLightSlotsPerCell = CVarNumLightSlotsPerCell.GetValueOnRenderThread();
	// pick the shortest axis
	FVector Diag = MeshLightBounds.Max - MeshLightBounds.Min;
	int LightGridResolution = CVarReGIRLightGridResolution.GetValueOnRenderThread();
	int LightGridAxis = 0;
	FIntVector GridCellDim, CellDispatchResolution;
	if (Diag.X < Diag.Y && Diag.X < Diag.Z)
	{
		LightGridAxis = 0;
		GridCellDim = FIntVector(1, LightGridResolution, LightGridResolution);
		CellDispatchResolution = FIntVector(NumLightSlotsPerCell, LightGridResolution, LightGridResolution);
	}
	else if (Diag.Y < Diag.Z)
	{
		LightGridAxis = 1;
		GridCellDim = FIntVector(LightGridResolution, 1, LightGridResolution);
		CellDispatchResolution = FIntVector(LightGridResolution, NumLightSlotsPerCell, LightGridResolution);
	}
	else
	{
		GridCellDim = FIntVector(LightGridResolution, LightGridResolution, 1);
		CellDispatchResolution = FIntVector(LightGridResolution, LightGridResolution, NumLightSlotsPerCell);
		LightGridAxis = 2;
	}
	//GridCellDim = FIntVector(LightGridResolution, LightGridResolution, LightGridResolution);
	//float GridCellSize = CVarReGIRLightGridCellSize.GetValueOnRenderThread();
	//FIntVector GridCellDim = FIntVector(FMath::CeilToInt(Diag.X / GridCellSize), FMath::CeilToInt(Diag.Y / GridCellSize), FMath::CeilToInt(Diag.Z / GridCellSize));

	CommonParameter.GridCellDim = GridCellDim;
	CommonParameter.NumLightSlotsPerCell = NumLightSlotsPerCell;
	FRDGBufferDesc ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(ReGIR_PackedReservoir), GridCellDim.X * GridCellDim.Y * GridCellDim.Z * NumLightSlotsPerCell );

	FRDGBufferRef LightReservoirs = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("ReGIRLightRReservoirs"));
	CommonParameter.RWLightReservoirUAV = GraphBuilder.CreateUAV(LightReservoirs);
	FRDGBufferDesc ReservoirHistoryDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(ReGIR_PackedReservoir), GridCellDim.X * GridCellDim.Y * GridCellDim.Z * NumLightSlotsPerCell);
	FRDGBufferRef ReGIRReservoirsHistory = GraphBuilder.CreateBuffer(ReservoirHistoryDesc, TEXT("ReGIRReservoirsHistory"));
	
	if (CVarLightSamplingType.GetValueOnRenderThread() > 1)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, BuilCellReservoir);
		RDG_EVENT_SCOPE(GraphBuilder, "BuilCellReservoir");
		TShaderMapRef<FBuildCellReservoirCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FBuildCellReservoirCS::FParameters* BuildPassParameters = GraphBuilder.AllocParameters<FBuildCellReservoirCS::FParameters>();
		uint32 numLightSlots = CommonParameter.NumLightSlotsPerCell * GridCellDim.X * GridCellDim.Y * GridCellDim.Z;

		
		BuildPassParameters->CommonParameters = CommonParameter;
		BuildPassParameters->MeshLightInstancePrimitiveBuffer = MeshLightCommonParam.MeshLightInstancePrimitiveBuffer;
		BuildPassParameters->MeshLightInstanceBuffer = MeshLightCommonParam.MeshLightInstanceBuffer;
		BuildPassParameters->MeshLightIndexBuffer = MeshLightCommonParam.MeshLightIndexBuffer;
		BuildPassParameters->MeshLightVertexBuffer = MeshLightCommonParam.MeshLightVertexBuffer;
		BuildPassParameters->NumLightTriangles = MeshLightCommonParam.NumLightTriangles;
		BuildPassParameters->LightGridAxis = LightGridAxis;
		BuildPassParameters->LightGridResolution = LightGridResolution;
		BuildPassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		BuildPassParameters->OutputSlice = 0;
		ClearUnusedGraphResources(ComputeShader, BuildPassParameters);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BuilCellReservoir"),
			ComputeShader,
			BuildPassParameters,
			FComputeShaderUtils::GetGroupCount(CellDispatchResolution, FIntVector(FBuildCellReservoirCS::GetThreadBlockSize())));
	}
	/*const bool bCameraCut = !(View.ViewState->FrameIndex >= 1) || View.bCameraCut;*/
	const bool bCameraCut = !View.PrevViewInfo.SampledReGIRHistory.IsValid() || View.bCameraCut;
	if (!bCameraCut && CVarLightSamplingType.GetValueOnRenderThread() > 1 && CVarReGIREnaleTemporal.GetValueOnRenderThread() > 0)
	{
		//RDG_GPU_STAT_SCOPE(GraphBuilder, BuilCellReservoir);
		//RDG_EVENT_SCOPE(GraphBuilder, "BuilCellReservoir");
		TShaderMapRef<FReGIRTemporalResamplingCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FReGIRTemporalResamplingCS::FParameters* BuildPassParameters = GraphBuilder.AllocParameters<FReGIRTemporalResamplingCS::FParameters>();
		uint32 numLightSlots = CommonParameter.NumLightSlotsPerCell * GridCellDim.X * GridCellDim.Y * GridCellDim.Z;

		BuildPassParameters->CommonParameters = CommonParameter;

		//BuildPassParameters->RWLightReservoirHistoryUAV = GraphBuilder.CreateUAV(ReGIRReservoirsHistory);

		BuildPassParameters->LightReservoirHistory = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(View.PrevViewInfo.SampledReGIRHistory.Reservoirs));
		BuildPassParameters->MeshLightInstancePrimitiveBuffer = MeshLightCommonParam.MeshLightInstancePrimitiveBuffer;
		BuildPassParameters->MeshLightInstanceBuffer = MeshLightCommonParam.MeshLightInstanceBuffer;
		BuildPassParameters->MeshLightIndexBuffer = MeshLightCommonParam.MeshLightIndexBuffer;
		BuildPassParameters->MeshLightVertexBuffer = MeshLightCommonParam.MeshLightVertexBuffer;
		BuildPassParameters->NumLightTriangles = MeshLightCommonParam.NumLightTriangles;
		BuildPassParameters->LightGridAxis = LightGridAxis;
		BuildPassParameters->LightGridResolution = LightGridResolution;
		BuildPassParameters->MaxTemporalHistory = CVarReGIRTemporalMaxHistory.GetValueOnRenderThread();
		BuildPassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		BuildPassParameters->OutputSlice = 0;
		BuildPassParameters->InputSlice = 0;
		ClearUnusedGraphResources(ComputeShader, BuildPassParameters);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BuilCellReservoir"),
			ComputeShader,
			BuildPassParameters,
			FComputeShaderUtils::GetGroupCount(CellDispatchResolution, FIntVector(FReGIRTemporalResamplingCS::GetThreadBlockSize())));
	}

	{
		TShaderMapRef<FWriteHistoryReservoirCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FWriteHistoryReservoirCS::FParameters* BuildPassParameters = GraphBuilder.AllocParameters<FWriteHistoryReservoirCS::FParameters>();
		uint32 numLightSlots = CommonParameter.NumLightSlotsPerCell * GridCellDim.X * GridCellDim.Y * GridCellDim.Z;

		BuildPassParameters->CommonParameters = CommonParameter;
		BuildPassParameters->RWLightReservoirHistoryUAV = GraphBuilder.CreateUAV(ReGIRReservoirsHistory);
		BuildPassParameters->OutputSlice = 0;
		BuildPassParameters->InputSlice = 0;
		ClearUnusedGraphResources(ComputeShader, BuildPassParameters);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("WriteHistoryReservoirBuffer"),
			ComputeShader,
			BuildPassParameters,
			FComputeShaderUtils::GetGroupCount(numLightSlots, FWriteHistoryReservoirCS::GetThreadBlockSize()));
	}
	PassParameters->ReGIRCommonParameters = CommonParameter;

	PassParameters->SceneTextures = SceneTextures;
	PassParameters->AccumulateEmissive = FMath::Clamp(CVarRayTracingGlobalIlluminationAccumulateEmissive.GetValueOnRenderThread(), 0, 1);

	const int32 GITiming = CVarRayTracingGlobalIlluminationTiming.GetValueOnRenderThread();
	if (VisualizeRayTracingTiming(View) && (GITiming == 1 || GITiming == 4))
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
		PassParameters->AccumulateTime = 1;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming));
	}
	else
	{
		PassParameters->AccumulateTime = 0;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance); // bogus UAV to just keep validation happy as it is dynamically unused
	}

	// TODO: should be converted to RDG
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*) GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
	PassParameters->RWGlobalIlluminationRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);
	PassParameters->RenderTileOffsetX = 0;
	PassParameters->RenderTileOffsetY = 0;
	FLightCutCommonParameter	LightCutCommonParameters;
	LightCutCommonParameters.CutShareGroupSize = GetCVarCutBlockSize().GetValueOnRenderThread();
	LightCutCommonParameters.MaxCutNodes = GetMaxCutNodes();
	LightCutCommonParameters.ErrorLimit = GetCVarErrorLimit().GetValueOnRenderThread();
	LightCutCommonParameters.UseApproximateCosineBound = GetCVarUseApproximateCosineBound().GetValueOnRenderThread();
	LightCutCommonParameters.InterleaveRate = GetCVarInterleaveRate().GetValueOnRenderThread();
	PassParameters->LightCutCommonParameters = LightCutCommonParameters;
	PassParameters->LightCutBuffer = GraphBuilder.CreateSRV(GTree.LightCutBuffer);
	PassParameters->NodesBuffer = GraphBuilder.CreateSRV(GTree.LightNodesBuffer);
	PassParameters->DistanceType = GetCVarLightTreeDistanceType().GetValueOnRenderThread();
	PassParameters->LeafStartIndex = GTree.GetLeafStartIndex();
	PassParameters->MeshLightLeafStartIndex = MeshTree.GetLeafStartIndex();
	PassParameters->MeshLightCutBuffer = GraphBuilder.CreateSRV(MeshTree.LightCutBuffer);
	PassParameters->MeshLightNodesBuffer = GraphBuilder.CreateSRV(MeshTree.LightNodesBuffer);
	FGlobalIlluminationRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGlobalIlluminationRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
	PermutationVector.Set<FGlobalIlluminationRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
	PermutationVector.Set<FGlobalIlluminationRGS::FLightSamplingTypeDim>((ELightSamplingType)CVarLightSamplingType.GetValueOnRenderThread());
	TShaderMapRef<FGlobalIlluminationRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
	ClearUnusedGraphResources(RayGenerationShader, PassParameters);

	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);
	//FIntPoint RayTracingResolution = View.ViewRect.Size();
	if (GRayTracingGlobalIlluminationRenderTileSize <= 0)
	{
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GlobalIlluminationRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, this, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
		{
			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;

			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);
			RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
		});
	}
	else
	{
		int32 RenderTileSize = FMath::Max(32, GRayTracingGlobalIlluminationRenderTileSize);
		int32 NumTilesX = FMath::DivideAndRoundUp(RayTracingResolution.X, RenderTileSize);
		int32 NumTilesY = FMath::DivideAndRoundUp(RayTracingResolution.Y, RenderTileSize);
		for (int32 Y = 0; Y < NumTilesY; ++Y)
		{
			for (int32 X = 0; X < NumTilesX; ++X)
			{
				FGlobalIlluminationRGS::FParameters* TilePassParameters = PassParameters;

				if (X > 0 || Y > 0)
				{
					TilePassParameters = GraphBuilder.AllocParameters<FGlobalIlluminationRGS::FParameters>();
					*TilePassParameters = *PassParameters;

					TilePassParameters->RenderTileOffsetX = X * RenderTileSize;
					TilePassParameters->RenderTileOffsetY = Y * RenderTileSize;
				}

				int32 DispatchSizeX = FMath::Min<int32>(RenderTileSize, RayTracingResolution.X - TilePassParameters->RenderTileOffsetX);
				int32 DispatchSizeY = FMath::Min<int32>(RenderTileSize, RayTracingResolution.Y - TilePassParameters->RenderTileOffsetY);

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GlobalIlluminationRayTracing %dx%d (tile %dx%d)", DispatchSizeX, DispatchSizeY, X, Y),
					TilePassParameters,
					ERDGPassFlags::Compute,
					[TilePassParameters, this, &View, RayGenerationShader, DispatchSizeX, DispatchSizeY](FRHICommandList& RHICmdList)
				{
					FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;

					FRayTracingShaderBindingsWriter GlobalResources;
					SetShaderParameters(GlobalResources, RayGenerationShader, *TilePassParameters);
					RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, 
						GlobalResources, DispatchSizeX, DispatchSizeY);
					RHICmdList.SubmitCommandsHint();
				});
			}
		}
	}
	GTree.VisualizeNodesLevel(*Scene, View, GraphBuilder);
	MeshTree.VisualizeNodesLevel(*Scene, View, GraphBuilder);

	if (CVarLightSamplingType.GetValueOnRenderThread() > 1)
	{
		if (!View.bStatePrevViewInfoIsReadOnly)
		{
			//Extract history feedback here
			GraphBuilder.QueueBufferExtraction(ReGIRReservoirsHistory, &View.ViewState->PrevFrameViewInfo.SampledReGIRHistory.Reservoirs);

		}
	}
}
#else
{
	unimplemented();
}
#endif

void FDeferredShadingSceneRenderer::RenderTestDDGI(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
#if RHI_RAYTRACING
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingDDGI);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: DDGI+");
	//if (!View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, RTDDGI_Update);
		RDG_EVENT_SCOPE(GraphBuilder, "RTDDGI Update");
		RayTracingDDIGIUpdate::DDGIUpdatePerFrame_RenderThread(*Scene, View, GraphBuilder);
	}

	// DDGIVolume and useful metadata
	struct FProxyEntry
	{
		FVector Position;
		FQuat Rotation;
		FVector Scale;
		float Density;
		uint32 lightingChannelMask;
		const FTestGIVolumeSceneProxy* proxy;
	};

	// Find all the volumes that intersect the view frustum
	TArray<FProxyEntry> volumes;
	for (FTestGIVolumeSceneProxy* volumeProxy : Scene->TestGIProxies)
	{
		// Skip this volume if it belongs to another scene
		if (volumeProxy->OwningScene != Scene) continue;

		// Skip this volume if it is not enabled
		if (!volumeProxy->ComponentData.EnableVolume) continue;

		// Skip this volume if it doesn't intersect the view frustum
		if (!volumeProxy->IntersectsViewFrustum(View.ViewMatrices, View.GetViewDirection(), View.ViewFrustum, View.SceneViewInitOptions.OverrideFarClippingPlaneDistance) )continue;

		// Get the volume position, rotation, and scale
		FVector ProxyPosition = volumeProxy->ComponentData.Origin;
		FQuat   ProxyRotation = volumeProxy->ComponentData.Transform.GetRotation();
		FVector ProxyScale = volumeProxy->ComponentData.Transform.GetScale3D();

		float ProxyDensity = float(volumeProxy->ComponentData.ProbeCounts.X * volumeProxy->ComponentData.ProbeCounts.Y * volumeProxy->ComponentData.ProbeCounts.Z) / (ProxyScale.X * ProxyScale.Y * ProxyScale.Z);
		uint32 ProxyLightingChannelMask =
			(volumeProxy->ComponentData.LightingChannels.bChannel0 ? 1 : 0) |
			(volumeProxy->ComponentData.LightingChannels.bChannel1 ? 2 : 0) |
			(volumeProxy->ComponentData.LightingChannels.bChannel2 ? 4 : 0);

		// Add the current volume to the list of in-frustum volumes
		volumes.Add(FProxyEntry{ ProxyPosition, ProxyRotation, ProxyScale, ProxyDensity, ProxyLightingChannelMask, volumeProxy });
	}

	// Early out if no volumes contribute light to the current view
	if (volumes.Num() == 0) return;

	// TODO: manage in-frustum volumes in a more sophisticated way
	// Support a large number of volumes by culling volumes based on spatial data, projected view area, and/or other heuristics

	// Sort the in-frustum volumes by user specified priority and probe density
	Algo::Sort(volumes, [](const FProxyEntry& A, const FProxyEntry& B)
		{
			if (A.proxy->ComponentData.LightingPriority < B.proxy->ComponentData.LightingPriority) return true;
			if ((A.proxy->ComponentData.LightingPriority == B.proxy->ComponentData.LightingPriority) && (A.Density > B.Density)) return true;
			return false;
		});

	// Get the number of relevant in-frustum volumes
	int32 numVolumes = FMath::Min(volumes.Num(), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_MAX_SHADING_VOLUMES);

	// Truncate the in-frustum volumes list to the maximum number of volumes supported
	volumes.SetNum(numVolumes, true);

	// Sort the final volume list by descending probe density
	Algo::Sort(volumes, [](const FProxyEntry& A, const FProxyEntry& B)
		{
			return (A.Density > B.Density);
		});


	int32 RayTracingGISamplesPerPixel = GetRayTracingGlobalIlluminationSamplesPerPixel(View);

	float MaxShadowDistance = 1.0e27;
	if (GRayTracingGlobalIlluminationMaxShadowDistance > 0.0)
	{
		MaxShadowDistance = GRayTracingGlobalIlluminationMaxShadowDistance;
	}
	else if (Scene->SkyLight)
	{
		// Adjust ray TMax so shadow rays do not hit the sky sphere 
		MaxShadowDistance = FMath::Max(0.0, 0.99 * Scene->SkyLight->SkyDistanceThreshold);
	}

	bool enableRelocation = true;
	bool enableScrolling = false;

	FGlobalIlluminationDDGIRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGlobalIlluminationDDGIRGS::FParameters>();
	PassParameters->SamplesPerPixel = RayTracingGISamplesPerPixel;
	int32 CVarRayTracingGlobalIlluminationMaxBouncesValue = CVarRayTracingGlobalIlluminationMaxBounces.GetValueOnRenderThread();
	PassParameters->MaxBounces = CVarRayTracingGlobalIlluminationMaxBouncesValue > -1 ? CVarRayTracingGlobalIlluminationMaxBouncesValue : View.FinalPostProcessSettings.RayTracingGIMaxBounces;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	float MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
	if (MaxRayDistanceForGI == -1.0)
	{
		MaxRayDistanceForGI = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	}
	PassParameters->MaxRayDistanceForGI = MaxRayDistanceForGI;
	PassParameters->MaxRayDistanceForAO = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	PassParameters->MaxShadowDistance = MaxShadowDistance;
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
	PassParameters->SceneTextures = SceneTextures;
	PassParameters->AccumulateEmissive = FMath::Clamp(CVarRayTracingGlobalIlluminationAccumulateEmissive.GetValueOnRenderThread(), 0, 1);

	// TODO: should be converted to RDG
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
	PassParameters->RWGlobalIlluminationRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);
	PassParameters->RenderTileOffsetX = 0;
	PassParameters->RenderTileOffsetY = 0;
	PassParameters->NumVolumes = numVolumes;
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		SceneTextures.SceneDepthTexture->Desc.Extent / UpscaleFactor,
		PF_FloatRGBA,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);

	auto DebugTex = GraphBuilder.CreateTexture(Desc, TEXT("DebugInDirectDiffuse"));

	PassParameters->RWDebugUAV = GraphBuilder.CreateUAV(DebugTex);
	
	//FIntVector GuidingBufferDim = FIntVector(Desc.Extent.X, Desc.Extent.Y, 1);
	//FRDGBufferDesc RayGuidingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(GuidingEntry), GuidingBufferDim.X * GuidingBufferDim.Y * GuidingBufferDim.Z);
	//FRDGBufferRef GIGuidingBuffer = GraphBuilder.CreateBuffer(RayGuidingDesc, TEXT("RayGuidingLuminance"));
	//FRDGBufferRef GIMaxQBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32_t), GuidingBufferDim.X * GuidingBufferDim.Y * GuidingBufferDim.Z), TEXT("RayGuidingMaxQ"));

	//PassParameters->RWRayGuidingEntries = GraphBuilder.CreateUAV(GIGuidingBuffer);
	//PassParameters->RWRayGuidingMaxQ = GraphBuilder.CreateUAV(GIMaxQBuffer);

	// Set the shader parameters for the relevant volumes
	for (int32 volumeIndex = 0; volumeIndex < numVolumes; ++volumeIndex)
	{
		FProxyEntry volume = volumes[volumeIndex];
		const FTestGIVolumeSceneProxy* volumeProxy = volume.proxy;

		// Set the volume textures
		PassParameters->DDGIVolume[volumeIndex].ProbeIrradiance = GraphBuilder.RegisterExternalTexture(volumeProxy->ProbesIrradiance);
		PassParameters->DDGIVolume[volumeIndex].ProbeDistance = GraphBuilder.RegisterExternalTexture(volumeProxy->ProbesDistance);
		PassParameters->DDGIVolume[volumeIndex].ProbeOffsets = RegisterExternalTextureWithFallback(GraphBuilder, volumeProxy->ProbesOffsets, GSystemTextures.BlackDummy);
		PassParameters->DDGIVolume[volumeIndex].ProbeStates = RegisterExternalTextureWithFallback(GraphBuilder, volumeProxy->ProbesStates, GSystemTextures.BlackDummy);

		// Set the volume parameters
		PassParameters->DDGIVolume[volumeIndex].Position = volumeProxy->ComponentData.Origin;
		PassParameters->DDGIVolume[volumeIndex].Rotation = FVector4(volume.Rotation.X, volume.Rotation.Y, volume.Rotation.Z, volume.Rotation.W);
		PassParameters->DDGIVolume[volumeIndex].Radius = volume.Scale * 100.0f;
		PassParameters->DDGIVolume[volumeIndex].LightingChannelMask = volume.lightingChannelMask;

		FVector volumeSize = volumeProxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(volumeProxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(volumeProxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(volumeProxy->ComponentData.ProbeCounts.Z);

		PassParameters->DDGIVolume[volumeIndex].ProbeGridSpacing = probeGridSpacing;
		PassParameters->DDGIVolume[volumeIndex].ProbeGridCounts = volumeProxy->ComponentData.ProbeCounts;
		PassParameters->DDGIVolume[volumeIndex].ProbeNumIrradianceTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance;
		PassParameters->DDGIVolume[volumeIndex].ProbeNumDistanceTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance;
		PassParameters->DDGIVolume[volumeIndex].ProbeIrradianceEncodingGamma = volumeProxy->ComponentData.ProbeIrradianceEncodingGamma;
		PassParameters->DDGIVolume[volumeIndex].NormalBias = volumeProxy->ComponentData.NormalBias;
		PassParameters->DDGIVolume[volumeIndex].ViewBias = volumeProxy->ComponentData.ViewBias;
		PassParameters->DDGIVolume[volumeIndex].BlendDistance = volumeProxy->ComponentData.BlendDistance;
		PassParameters->DDGIVolume[volumeIndex].BlendDistanceBlack = volumeProxy->ComponentData.BlendDistanceBlack;
		PassParameters->DDGIVolume[volumeIndex].ProbeScrollOffsets = volumeProxy->ComponentData.ProbeScrollOffsets;

		// Only apply lighting if this is the pass it should be applied in
		// The shader needs data for all of the volumes for blending purposes
		bool applyLighting = true;
		applyLighting = applyLighting && (enableRelocation == volumeProxy->ComponentData.EnableProbeRelocation);
		applyLighting = applyLighting && (enableScrolling == volumeProxy->ComponentData.EnableProbeScrolling);
		PassParameters->DDGIVolume[volumeIndex].ApplyLighting = applyLighting;
		PassParameters->DDGIVolume[volumeIndex].IrradianceScalar = volumeProxy->ComponentData.IrradianceScalar;

		// Apply the lighting multiplier to artificially lighten or darken the indirect light from the volume
		PassParameters->DDGIVolume[volumeIndex].IrradianceScalar /= volumeProxy->ComponentData.LightingMultiplier;
		PassParameters->DDGIVolumeRayDataUAV = volumeProxy->ProbesRadianceUAV;
		PassParameters->DDGIVolume[volumeIndex].ProbeNumRays = volumeProxy->ComponentData.GetNumRaysPerProbe();
		PassParameters->DDGIVolume[volumeIndex].RayRotationTransform = volumeProxy->ComponentData.RayRotationTransform;
	}

	// When there are fewer relevant volumes than the maximum supported, set the empty volume texture slots to dummy values
	for (int32 volumeIndex = numVolumes; volumeIndex < FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_MAX_SHADING_VOLUMES; ++volumeIndex)
	{
		PassParameters->DDGIVolume[volumeIndex].ProbeIrradiance = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		PassParameters->DDGIVolume[volumeIndex].ProbeDistance = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		PassParameters->DDGIVolume[volumeIndex].ProbeOffsets = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		PassParameters->DDGIVolume[volumeIndex].ProbeStates = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
	}

	bool highBitCount = false;
	FGlobalIlluminationDDGIRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
	//PermutationVector.Set<FGlobalIlluminationDDGIRGS::FLightingChannelsDim>(false);
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableRelocation>(enableRelocation);
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FEnableScrolling>(enableScrolling);
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FFormatRadiance>(highBitCount);
	PermutationVector.Set<FGlobalIlluminationDDGIRGS::FFormatIrradiance>(highBitCount);
	TShaderMapRef<FGlobalIlluminationDDGIRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);

	ClearUnusedGraphResources(RayGenerationShader, PassParameters);

	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);
	//FIntPoint RayTracingResolution = View.ViewRect.Size();

	{
		

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GlobalIlluminationRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, this, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
			{
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;

				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
			});
	}
}
#else
{
	unimplemented();
}

#endif // RHI_RAYTRACING