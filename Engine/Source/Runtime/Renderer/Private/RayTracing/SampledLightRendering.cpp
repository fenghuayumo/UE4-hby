/*
* Copyright (c) 2021 NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA Corporation and its licensors retain all intellectual property and proprietary
* rights in and to this software, related documentation and any modifications thereto.
* Any use, reproduction, disclosure or distribution of this software and related
* documentation without an express license agreement from NVIDIA Corporation is strictly
* prohibited.
*
* TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS*
* AND NVIDIA AND ITS SUPPLIERS DISCLAIM ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
* PARTICULAR PURPOSE.  IN NO EVENT SHALL NVIDIA OR ITS SUPPLIERS BE LIABLE FOR ANY
* SPECIAL, INCIDENTAL, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER (INCLUDING, WITHOUT
* LIMITATION, DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF
* BUSINESS INFORMATION, OR ANY OTHER PECUNIARY LOSS) ARISING OUT OF THE USE OF OR
* INABILITY TO USE THIS SOFTWARE, EVEN IF NVIDIA HAS BEEN ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGES.
*/

/*=============================================================================
	SampledLightRendering.cpp: Light rendering implementation.
=============================================================================*/

#include "LightRendering.h"
#include "RendererModule.h"
#include "DeferredShadingRenderer.h"
#include "LightPropagationVolume.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "Engine/SubsurfaceProfile.h"
#include "ShowFlags.h"
#include "VisualizeTexture.h"
#include "RayTracing/RaytracingOptions.h"
#include "SceneTextureParameters.h"
#include "HairStrands/HairStrandsRendering.h"
#include "ScreenPass.h"
#include "SkyAtmosphereRendering.h"

#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"

#if RHI_RAYTRACING
#include "RayTracing/RayTracingLighting.h"

static TAutoConsoleVariable<int32> CVarSampledLightingDenoiser(
	TEXT("r.RayTracing.SampledLighting.Denoiser"),
	2,
	TEXT("Choose the denoising algorithm.\n")
	TEXT(" 0: Disabled ;\n")
	TEXT(" 1: Forces the default denoiser of the renderer;\n")
	TEXT(" 2: GScreenSpaceDenoiser whitch may be overriden by a third party plugin. This needs the NRD denoiser plugin to work correctly (default)\n"),
	ECVF_RenderThreadSafe);


static TAutoConsoleVariable<int32> CVarSampledLightingCompositeDiffuse(
	TEXT("r.RayTracing.SampledLighting.CompositeDiffuse"), 1,
	TEXT("Whether to composite the diffuse signal"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingCompositeSpecular(
	TEXT("r.RayTracing.SampledLighting.CompositeSpecular"), 1,
	TEXT("Whether to composite the specular signal"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingCompositeMode(
	TEXT("r.RayTracing.SampledLighting.CompositeMode"), 0,
	TEXT("How to composite the signal (add = 0, replace = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingMode(
	TEXT("r.RayTracing.SampledLighting.Mode"), 1,
	TEXT("Which mode to process sampled lighting with\n")
	TEXT("  0 - monolithic single pass \n")
	TEXT("  1 - multipass ReSTIRs style (Default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingSpatial(
	TEXT("r.RayTracing.SampledLighting.Spatial"), 1,
	TEXT("Whether to apply spatial resmapling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingInitialCandidates(
	TEXT("r.RayTracing.SampledLighting.InitialSamples"), 4,
	TEXT("How many lights to test sample during the initial candidate search"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingInitialCandidatesBoost(
	TEXT("r.RayTracing.SampledLighting.InitialSamplesBoost"), 32,
	TEXT("How many lights to test sample during the initial candidate search when history is invalidated"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingTemporal(
	TEXT("r.RayTracing.SampledLighting.Temporal"), 1,
	TEXT("Whether to use temporal resampling for the reserviors"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingApplyBoilingFilter(
	TEXT("r.RayTracing.SampledLighting.ApplyBoilingFilter"), 1,
	TEXT("Whether to apply boiling filter when temporally resampling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingBoilingFilterStrength(
	TEXT("r.RayTracing.SampledLighting.BoilingFilterStrength"), 0.20f,
	TEXT("Strength of Boiling filter"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingSpatialSamplingRadius(
	TEXT("r.RayTracing.SampledLighting.Spatial.SamplingRadius"), 32.0f,
	TEXT("Spatial radius for sampling in pixels (Default 32.0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingSpatialSamples(
	TEXT("r.RayTracing.SampledLighting.Spatial.Samples"), 1,
	TEXT("Spatial samples per pixel"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingSpatialSamplesBoost(
	TEXT("r.RayTracing.SampledLighting.Spatial.SamplesBoost"), 8,
	TEXT("Spatial samples per pixel when invalid history is detected"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingSpatialNormalRejectionThreshold(
	TEXT("r.RayTracing.SampledLighting.Spatial.NormalRejectionThreshold"), 0.5f,
	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingSpatialDepthRejectionThreshold(
	TEXT("r.RayTracing.SampledLighting.Spatial.DepthRejectionThreshold"), 0.1f,
	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingSpatialApplyApproxVisibility(
	TEXT("r.RayTracing.SampledLighting.Spatial.ApplyApproxVisibility"), 0,
	TEXT("Apply an approximate visibility test on sample selected during spatial sampling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingTemporalMaxHistory(
	TEXT("r.RayTracing.SampledLighting.Temporal.MaxHistory"), 10,
	TEXT("Maximum temporal history for samples (default 10)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingTemporalNormalRejectionThreshold(
	TEXT("r.RayTracing.SampledLighting.Temporal.NormalRejectionThreshold"), 0.5f,
	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSampledLightingTemporalDepthRejectionThreshold(
	TEXT("r.RayTracing.SampledLighting.Temporal.DepthRejectionThreshold"), 0.1f,
	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingTemporalApplyApproxVisibility(
	TEXT("r.RayTracing.SampledLighting.Temporal.ApplyApproxVisibility"), 0,
	TEXT("Apply an approximate visibility test on sample selected during reprojection"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingDemodulateMaterials(
	TEXT("r.RayTracing.SampledLighting.DemodulateMaterials"), 1,
	TEXT("Whether to demodulate the material contributiuon from the signal for denoising"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingFaceCull(
	TEXT("r.RayTracing.SampledLighting.FaceCull"), 0,
	TEXT("Face culling to use for visibility tests\n")
	TEXT("  0 - none (Default)\n")
	TEXT("  1 - front faces (equivalent to backface culling in shadow maps)\n")
	TEXT("  2 - back faces"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingApproximateVisibilityMode(
	TEXT("r.RayTracing.SampledLighting.ApproximateVisibilityMode"), 0,
	TEXT("Visibility mode for approximate visibility tests (default 0/accurate)\n")
	TEXT("  0 - Accurate, any hit shaders process alpha coverage\n")
	TEXT("  1 - Force opaque, anyhit shaders ignored, alpha coverage considered 100%\n")
	TEXT("  2 - Force transparent, anyhit shaders ignored, alpha coverage considered 0%"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingNumReservoirs(
	TEXT("r.RayTracing.SampledLighting.NumReservoirs"), -1,
	TEXT("Number of independent light reservoirs per pixel\n")
	TEXT("  1-N - Explicit number of reservoirs\n")
	TEXT("  -1 - Auto-select based on subsampling (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingMinReservoirs(
	TEXT("r.RayTracing.SampledLighting.MinReservoirs"), 1,
	TEXT("Minimum number of light reservoirs when auto-seleting(default 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingMaxReservoirs(
	TEXT("r.RayTracing.SampledLighting.MaxReservoirs"), 2,
	TEXT("Maximum number of light reservoirs when auto-seleting (default 2)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLightingFusedSampling(
	TEXT("r.RayTracing.SampledLighting.FusedSampling"), 1,
	TEXT("Whether to fuse initial candidate and temporal sampling (default 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSampledLighting(
	TEXT("r.RayTracing.SampledDirectLighting"), 0,
	TEXT("Whether to use sampling for evaluating direct lighting"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingDirectionalLight(
	TEXT("r.RayTracing.SampledLighting.Lights.Directional"),
	1,
	TEXT("Enables ray traced sampled lighting for directional lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingPointLight(
	TEXT("r.RayTracing.SampledLighting.Lights.Point"),
	1,
	TEXT("Enables ray traced sampled lighting for point lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingSpotLight(
	TEXT("r.RayTracing.SampledLighting.Lights.Spot"),
	1,
	TEXT("Enables ray traced sampled lighting for spot lights (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingRectLight(
	TEXT("r.RayTracing.SampledLighting.Lights.Rect"),
	1,
	TEXT("Enables ray traced sampled lighting for rect light (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingFunctionLights(
	TEXT("r.RayTracing.SampledLighting.Lights.FunctionLights"),
	1,
	TEXT("Enables ray traced sampled lighting forlights with light functions (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingDebugMode(
	TEXT("r.RayTracing.SampledLighting.DebugMode"),
	0,
	TEXT("Debug visualization mode (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingFeedbackVisibility(
	TEXT("r.RayTracing.SampledLighting.FeedbackVisibility"),
	1,
	TEXT("Whether to feedback the final visibility result to the history (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingTestInitialVisibility(
	TEXT("r.RayTracing.SampledLighting.TestInitialVisibility"),
	1,
	TEXT("Test initial samples for visibility (default = 1)\n")
	TEXT("  0 - Do not test visibility during inital sampling\n")
	TEXT("  1 - Test visibility on final merged reservoir  (default)\n")
	TEXT("  2 - Test visibility on reservoirs prior to merging\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingEnableHairVoxel(
	TEXT("r.RayTracing.SampledLighting.EnableHairVoxel"),
	1,
	TEXT("Whether to test hair voxels for visibility when evaluating (default = 1)\n"),
	ECVF_RenderThreadSafe);

//
// CVars controlling shader permutations
//
static TAutoConsoleVariable<int32> CVarRayTracingSampledLightingEvaluationMode(
	TEXT("r.RayTracing.SampledLighting.Permute.EvaluationMode"),
	1,
	TEXT("Method for computing the light estimate used for driving sampling\n")
	TEXT("  0 - Use standard integrated lighting via the GetDynamicLightingSplit function, similar to raster\n")
	TEXT("  1 - Use sampled lighting like the path tracer (default)"),
	ECVF_RenderThreadSafe);


struct FSampledLightingPresets
{
	int32 CorrectionMode;
	int32 SpatialSamples;
	int32 InitialSamples;
	int32 DisocclusionSamples;
};

static const FSampledLightingPresets SampledLightingPresets[] =
{
	{ 0, 1, 4, 8},
	{ 1, 1, 4, 16},
	{ 1, 4, 8, 16}
};

static FAutoConsoleCommand GSendRemoteTalkersToEndpointCommand(
	TEXT("r.RayTracing.SampledLighting.Preset"),
	TEXT("Command applies preset quality levels for sampled lighting\n")
	TEXT("  Available levels: medium, high, ultra"),
	FConsoleCommandWithArgsDelegate::CreateStatic(
		[](const TArray< FString >& Args)
{
	int32 QualityLevel = -1;
	if (Args.Num() == 1)
	{
		if (Args[0] == TEXT("medium"))
		{
			QualityLevel = 0;
		}
		if (Args[0] == TEXT("high"))
		{
			QualityLevel = 1;
		}
		if (Args[0] == TEXT("ultra"))
		{
			QualityLevel = 2;
		}
	}

	if (QualityLevel == -1)
	{
		UE_LOG(LogRenderer, Display, TEXT("Invalid arguments for setting sampled lighting presets (options: medium, high, ultra)"));
	}
	else
	{
		check(QualityLevel >= 0 && QualityLevel < (sizeof(SampledLightingPresets) / sizeof(SampledLightingPresets[0])));

		const FSampledLightingPresets& Presets = SampledLightingPresets[QualityLevel];

		// Correction mode / approximate visibility shared for temporal/spatial
		CVarSampledLightingTemporalApplyApproxVisibility.AsVariable()->Set(Presets.CorrectionMode, ECVF_SetByConsole);
		CVarSampledLightingSpatialApplyApproxVisibility.AsVariable()->Set(Presets.CorrectionMode, ECVF_SetByConsole);

		// spatial sample count
		CVarSampledLightingSpatialSamples.AsVariable()->Set(Presets.SpatialSamples, ECVF_SetByConsole);

		// boosted spatial count
		CVarSampledLightingSpatialSamplesBoost.AsVariable()->Set(Presets.DisocclusionSamples, ECVF_SetByConsole);

		// initial sample count
		CVarSampledLightingInitialCandidates.AsVariable()->Set(Presets.InitialSamples, ECVF_SetByConsole);
	}
})
);

bool ShouldRenderRayTracingSampledLighting()
{
	return ShouldRenderRayTracingEffect(CVarSampledLighting.GetValueOnRenderThread() > 0);
}

bool SupportSampledLightingForType(ELightComponentType Type)
{
	bool Result = false;

	switch (Type)
	{
		case LightType_Directional:
			Result = CVarRayTracingSampledLightingDirectionalLight.GetValueOnAnyThread() != 0;
			break;
		case LightType_Point:
			Result = CVarRayTracingSampledLightingPointLight.GetValueOnAnyThread() != 0;
			break;
		case LightType_Spot:
			Result = CVarRayTracingSampledLightingSpotLight.GetValueOnAnyThread() != 0;
			break;
		case LightType_Rect:
			Result = CVarRayTracingSampledLightingRectLight.GetValueOnAnyThread() != 0;
			break;
		default:
			break;
	}

	return Result;
}

bool SupportSampledLightingForLightFunctions()
{
	return CVarRayTracingSampledLightingFunctionLights.GetValueOnRenderThread() != 0;
}

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FSampledLightData, )
	SHADER_PARAMETER(uint32, DirectionalLightCount)
	SHADER_PARAMETER(uint32, LocalLightCount)
	SHADER_PARAMETER(float, IESLightProfileInvCount)
	SHADER_PARAMETER(uint32, CellCount)
	SHADER_PARAMETER(float, CellScale)
	SHADER_PARAMETER_TEXTURE(Texture2D, LTCMatTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, LTCMatSampler)
	SHADER_PARAMETER_TEXTURE(Texture2D, LTCAmpTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, LTCAmpSampler)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture0)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture1)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture2)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture3)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture4)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture5)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture6)
	SHADER_PARAMETER_TEXTURE(Texture2D, RectLightTexture7)
	SHADER_PARAMETER_SAMPLER(SamplerState, IESLightProfileTextureSampler)
	SHADER_PARAMETER_TEXTURE(Texture2DArray, IESLightProfileTexture)
	SHADER_PARAMETER_SRV(Texture2D, SSProfilesTexture)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, LightDataBuffer)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, PackedLightDataBuffer)
	SHADER_PARAMETER_SRV(StructuredBuffer<int>, LightIndexRemapTable)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FSampledLightData, "SampledLightData");

BEGIN_SHADER_PARAMETER_STRUCT(FSampledLightingCommonParameters, )
	SHADER_PARAMETER(float, MaxNormalBias)
	SHADER_PARAMETER(int32, VisibilityApproximateTestMode)
	SHADER_PARAMETER(int32, VisibilityFaceCull)
	SHADER_PARAMETER(int32, SupportTranslucency)
	SHADER_PARAMETER(int32, InexactShadows)
	SHADER_PARAMETER(float, MaxBiasForInexactGeometry)
	SHADER_PARAMETER(int32, MaxTemporalHistory)
	SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWLightReservoirUAV)
	SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
END_SHADER_PARAMETER_STRUCT()

static void ApplySampledLightingGlobalSettings(FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("RTXDI_INTEGRATION_VERSION"), 4270);

	OutEnvironment.SetDefine(TEXT("LIGHT_ESTIMATION_MODE"), 1);
	OutEnvironment.SetDefine(TEXT("USE_ALTERNATE_RNG"), 0);
	OutEnvironment.SetDefine(TEXT("USE_LDS_FOR_SPATIAL_RESAMPLE"), 1);
}

class FDirectLightRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDirectLightRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FDirectLightRGS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER_STRUCT_REF(FSampledLightData, SampledLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWSpecularUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWRayDistanceUAV)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FDirectLightRGS, "/Engine/Private/RTXDI/RayTracingSampledDirectLighting.usf", "SampledDirectLightingRGS", SF_RayGen);

class FGenerateInitialSamplesRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateInitialSamplesRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FGenerateInitialSamplesRGS, FGlobalShader)

	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");

	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(int32, InitialCandidates)
		SHADER_PARAMETER(int32, InitialSampleVisibility)
		SHADER_PARAMETER_STRUCT_REF(FSampledLightData, SampledLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneLightingChannelParameters, SceneLightingChannels)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSampledLightingCommonParameters, SampledLightingCommonParameters)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGenerateInitialSamplesRGS, "/Engine/Private/RTXDI/RayTracingSampledDirectLighting.usf", "GenerateInitialSamplesRGS", SF_RayGen);

class FEvaluateSampledLightingRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FEvaluateSampledLightingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FEvaluateSampledLightingRGS, FGlobalShader)

	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");
	class FHairLightingDim : SHADER_PERMUTATION_BOOL("USE_HAIR_LIGHTING");

	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim, FHairLightingDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, NumReservoirs)
		SHADER_PARAMETER(int32, DemodulateMaterials)
		SHADER_PARAMETER(int32, DebugOutput)
		SHADER_PARAMETER(int32, FeedbackVisibility)
		SHADER_PARAMETER(uint32, bUseHairVoxel)
		SHADER_PARAMETER_STRUCT_REF(FSampledLightData, SampledLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWSpecularUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWRayDistanceUAV)
		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWLightReservoirHistoryUAV)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCategorizationTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairLightChannelMaskTexture)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneLightingChannelParameters, SceneLightingChannels)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSampledLightingCommonParameters, SampledLightingCommonParameters)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FEvaluateSampledLightingRGS, "/Engine/Private/RTXDI/RayTracingSampledDirectLighting.usf", "EvaluateSampledLightingRGS", SF_RayGen);

class FApplySpatialResamplingRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FApplySpatialResamplingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FApplySpatialResamplingRGS, FGlobalShader)

	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");

	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(float, SpatialSamplingRadius)
		SHADER_PARAMETER(int32, SpatialSamples)
		SHADER_PARAMETER(int32, SpatialSamplesBoost)
		SHADER_PARAMETER(float, SpatialDepthRejectionThreshold)
		SHADER_PARAMETER(float, SpatialNormalRejectionThreshold)
		SHADER_PARAMETER(int32, ApplyApproximateVisibilityTest)
		SHADER_PARAMETER(uint32, NeighborOffsetMask)
	
		SHADER_PARAMETER_STRUCT_REF(FSampledLightData, SampledLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneLightingChannelParameters, SceneLightingChannels)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSampledLightingCommonParameters, SampledLightingCommonParameters)

		SHADER_PARAMETER_SRV(Buffer<float2>, NeighborOffsets)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FApplySpatialResamplingRGS, "/Engine/Private/RTXDI/RayTracingSampledDirectLighting.usf", "ApplySpatialResamplingRGS", SF_RayGen);


class FApplyTemporalResamplingRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FApplyTemporalResamplingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FApplyTemporalResamplingRGS, FGlobalShader)

	class FFuseInitialSamplingDim : SHADER_PERMUTATION_BOOL("FUSE_TEMPORAL_AND_INITIAL_SAMPLING");

	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");

	using FPermutationDomain = TShaderPermutationDomain<FFuseInitialSamplingDim,FEvaluateLightingDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(float, TemporalDepthRejectionThreshold)
		SHADER_PARAMETER(float, TemporalNormalRejectionThreshold)
		SHADER_PARAMETER(int32, ApplyApproximateVisibilityTest)
		SHADER_PARAMETER(int32, InitialCandidates)
		SHADER_PARAMETER(int32, InitialSampleVisibility)

		SHADER_PARAMETER_STRUCT_REF(FSampledLightData, SampledLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, LightReservoirHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, NormalHistory)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneLightingChannelParameters, SceneLightingChannels)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSampledLightingCommonParameters, SampledLightingCommonParameters)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FApplyTemporalResamplingRGS, "/Engine/Private/RTXDI/RayTracingSampledDirectLighting.usf", "ApplyTemporalResamplingRGS", SF_RayGen);

class FApplyBoilingFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FApplyBoilingFilterCS)
	SHADER_USE_PARAMETER_STRUCT(FApplyBoilingFilterCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		ApplySampledLightingGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(float, BoilingFilterStrength)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWLightReservoirUAV)
		SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FApplyBoilingFilterCS, "/Engine/Private/RTXDI/BoilingFilter.usf", "BoilingFilterCS", SF_Compute);

class FCompositeSampledLightingPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompositeSampledLightingPS);
	SHADER_USE_PARAMETER_STRUCT(FCompositeSampledLightingPS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Diffuse)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Specular)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(int, ApplyDiffuse)
		SHADER_PARAMETER(int, ApplySpecular)
		SHADER_PARAMETER(int32, ModulateMaterials)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FCompositeSampledLightingPS, "/Engine/Private/RTXDI/CompositeSampledLightingPS.usf", "CompositeSampledLightingPS", SF_Pixel);

/**
 * This buffer provides a table with a low discrepency sequence
 */
class FDiscSampleBuffer : public FRenderResource
{
public:

	/** The vertex buffer used for storage. */
	FVertexBufferRHIRef DiscSampleBufferRHI;
	/** Shader resource view in to the vertex buffer. */
	FShaderResourceViewRHIRef DiscSampleBufferSRV;

	const uint32 NumSamples = 8192;

	/**
	 * Initialize RHI resources.
	 */
	virtual void InitRHI() override
	{
		if (RHISupportsRayTracingShaders(GShaderPlatformForFeatureLevel[GetFeatureLevel()]))
		{
			// Create a sequence of low-discrepancy samples within a unit radius around the origin
			// for "randomly" sampling neighbors during spatial resampling
			TResourceArray<uint8> Buffer;

			Buffer.AddZeroed(NumSamples * 2);

			const int32 R = 250;
			const float phi2 = 1.0f / 1.3247179572447f;
			uint32 num = 0;
			float U = 0.5f;
			float V = 0.5f;
			while (num < NumSamples * 2) {
				U += phi2;
				V += phi2 * phi2;
				if (U >= 1.0f) U -= 1.0f;
				if (V >= 1.0f) V -= 1.0f;

				float rSq = (U - 0.5f) * (U - 0.5f) + (V - 0.5f) * (V - 0.5f);
				if (rSq > 0.25f)
					continue;

				Buffer[num++] = uint8((U - 0.5f) * R + 127.5f);
				Buffer[num++] = uint8((V - 0.5f) * R + 127.5f);
				
			}

			FRHIResourceCreateInfo CreateInfo(&Buffer);
			DiscSampleBufferRHI = RHICreateVertexBuffer(
				/*Size=*/ sizeof(uint8) * 2 * NumSamples,
				/*Usage=*/ BUF_Volatile | BUF_ShaderResource,
				CreateInfo);
			DiscSampleBufferSRV = RHICreateShaderResourceView(
				DiscSampleBufferRHI, /*Stride=*/ sizeof(uint8) * 2, PF_R8G8
			);
		}
	}

	/**
	 * Release RHI resources.
	 */
	virtual void ReleaseRHI() override
	{
		DiscSampleBufferSRV.SafeRelease();
		DiscSampleBufferRHI.SafeRelease();
	}
};

/** The global resource for the disc sample buffer. */
TGlobalResource<FDiscSampleBuffer> GDiscSampleBuffer;

/*
 * Compressed light structure, packs per-light data into 80 bytes
 * Compression is relatively conservative
 */
struct FPackedLightingData
{
	//uint Type;											// 2 bits (directional, spot, point, rect)
	//uint RectLightTextureIndex;							// 7 bit (99 is the invalid index)
	//float SoftSourceRadius;								// 16 bit half float
	uint32 TypeRectLightTextureIndexAndSoftSourceRadius;	// 
	int32 LightProfileIndex;								// 32 bits - uses sentinel of -1 for NONE
	uint32 LightIDLightFunctionAndMask;						// Function and mask 8 bits each 
	float InvRadius;										// 

	float LightPosition[3];									// 96 bits
	//float SourceRadius;									// 16 bit half float
	//float SourceLength;									// 16 bit half float
	uint32 SourceRadiusAndLength;							// 32 bits

	//float3 Direction;										// 48 bits half float (could use 32-bit oct)
	//float3 Tangent;										// 48 bits half float (could use 32-bit oct)
	uint32 DirectionAndTangent[3];							// 96 bits
	//float FalloffExponent;								// 16 bit half float
	//float SpecularScale;									// 16 bit half float
	uint32 FalloffExponentAndSpecularScale;					// 32 bits

	float LightColor[3];									// 96 bits - could possibly repack using scale or bfloat style encodings
	//float RectLightBarnCosAngle;							// 16 bit half float
	//float RectLightBarnLength;							// 16 bit half float
	uint32 RectLightBarnCosAngleAndLength;					// 32 bits
	
	float SpotAngles[2];									// 64 bits (second arg is 1/(cos a - cos b)
	float DistanceFadeMAD[2];								// 64 bits

	FPackedLightingData(const FRTLightingData& LightData)
	{
		TypeRectLightTextureIndexAndSoftSourceRadius = FFloat16(LightData.SoftSourceRadius).Encoded;
		TypeRectLightTextureIndexAndSoftSourceRadius |= (LightData.RectLightTextureIndex & 0xff) << 16;
		TypeRectLightTextureIndexAndSoftSourceRadius |= (LightData.Type & 0xff) << 24;
		LightProfileIndex = LightData.LightProfileIndex;
		LightIDLightFunctionAndMask = LightData.FlagsLightFunctionAndMask;
		LightIDLightFunctionAndMask |= ((*((uint32*)LightData.Dummy)) & 0xffff) << 16;
		InvRadius = LightData.InvRadius;

		LightPosition[0] = LightData.LightPosition[0];
		LightPosition[1] = LightData.LightPosition[1];
		LightPosition[2] = LightData.LightPosition[2];
		SourceRadiusAndLength = FFloat16(LightData.SourceLength).Encoded;
		SourceRadiusAndLength |= FFloat16(LightData.SourceRadius).Encoded << 16;

		DirectionAndTangent[0] = FFloat16(LightData.Tangent[0]).Encoded;
		DirectionAndTangent[0] |= FFloat16(LightData.Direction[0]).Encoded << 16;
		DirectionAndTangent[1] = FFloat16(LightData.Tangent[1]).Encoded;
		DirectionAndTangent[1] |= FFloat16(LightData.Direction[1]).Encoded << 16;
		DirectionAndTangent[2] = FFloat16(LightData.Tangent[2]).Encoded;
		DirectionAndTangent[2] |= FFloat16(LightData.Direction[2]).Encoded << 16;
		FalloffExponentAndSpecularScale = FFloat16(LightData.SpecularScale).Encoded;
		FalloffExponentAndSpecularScale |= FFloat16(LightData.FalloffExponent).Encoded << 16;

		LightColor[0] = LightData.LightColor[0];
		LightColor[1] = LightData.LightColor[1];
		LightColor[2] = LightData.LightColor[2];
		RectLightBarnCosAngleAndLength = FFloat16(LightData.RectLightBarnLength).Encoded;
		RectLightBarnCosAngleAndLength |= FFloat16(LightData.RectLightBarnCosAngle).Encoded << 16;

		SpotAngles[0] = LightData.SpotAngles[0];
		SpotAngles[1] = LightData.SpotAngles[1];
		DistanceFadeMAD[0] = LightData.DistanceFadeMAD[0];
		DistanceFadeMAD[1] = LightData.DistanceFadeMAD[1];
	}
}; // 80 bytes total

static_assert(sizeof(FPackedLightingData) == 80, "FPackedLightingData compiled to incompatible size");

static void SetupSampledRaytracingLightData(
	FRHICommandListImmediate& RHICmdList,
	const FScene *Scene,
	const TArray<FSortedLightSceneInfo, SceneRenderingAllocator>& Lights,
	const FViewInfo& View,
	const TMap<const ULightComponent*, int32>& PrevLightMap,
	FSampledLightData* LightData,
	TResourceArray<FRTLightingData>& LightDataArray,
	TResourceArray<FPackedLightingData>& PackedLightDataArray,
	TMap<const ULightComponent*, int32>& LightMap,
	TResourceArray<int32>& LightRemapTable)
{
	TMap<UTextureLightProfile*, int32> IESLightProfilesMap;
	TMap<FRHITexture*, uint32> RectTextureMap;

	const bool bSupportLightFunctions = CanUseRayTracingLightingMissShader(View.GetShaderPlatform());

	TArray<FSortedLightSceneInfo, SceneRenderingAllocator> SortedLights = Lights;

	struct FSeparateLocal
	{
		FORCEINLINE bool operator()(const FSortedLightSceneInfo& A, const FSortedLightSceneInfo& B) const
		{
			int32 AIsLocal = A.LightSceneInfo->Proxy->GetLightType() != LightType_Directional;
			int32 BIsLocal = B.LightSceneInfo->Proxy->GetLightType() != LightType_Directional;
			return AIsLocal < BIsLocal;
		}
	};

	SortedLights.StableSort(FSeparateLocal());

	// initialize the light remapping table to invalid (-1)
	const int32 PrevLightCount = PrevLightMap.Num();
	LightRemapTable.Empty();
	LightRemapTable.AddZeroed(PrevLightCount);

	for (int32 LightIndex = 0; LightIndex < PrevLightCount; LightIndex++)
	{
		LightRemapTable[LightIndex] = -1;
	}

	LightData->LTCMatTexture = GSystemTextures.LTCMat->GetRenderTargetItem().ShaderResourceTexture;
	LightData->LTCMatSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	LightData->LTCAmpTexture = GSystemTextures.LTCAmp->GetRenderTargetItem().ShaderResourceTexture;
	LightData->LTCAmpSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FTextureRHIRef DymmyWhiteTexture = GWhiteTexture->TextureRHI;
	LightData->RectLightTexture0 = DymmyWhiteTexture;
	LightData->RectLightTexture1 = DymmyWhiteTexture;
	LightData->RectLightTexture2 = DymmyWhiteTexture;
	LightData->RectLightTexture3 = DymmyWhiteTexture;
	LightData->RectLightTexture4 = DymmyWhiteTexture;
	LightData->RectLightTexture5 = DymmyWhiteTexture;
	LightData->RectLightTexture6 = DymmyWhiteTexture;
	LightData->RectLightTexture7 = DymmyWhiteTexture;
	static constexpr uint32 MaxRectLightTextureSlos = 8;
	static constexpr uint32 InvalidTextureIndex = 99; 


	for (auto Light : SortedLights)
	{
		if (View.Family->EngineShowFlags.TexturedLightProfiles)
		{
			UTextureLightProfile* IESLightProfileTexture = Light.LightSceneInfo->Proxy->GetIESTexture();
			if (IESLightProfileTexture)
			{
				int32* IndexFound = IESLightProfilesMap.Find(IESLightProfileTexture);
				if (!IndexFound)
				{
					IESLightProfilesMap.Add(IESLightProfileTexture, IESLightProfilesMap.Num());
				}
			}
		}
	}

	if (View.IESLightProfile2DResource != nullptr && IESLightProfilesMap.Num() > 0)
	{
		TArray<UTextureLightProfile*, SceneRenderingAllocator> IESProfilesArray;
		IESProfilesArray.AddUninitialized(IESLightProfilesMap.Num());
		for (auto It = IESLightProfilesMap.CreateIterator(); It; ++It)
		{
			IESProfilesArray[It->Value] = It->Key;
		}

		View.IESLightProfile2DResource->BuildIESLightProfilesTexture(RHICmdList,IESProfilesArray);
	}

	{
		// IES profiles
		float IESInvProfileCount = 1.0f;

		if (View.IESLightProfile2DResource && View.IESLightProfile2DResource->GetIESLightProfilesCount())
		{
			LightData->IESLightProfileTexture = View.IESLightProfile2DResource->GetTexture();

			uint32 ProfileCount = View.IESLightProfile2DResource->GetIESLightProfilesPerPage();
			IESInvProfileCount = ProfileCount ? 1.f / static_cast<float>(ProfileCount) : 0.f;
		}
		else
		{
			LightData->IESLightProfileTexture = GWhiteTexture->TextureRHI;
		}

		LightData->IESLightProfileInvCount = IESInvProfileCount;
		LightData->IESLightProfileTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	}

	LightData->DirectionalLightCount = 0;
	LightData->LocalLightCount = 0;

	for (auto Light : SortedLights)
	{
		auto LightType = Light.LightSceneInfo->Proxy->GetLightType();

		FLightShaderParameters LightParameters;
		Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);

		if (Light.LightSceneInfo->Proxy->IsInverseSquared())
		{
			LightParameters.FalloffExponent = 0;
		}

		int32 IESLightProfileIndex = INDEX_NONE;
		if (View.Family->EngineShowFlags.TexturedLightProfiles)
		{
			UTextureLightProfile* IESLightProfileTexture = Light.LightSceneInfo->Proxy->GetIESTexture();
			if (IESLightProfileTexture)
			{
				int32* IndexFound = IESLightProfilesMap.Find(IESLightProfileTexture);
				if (!IndexFound)
				{
					check(0);
				}
				else
				{
					IESLightProfileIndex = *IndexFound;
				}
			}
		}

		FRTLightingData LightDataElement;

		LightDataElement.Type = LightType;
		LightDataElement.RectLightTextureIndex = InvalidTextureIndex;

		if (IESLightProfileIndex == INDEX_NONE)
		{
			LightDataElement.LightProfileIndex = 0xffffffff;
		}
		else
		{
			FIESLightProfile2DResource::FIESLightProfileIndex Index = View.IESLightProfile2DResource->GetProfileIndex(IESLightProfileIndex);
			LightDataElement.LightProfileIndex = Index.Page << 16 | Index.Start;
		}

		for (int32 Element = 0; Element < 3; Element++)
		{
			LightDataElement.Direction[Element] = LightParameters.Direction[Element];
			LightDataElement.LightPosition[Element] = LightParameters.Position[Element];
			LightDataElement.LightColor[Element] = LightParameters.Color[Element];
			LightDataElement.Tangent[Element] = LightParameters.Tangent[Element];
		}

		const FVector2D FadeParams = Light.LightSceneInfo->Proxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), Light.LightSceneInfo->IsPrecomputedLightingValid(), View.MaxShadowCascades);
		const FVector2D DistanceFadeMAD = { FadeParams.Y, -FadeParams.X * FadeParams.Y };

		for (int32 Element = 0; Element < 2; Element++)
		{
			LightDataElement.SpotAngles[Element] = LightParameters.SpotAngles[Element];
			LightDataElement.DistanceFadeMAD[Element] = DistanceFadeMAD[Element];
		}

		LightDataElement.InvRadius = LightParameters.InvRadius;
		LightDataElement.SpecularScale = LightParameters.SpecularScale;
		LightDataElement.FalloffExponent = LightParameters.FalloffExponent;
		LightDataElement.SourceRadius = LightParameters.SourceRadius;
		LightDataElement.SourceLength = LightParameters.SourceLength;
		LightDataElement.SoftSourceRadius = LightParameters.SoftSourceRadius;
		LightDataElement.RectLightBarnCosAngle = LightParameters.RectLightBarnCosAngle;
		LightDataElement.RectLightBarnLength = LightParameters.RectLightBarnLength;

		LightDataElement.FlagsLightFunctionAndMask = 0;

		const int32 *LightFunctionIndex = Scene->RayTracingLightFunctionMap.Find(Light.LightSceneInfo->Proxy->GetLightComponent());

		if (View.Family->EngineShowFlags.LightFunctions && bSupportLightFunctions && LightFunctionIndex && *LightFunctionIndex >= 0)
		{
			// set the light function index, 0 is reserved as none, so the index is offset by 1
			LightDataElement.FlagsLightFunctionAndMask = *LightFunctionIndex + 1;
		}

		// store light channel mask
		uint8 LightMask = Light.LightSceneInfo->Proxy->GetLightingChannelMask();
		LightDataElement.FlagsLightFunctionAndMask |= LightMask << 8;

		// Stuff directional light's shadow angle factor into a RectLight parameter
		if (LightType == LightType_Directional)
		{
			LightDataElement.RectLightBarnCosAngle = Light.LightSceneInfo->Proxy->GetShadowSourceAngleFactor();
		}

		{
			const ULightComponent *Component = Light.LightSceneInfo->Proxy->GetLightComponent();
			const int32 NewIndex = LightDataArray.Num();
			const int32* IndexFound = PrevLightMap.Find(Component);
			if (IndexFound)
			{
				LightRemapTable[*IndexFound] = NewIndex;
			}

			if (!View.bStatePrevViewInfoIsReadOnly)
			{
				LightMap.Add(Component, NewIndex);
			}

			// Temporary hack until we reformat the light data
			// Tracks the light pointer to ensure we have a stable identifier for light visualization purposes
			uint32 *LightID = (uint32*)(LightDataElement.Dummy);
			*LightID = FCrc::TypeCrc32(Component);

		}

		LightDataArray.Add(LightDataElement);

		const bool bRequireTexture = LightType == ELightComponentType::LightType_Rect && LightParameters.SourceTexture;
		uint32 RectLightTextureIndex = InvalidTextureIndex;
		if (bRequireTexture)
		{
			const uint32* IndexFound = RectTextureMap.Find(LightParameters.SourceTexture);
			if (!IndexFound)
			{
				if (RectTextureMap.Num() < MaxRectLightTextureSlos)
				{
					RectLightTextureIndex = RectTextureMap.Num();
					RectTextureMap.Add(LightParameters.SourceTexture, RectLightTextureIndex);
				}
			}
			else
			{
				RectLightTextureIndex = *IndexFound;
			}
		}

		const uint32 Count = LightData->DirectionalLightCount + LightData->LocalLightCount;

		if (RectLightTextureIndex != InvalidTextureIndex)
		{
			LightDataArray[Count].RectLightTextureIndex = RectLightTextureIndex;
			switch (RectLightTextureIndex)
			{
			case 0: LightData->RectLightTexture0 = LightParameters.SourceTexture; break;
			case 1: LightData->RectLightTexture1 = LightParameters.SourceTexture; break;
			case 2: LightData->RectLightTexture2 = LightParameters.SourceTexture; break;
			case 3: LightData->RectLightTexture3 = LightParameters.SourceTexture; break;
			case 4: LightData->RectLightTexture4 = LightParameters.SourceTexture; break;
			case 5: LightData->RectLightTexture5 = LightParameters.SourceTexture; break;
			case 6: LightData->RectLightTexture6 = LightParameters.SourceTexture; break;
			case 7: LightData->RectLightTexture7 = LightParameters.SourceTexture; break;
			}
		}

		PackedLightDataArray.Add(LightDataArray[Count]);

		if (LightType == LightType_Directional)
		{
			// directional lights must be before local lights
			check(LightData->LocalLightCount == 0);

			LightData->DirectionalLightCount++;
		}
		else
		{
			LightData->LocalLightCount++;
		}
	}


}

TUniformBufferRef<FSampledLightData> CreateSampledLightDatadUniformBuffer(
	FRHICommandListImmediate& RHICmdList,
	const FScene *Scene,
	const TArray<FSortedLightSceneInfo, SceneRenderingAllocator>& Lights,
	const FViewInfo& View,
	const TMap<const ULightComponent*, int32>& PrevLightMap,
	EUniformBufferUsage Usage,
	FStructuredBufferRHIRef& OutLightDataBuffer,
	FStructuredBufferRHIRef& OutLightRemapBuffer,
	FShaderResourceViewRHIRef& OutLightDataSRV,
	TMap<const ULightComponent*, int32>& OutLightMap)
{
	FSampledLightData LightData;
	TResourceArray<FRTLightingData> LightDataArray;
	TResourceArray<FPackedLightingData> PackedLightDataArray;
	TResourceArray<int32> LightRemapTable;

	SetupSampledRaytracingLightData(RHICmdList, Scene, Lights, View, PrevLightMap, &LightData, LightDataArray, PackedLightDataArray, OutLightMap, LightRemapTable);

	check( (LightData.LocalLightCount + LightData.DirectionalLightCount) == LightDataArray.Num());
	check(LightDataArray.Num() == PackedLightDataArray.Num());
	check(LightRemapTable.Num() == PrevLightMap.Num());

	// need at least one element
	if (LightDataArray.Num() == 0)
	{
		LightDataArray.AddZeroed(1);
	}
	if (LightRemapTable.Num() == 0)
	{
		LightRemapTable.Add(-1);
	}

	// This buffer might be best placed as an element of the LightData uniform buffer
	FRHIResourceCreateInfo CreateInfo;
	CreateInfo.ResourceArray = &LightDataArray;

	OutLightDataBuffer = RHICreateStructuredBuffer(sizeof(FVector4), LightDataArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
	OutLightDataSRV = RHICreateShaderResourceView(OutLightDataBuffer);

	CreateInfo.ResourceArray = &LightRemapTable;

	OutLightRemapBuffer = RHICreateStructuredBuffer(sizeof(int32), LightRemapTable.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
	LightData.LightIndexRemapTable = RHICreateShaderResourceView(OutLightRemapBuffer);

	LightData.LightDataBuffer = OutLightDataSRV;
	LightData.SSProfilesTexture = View.RayTracingSubSurfaceProfileSRV;

	CreateInfo.ResourceArray = &PackedLightDataArray;
	FStructuredBufferRHIRef PackedLightData = RHICreateStructuredBuffer(sizeof(FVector4), PackedLightDataArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
	LightData.PackedLightDataBuffer = RHICreateShaderResourceView(PackedLightData);

	return CreateUniformBufferImmediate(LightData, Usage);
}

/*
 * Code for handling top-level shader permutations
 */
struct SampledLightingPermutation
{
	bool EvaluationMode;
};

template<typename ShaderType>
static TShaderRef<ShaderType> GetShaderPermutation(typename ShaderType::FPermutationDomain PermutationVector, SampledLightingPermutation Options, const FViewInfo& View)
{
	PermutationVector.Set<typename ShaderType::FEvaluateLightingDim>(Options.EvaluationMode);

	return View.ShaderMap->GetShader<ShaderType>(PermutationVector);
}

template<typename ShaderType>
static TShaderRef<ShaderType> GetShaderPermutation(SampledLightingPermutation Options, const FViewInfo& View)
{
	ShaderType::FPermutationDomain PermutationVector;
	return GetShaderPermutation<ShaderType>(PermutationVector, Options, View);
}

template<typename ShaderType>
static void AddShaderPermutation(typename ShaderType::FPermutationDomain PermutationVector, SampledLightingPermutation Options, const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	auto RayGenShader = GetShaderPermutation<ShaderType>(PermutationVector, Options, View);

	OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
}

template<typename ShaderType>
static void AddShaderPermutation(SampledLightingPermutation Options, const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	ShaderType::FPermutationDomain PermutationVector;
	AddShaderPermutation< ShaderType>(PermutationVector, Options, View, OutRayGenShaders);
}

static SampledLightingPermutation GetPermutationOptions()
{
	SampledLightingPermutation Options;
	Options.EvaluationMode = CVarRayTracingSampledLightingEvaluationMode.GetValueOnRenderThread() != 0;

	return Options;
}

void FDeferredShadingSceneRenderer::PrepareRayTracingSampledDirectLighting(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Declare all RayGen shaders that require material closest hit shaders to be bound

	if (!ShouldRenderRayTracingSampledLighting())
	{
		return;
	}

	SampledLightingPermutation Options = GetPermutationOptions();

	auto RayGenShader = View.ShaderMap->GetShader<FDirectLightRGS>();
	OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());

	AddShaderPermutation< FGenerateInitialSamplesRGS>(Options,View, OutRayGenShaders);
	AddShaderPermutation< FApplySpatialResamplingRGS>(Options,View, OutRayGenShaders);

	for (int32 Permutation = 0; Permutation < 2; Permutation++)
	{
		FApplyTemporalResamplingRGS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FApplyTemporalResamplingRGS::FFuseInitialSamplingDim>(Permutation != 0);

		AddShaderPermutation< FApplyTemporalResamplingRGS>(PermutationVector, Options, View, OutRayGenShaders);
	}

	for (int32 Permutation = 0; Permutation < 2; Permutation++)
	{
		FEvaluateSampledLightingRGS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FEvaluateSampledLightingRGS::FHairLightingDim>(Permutation != 0);

		AddShaderPermutation< FEvaluateSampledLightingRGS>(PermutationVector, Options, View, OutRayGenShaders);
	}
}


void FDeferredShadingSceneRenderer::RenderSampledDirectLighting(
	FRDGBuilder& GraphBuilder,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	const TArray<FSortedLightSceneInfo,SceneRenderingAllocator> &SampledLights,
	const FHairStrandsRenderingData* HairDatas,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef LightingChannelsTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "SampledDirectLighting");

	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder, SceneTexturesUniformBuffer);

	const FViewInfo& ReferenceView = Views[0];

	//create/update light structure, could do per view rather than via reference view
	FStructuredBufferRHIRef							SampledLightDataBuffer;
	FStructuredBufferRHIRef							LightRemapBuffer;
	TUniformBufferRef<FSampledLightData>		SampledLightDataUniformBuffer;
	FShaderResourceViewRHIRef						SampledLightDataSRV;
	SampledLightDataUniformBuffer = CreateSampledLightDatadUniformBuffer(
		GraphBuilder.RHICmdList,
		Scene,
		SampledLights,
		ReferenceView,
		ReferenceView.PrevViewInfo.SampledLightHistory.LightRemapTable,
		EUniformBufferUsage::UniformBuffer_SingleFrame,
		SampledLightDataBuffer,
		LightRemapBuffer,
		SampledLightDataSRV,
		ReferenceView.ViewState->PrevFrameViewInfo.SampledLightHistory.LightRemapTable);

	// Intermediate lighting targets
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		SceneTextures.SceneDepthTexture->Desc.Extent,
		PF_FloatRGBA,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef Diffuse = GraphBuilder.CreateTexture(Desc, TEXT("SampledLightDiffuse"));
	FRDGTextureRef Specular = GraphBuilder.CreateTexture(Desc, TEXT("SampledLightSpecular"));

	Desc.Format = PF_G16R16F;
	FRDGTextureRef RayHitDistance = GraphBuilder.CreateTexture(Desc, TEXT("SampledLightHitDistance"));

	const int32 RequestedReservoirs = CVarSampledLightingNumReservoirs.GetValueOnAnyThread();
	const int32 MinReservoirs = FMath::Max(CVarSampledLightingMinReservoirs.GetValueOnAnyThread(), 1);
	const int32 MaxReservoirs = FMath::Max(CVarSampledLightingMaxReservoirs.GetValueOnAnyThread(), 1);
	const bool SubsampledView = ReferenceView.GetSecondaryViewRectSize() != ReferenceView.ViewRect.Size();
	const int32 AutoReservoirs = SubsampledView ? MaxReservoirs : MinReservoirs;
	const int32 NumReservoirs = RequestedReservoirs < 0 ? AutoReservoirs : FMath::Max(RequestedReservoirs, 1);
	FIntPoint PaddedSize = FMath::DivideAndRoundUp<FIntPoint>(SceneTextures.SceneDepthTexture->Desc.Extent, 4) * 4;

	FIntVector ReservoirBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs + 1);
	FRDGBufferDesc ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(16, ReservoirBufferDim.X * ReservoirBufferDim.Y * ReservoirBufferDim.Z);

	FRDGBufferRef LightReservoirs = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("LightReservoirs"));

	
	FIntVector ReservoirHistoryBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs);
	FRDGBufferDesc ReservoirHistoryDesc = FRDGBufferDesc::CreateStructuredDesc(16, ReservoirHistoryBufferDim.X * ReservoirHistoryBufferDim.Y * ReservoirHistoryBufferDim.Z);
	FRDGBufferRef LightReservoirsHistory = GraphBuilder.CreateBuffer(ReservoirHistoryDesc, TEXT("LightReservoirsHistory"));

	const int32 LightingMode = CVarSampledLightingMode.GetValueOnRenderThread();

	// evaluate lighting
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		auto& View = Views[ViewIndex];
		FIntPoint LightingResolution = View.ViewRect.Size();

		// Code is replicated from static function GetHairStrandResources in LightRendering.cpp
		FHairStrandsOcclusionResources HairResources;
		if (HairDatas && ViewIndex < HairDatas->HairVisibilityViews.HairDatas.Num())
		{
			if (HairDatas->HairVisibilityViews.HairDatas[ViewIndex].CategorizationTexture)
			{
				HairResources.CategorizationTexture = HairDatas->HairVisibilityViews.HairDatas[ViewIndex].CategorizationTexture;
			}
			if (HairDatas->HairVisibilityViews.HairDatas[ViewIndex].LightChannelMaskTexture)
			{
				HairResources.LightChannelMaskTexture = HairDatas->HairVisibilityViews.HairDatas[ViewIndex].LightChannelMaskTexture;
			}

			HairResources.VoxelResources = &HairDatas->MacroGroupsPerViews.Views[ViewIndex].VirtualVoxelResources;
		}
		// Deep shadow maps require per-light processing, so all lights requesting sampled lighting get the hair voxel lighting
		HairResources.bUseHairVoxel = true;

		if (LightingMode == 0)
		{
			FDirectLightRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDirectLightRGS::FParameters>();

			PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			PassParameters->SampledLightData = SampledLightDataUniformBuffer;
			PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
			PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
			PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);

			PassParameters->RWDiffuseUAV = GraphBuilder.CreateUAV(Diffuse);
			PassParameters->RWSpecularUAV = GraphBuilder.CreateUAV(Specular);
			PassParameters->RWRayDistanceUAV = GraphBuilder.CreateUAV(RayHitDistance);

			auto RayGenShader = View.ShaderMap->GetShader<FDirectLightRGS>();

			ClearUnusedGraphResources(RayGenShader, PassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("SinglePassSampledLighting"),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);


			});
		}
		else
		{
			// detect camera cuts/history invalidation and boost initial/spatial samples to compensate
			const bool bCameraCut = !ReferenceView.PrevViewInfo.SampledLightHistory.LightReservoirs.IsValid() || ReferenceView.bCameraCut;
			const int32 PrevHistoryCount = ReferenceView.PrevViewInfo.SampledLightHistory.ReservoirDimensions.Z;

			// Global permutation options
			const SampledLightingPermutation Options = GetPermutationOptions();

			const int32 InitialCandidates = bCameraCut ? CVarSampledLightingInitialCandidatesBoost.GetValueOnRenderThread() : CVarSampledLightingInitialCandidates.GetValueOnRenderThread();

			int32 InitialSlice = 0;
			const bool bEnableFusedSampling = CVarSampledLightingFusedSampling.GetValueOnRenderThread() != 0;

			static auto CVarSupportTranslucency = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadows.SupportTranslucency"));
			static auto CVarMaxInexactBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadow.MaxBiasForInexactGeometry"));
			static auto CVarEnableInexactBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadow.UseBiasForSkipWPOEval"));

			// Parameters shared by ray tracing passes
			FSampledLightingCommonParameters CommonParameters;
			CommonParameters.MaxNormalBias = GetRaytracingMaxNormalBias();
			CommonParameters.TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
			CommonParameters.RWLightReservoirUAV = GraphBuilder.CreateUAV(LightReservoirs);
			CommonParameters.ReservoirBufferDim = ReservoirBufferDim;
			CommonParameters.VisibilityApproximateTestMode = CVarSampledLightingApproximateVisibilityMode.GetValueOnRenderThread();
			CommonParameters.VisibilityFaceCull = CVarSampledLightingFaceCull.GetValueOnRenderThread();
			CommonParameters.SupportTranslucency = CVarSupportTranslucency ? CVarSupportTranslucency->GetInt() : 0;
			CommonParameters.InexactShadows = CVarEnableInexactBias ? CVarEnableInexactBias->GetInt() : 0;
			CommonParameters.MaxBiasForInexactGeometry = CVarMaxInexactBias ? CVarMaxInexactBias->GetFloat() : 0.0f;
			CommonParameters.MaxTemporalHistory = FMath::Max(1, CVarSampledLightingTemporalMaxHistory.GetValueOnRenderThread());

			for (int32 Reservoir = 0; Reservoir < NumReservoirs; Reservoir++)
			{
				const bool bUseFusedSampling = CVarSampledLightingTemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount && bEnableFusedSampling;

				// Initial sampling pass to select a light candidate
				if (!bUseFusedSampling)
				{
					FGenerateInitialSamplesRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateInitialSamplesRGS::FParameters>();

					PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
					PassParameters->SampledLightData = SampledLightDataUniformBuffer;
					PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
					PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
					PassParameters->SceneLightingChannels = GetSceneLightingChannelParameters(GraphBuilder, LightingChannelsTexture);

					PassParameters->OutputSlice = Reservoir;
					PassParameters->HistoryReservoir = Reservoir;
					PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
					PassParameters->InitialSampleVisibility = CVarRayTracingSampledLightingTestInitialVisibility.GetValueOnRenderThread();

					PassParameters->SampledLightingCommonParameters = CommonParameters;

					//auto RayGenShader = View.ShaderMap->GetShader<FGenerateInitialSamplesRGS>();
					auto RayGenShader = GetShaderPermutation<FGenerateInitialSamplesRGS>(Options, View);

					ClearUnusedGraphResources(RayGenShader, PassParameters);

					GraphBuilder.AddPass(
						RDG_EVENT_NAME("CreateInitialSamples"),
						PassParameters,
						ERDGPassFlags::Compute,
						[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
					{
						FRayTracingShaderBindingsWriter GlobalResources;
						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
					});
				}

				// Temporal candidate merge pass, optionally merged with initial candidate pass
				if (CVarSampledLightingTemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount)
				{
					{
						FApplyTemporalResamplingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyTemporalResamplingRGS::FParameters>();

						PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
						PassParameters->SampledLightData = SampledLightDataUniformBuffer;
						PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
						PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
						PassParameters->SceneLightingChannels = GetSceneLightingChannelParameters(GraphBuilder, LightingChannelsTexture);

						PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
						PassParameters->InputSlice = Reservoir;
						PassParameters->OutputSlice = Reservoir;
						PassParameters->HistoryReservoir = Reservoir;
						PassParameters->TemporalDepthRejectionThreshold = FMath::Clamp(CVarSampledLightingTemporalDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
						PassParameters->TemporalNormalRejectionThreshold = FMath::Clamp(CVarSampledLightingTemporalNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
						PassParameters->ApplyApproximateVisibilityTest = CVarSampledLightingTemporalApplyApproxVisibility.GetValueOnAnyThread();
						PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
						PassParameters->InitialSampleVisibility = CVarRayTracingSampledLightingTestInitialVisibility.GetValueOnRenderThread();

						PassParameters->LightReservoirHistory = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ReferenceView.PrevViewInfo.SampledLightHistory.LightReservoirs));
						PassParameters->NormalHistory = RegisterExternalTextureWithFallback(GraphBuilder, ReferenceView.PrevViewInfo.GBufferA, GSystemTextures.BlackDummy);
						PassParameters->DepthHistory = RegisterExternalTextureWithFallback(GraphBuilder, ReferenceView.PrevViewInfo.DepthBuffer, GSystemTextures.BlackDummy);

						PassParameters->SampledLightingCommonParameters = CommonParameters;

						FApplyTemporalResamplingRGS::FPermutationDomain PermutationVector;
						PermutationVector.Set<FApplyTemporalResamplingRGS::FFuseInitialSamplingDim>(bEnableFusedSampling);

						//auto RayGenShader = View.ShaderMap->GetShader<FApplyTemporalResamplingRGS>(PermutationVector);
						auto RayGenShader = GetShaderPermutation<FApplyTemporalResamplingRGS>(PermutationVector,Options, View);

						ClearUnusedGraphResources(RayGenShader, PassParameters);

						GraphBuilder.AddPass(
							RDG_EVENT_NAME("%sTemporalResample", bEnableFusedSampling ? TEXT("FusedInitialCandidateAnd") : TEXT("")),
							PassParameters,
							ERDGPassFlags::Compute,
							[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
						{
							FRayTracingShaderBindingsWriter GlobalResources;
							SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

							FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
							RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);

						});
					}

					// Boiling filter pass to prevent runaway samples
					if (CVarSampledLightingApplyBoilingFilter.GetValueOnRenderThread() != 0)
					{
						FApplyBoilingFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyBoilingFilterCS::FParameters>();

						PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

						PassParameters->RWLightReservoirUAV = GraphBuilder.CreateUAV(LightReservoirs);
						PassParameters->ReservoirBufferDim = ReservoirBufferDim;
						PassParameters->InputSlice = Reservoir;
						PassParameters->OutputSlice = Reservoir;
						PassParameters->BoilingFilterStrength = FMath::Clamp(CVarSampledLightingBoilingFilterStrength.GetValueOnRenderThread(), 0.00001f, 1.0f);

						auto ComputeShader = View.ShaderMap->GetShader<FApplyBoilingFilterCS>();

						ClearUnusedGraphResources(ComputeShader, PassParameters);
						FIntPoint GridSize = FMath::DivideAndRoundUp<FIntPoint>(View.ViewRect.Size(), 16);

						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoilingFilter"), ComputeShader, PassParameters, FIntVector(GridSize.X, GridSize.Y, 1));
					}
				}
			}

			// Spatial resampling passes, one per reservoir
			for (int32 Reservoir = NumReservoirs; Reservoir > 0; Reservoir--)
			{
				if (CVarSampledLightingSpatial.GetValueOnRenderThread() != 0)
				{
					FApplySpatialResamplingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplySpatialResamplingRGS::FParameters>();

					PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
					PassParameters->SampledLightData = SampledLightDataUniformBuffer;
					PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
					PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
					PassParameters->SceneLightingChannels = GetSceneLightingChannelParameters(GraphBuilder, LightingChannelsTexture);

					PassParameters->InputSlice = Reservoir - 1;
					PassParameters->OutputSlice = Reservoir;
					PassParameters->HistoryReservoir = Reservoir - 1;
					PassParameters->SpatialSamples = FMath::Max(CVarSampledLightingSpatialSamples.GetValueOnRenderThread(), 1);
					PassParameters->SpatialSamplesBoost = FMath::Max(CVarSampledLightingSpatialSamplesBoost.GetValueOnRenderThread(), 1);
					PassParameters->SpatialSamplingRadius = FMath::Max(1.0f, CVarSampledLightingSpatialSamplingRadius.GetValueOnRenderThread());
					PassParameters->SpatialDepthRejectionThreshold = FMath::Clamp(CVarSampledLightingSpatialDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
					PassParameters->SpatialNormalRejectionThreshold = FMath::Clamp(CVarSampledLightingSpatialNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
					PassParameters->ApplyApproximateVisibilityTest = CVarSampledLightingSpatialApplyApproxVisibility.GetValueOnRenderThread();

					PassParameters->NeighborOffsetMask = GDiscSampleBuffer.NumSamples - 1;
					PassParameters->NeighborOffsets = GDiscSampleBuffer.DiscSampleBufferSRV;

					PassParameters->SampledLightingCommonParameters = CommonParameters;

					//auto RayGenShader = View.ShaderMap->GetShader<FApplySpatialResamplingRGS>();
					auto RayGenShader = GetShaderPermutation<FApplySpatialResamplingRGS>(Options, View);

					ClearUnusedGraphResources(RayGenShader, PassParameters);

					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SpatialResample"),
						PassParameters,
						ERDGPassFlags::Compute,
						[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
					{
						FRayTracingShaderBindingsWriter GlobalResources;
						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);

					});
					InitialSlice = Reservoir;
				}
			}

			// Shading evaluation pass
			{
				const bool bUseHairLighting =
					HairResources.CategorizationTexture != nullptr &&
					HairResources.LightChannelMaskTexture != nullptr &&
					HairResources.VoxelResources != nullptr;

				FEvaluateSampledLightingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FEvaluateSampledLightingRGS::FParameters>();

				PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
				PassParameters->SampledLightData = SampledLightDataUniformBuffer;
				PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
				PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
				PassParameters->SceneLightingChannels = GetSceneLightingChannelParameters(GraphBuilder, LightingChannelsTexture);

				PassParameters->RWDiffuseUAV = GraphBuilder.CreateUAV(Diffuse);
				PassParameters->RWSpecularUAV = GraphBuilder.CreateUAV(Specular);
				PassParameters->RWRayDistanceUAV = GraphBuilder.CreateUAV(RayHitDistance);
				PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
				PassParameters->RWLightReservoirHistoryUAV = GraphBuilder.CreateUAV(LightReservoirsHistory);
				PassParameters->InputSlice = InitialSlice;
				PassParameters->NumReservoirs = NumReservoirs;
				PassParameters->DemodulateMaterials = CVarSampledLightingDemodulateMaterials.GetValueOnRenderThread();
				PassParameters->DebugOutput = CVarRayTracingSampledLightingDebugMode.GetValueOnRenderThread();
				PassParameters->FeedbackVisibility = CVarRayTracingSampledLightingFeedbackVisibility.GetValueOnRenderThread();

				if (bUseHairLighting)
				{
					const bool bUseHairVoxel = CVarRayTracingSampledLightingEnableHairVoxel.GetValueOnRenderThread() > 0;
					PassParameters->bUseHairVoxel = (HairResources.bUseHairVoxel && bUseHairVoxel) ? 1 : 0;
					PassParameters->HairCategorizationTexture = HairResources.CategorizationTexture;
					PassParameters->HairLightChannelMaskTexture = HairResources.LightChannelMaskTexture;
					PassParameters->VirtualVoxel = HairResources.VoxelResources->UniformBuffer;
				}

				PassParameters->SampledLightingCommonParameters = CommonParameters;

				FEvaluateSampledLightingRGS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FEvaluateSampledLightingRGS::FHairLightingDim>(bUseHairLighting);
				auto RayGenShader = GetShaderPermutation<FEvaluateSampledLightingRGS>(PermutationVector, Options, View);

				ClearUnusedGraphResources(RayGenShader, PassParameters);

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("ShadeSamples"),
					PassParameters,
					ERDGPassFlags::Compute,
					[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
				{
					FRayTracingShaderBindingsWriter GlobalResources;
					SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

					FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
					RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
				});
			}
		}

		
		// evaluate denoiser
		{
			const int32 DenoiserMode = CVarSampledLightingDenoiser.GetValueOnRenderThread();
			const IScreenSpaceDenoiser* DefaultDenoiser = IScreenSpaceDenoiser::GetDefaultDenoiser();
			const IScreenSpaceDenoiser* DenoiserToUse = DenoiserMode == 1 ? DefaultDenoiser : GScreenSpaceDenoiser;


			// this needs the NRD plugin since we are using DenoisePolychromaticPenumbraHarmonics differently than the default denoiser
			// the default DenoisePolychromaticPenumbraHarmonics denoiser also is missing shaders
			// we can't check for FNRDDenoiser in GScreenSpaceDenoiser->GetDebugName() directly since the DLSS plugin put itself into GScreenSpaceDenoiser and then passes through to the FNRDDenoiser
			// so we check for the NRD module that's part of the NRD plugin

			static IModuleInterface* NRDModule = FModuleManager::GetModulePtr<IModuleInterface>(TEXT("NRD"));
			const bool bHasNRDPluginEnabled = NRDModule != nullptr;

#if WITH_EDITOR
			if (DenoiserMode == 2 && !bHasNRDPluginEnabled)
			{
				static bool bMessageBoxShown = false;
				const bool IsUnattended = FApp::IsUnattended() || IsRunningCommandlet() || GIsRunningUnattendedScript;
				if (!IsUnattended && !bMessageBoxShown)
				{
					const FText DialogTitle(NSLOCTEXT("RaytracingRTXDISampledLighting","RTXDINRDPluginRequiredTitle", "Error - RTXDI sampled lighting requires the NRD Denoiser plugin"));
					const FTextFormat Format(NSLOCTEXT("RaytracingRTXDISampledLighting","RTXDINRDPluginRequiredMessage",
						"r.RayTracing.SampledDirectLighting (RTXDI), requires the NVIDIA Realtime Denoiser (NRD) plugin.\n\n"
						"Please enable the NRD plugin for your project and restart the engine"));
					const FText WarningMessage = FText::Format(Format, FText::FromString((TEXT(""))));
					FMessageDialog::Open(EAppMsgType::Ok, WarningMessage, &DialogTitle);
					bMessageBoxShown = true;
				}
			}
#endif //WITH_EDITOR

			if(DenoiserMode == 2 && bHasNRDPluginEnabled && DenoiserToUse != DefaultDenoiser)
			{
				RDG_EVENT_SCOPE(GraphBuilder, "%s%s(Diffuse + Specular) %dx%d",
					DenoiserToUse != DefaultDenoiser ? TEXT("ThirdParty ") : TEXT(""),
					DenoiserToUse->GetDebugName(),
					View.ViewRect.Width(), View.ViewRect.Height());

				IScreenSpaceDenoiser::FPolychromaticPenumbraHarmonics DenoiserInputs;
				DenoiserInputs.Diffuse.Harmonics[0] = Diffuse;
				DenoiserInputs.Diffuse.Harmonics[1] = RayHitDistance;
				DenoiserInputs.Specular.Harmonics[0] = Specular;
				DenoiserInputs.Specular.Harmonics[1] = RayHitDistance;

				IScreenSpaceDenoiser::FPolychromaticPenumbraOutputs DenoiserOutputs = DenoiserToUse->DenoisePolychromaticPenumbraHarmonics(
					GraphBuilder,
					View,
					(FPreviousViewInfo*)&View.PrevViewInfo,
					SceneTextures,
					DenoiserInputs
					);

				Diffuse = DenoiserOutputs.Diffuse;
				Specular = DenoiserOutputs.Specular;
			}
		}
		
		// composite
		{

			const FScreenPassRenderTarget Output(SceneColorTexture, View.ViewRect, ERenderTargetLoadAction::ELoad);

			const FScreenPassTexture SceneColor(Diffuse, View.ViewRect);

			TShaderMapRef<FScreenPassVS> VertexShader(View.ShaderMap);
			TShaderMapRef<FCompositeSampledLightingPS> PixelShader(View.ShaderMap);
			const bool bCompositeReplace = CVarSampledLightingCompositeMode.GetValueOnRenderThread() != 0;
			FRHIBlendState* BlendState = bCompositeReplace ?
				TStaticBlendState<CW_RGBA>::GetRHI()  :
				TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
			const FScreenPassTextureViewport InputViewport(SceneColorTexture->Desc.Extent, View.ViewRect);
			const FScreenPassTextureViewport OutputViewport(SceneColorTexture->Desc.Extent, View.ViewRect);

			auto Parameters = GraphBuilder.AllocParameters<FCompositeSampledLightingPS::FParameters>();

			Parameters->ApplyDiffuse = CVarSampledLightingCompositeDiffuse.GetValueOnRenderThread();
			Parameters->ApplySpecular = CVarSampledLightingCompositeSpecular.GetValueOnRenderThread();
			Parameters->ModulateMaterials = CVarSampledLightingDemodulateMaterials.GetValueOnRenderThread();

			Parameters->Diffuse = Diffuse;
			Parameters->Specular = Specular;
			Parameters->InputSampler = TStaticSamplerState<>::GetRHI();

			Parameters->SceneTextures = SceneTexturesUniformBuffer;
			Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
			Parameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);

			ClearUnusedGraphResources(PixelShader, Parameters);

			AddDrawScreenPass(
				GraphBuilder,
				RDG_EVENT_NAME("CompositeSampledLighting"),
				View,
				OutputViewport,
				InputViewport,
				VertexShader,
				PixelShader,
				BlendState,
				Parameters);
		}
	}

	if (LightingMode == 1 && !ReferenceView.bStatePrevViewInfoIsReadOnly)
	{
		//Extract history feedback here
		GraphBuilder.QueueBufferExtraction(LightReservoirsHistory, &ReferenceView.ViewState->PrevFrameViewInfo.SampledLightHistory.LightReservoirs);

		ReferenceView.ViewState->PrevFrameViewInfo.SampledLightHistory.ReservoirDimensions = ReservoirHistoryBufferDim;
	}

	//ToDo - revist light buffer lifetimes. Maybe they should be made as explict allocations from the RDG
}
#else

void FDeferredShadingSceneRenderer::RenderSampledDirectLighting(
	FRDGBuilder& GraphBuilder,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	const TArray<FSortedLightSceneInfo, SceneRenderingAllocator> &SampledLights,
	const FHairStrandsRenderingData* HairDatas,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef LightingChannelsTexture)
{
	// presently unsupported on platforms without ray tracing
	check(0);
}

#endif