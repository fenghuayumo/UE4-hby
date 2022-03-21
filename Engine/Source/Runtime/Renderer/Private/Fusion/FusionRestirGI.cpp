
#include "Fusion.h"

#ifdef RHI_RAYTRACING

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
#include "RayTracing/RaytracingOptions.h"
#include "BlueNoise.h"
#include "SceneTextureParameters.h"
#include "RayTracingDefinitions.h"
#include "RayTracing/RayTracingDeferredMaterials.h"
#include "RayTracingTypes.h"
#include "PathTracingDefinitions.h"
#include "PathTracing.h"

static TAutoConsoleVariable<int32> CVarRestirGISpatial(
	TEXT("r.RayTracing.RestirGI.Spatial"), 1,
	TEXT("Whether to apply spatial resmapling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIInitialCandidates(
	TEXT("r.RayTracing.RestirGI.InitialSamples"), 1,
	TEXT("How many lights to test sample during the initial candidate search"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIInitialCandidatesBoost(
	TEXT("r.RayTracing.RestirGI.InitialSamplesBoost"), 4,
	TEXT("How many lights to test sample during the initial candidate search when history is invalidated"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGITemporal(
	TEXT("r.RayTracing.RestirGI.Temporal"), 1,
	TEXT("Whether to use temporal resampling for the reserviors"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIApplyBoilingFilter(
	TEXT("r.RayTracing.RestirGI.ApplyBoilingFilter"), 1,
	TEXT("Whether to apply boiling filter when temporally resampling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRestirGIBoilingFilterStrength(
	TEXT("r.RayTracing.RestirGI.BoilingFilterStrength"), 0.20f,
	TEXT("Strength of Boiling filter"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingRestirGIEnableSpatialBias(
	TEXT("r.RayTracing.RestirGI.EnableSpatialBias"),
	1,
	TEXT("Enables Bias when Spatial resampling (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingRestirGIEnableTemporalBias(
	TEXT("r.RayTracing.RestirGI.EnableTemporalBias"),
	1,
	TEXT("Enables Bias when Temporal resampling (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRestirGISpatialSamplingRadius(
	TEXT("r.RayTracing.RestirGI.Spatial.SamplingRadius"), 32.0f,
	TEXT("Spatial radius for sampling in pixels (Default 32.0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGISpatialSamples(
	TEXT("r.RayTracing.RestirGI.Spatial.Samples"), 1,
	TEXT("Spatial samples per pixel"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGISpatialSamplesBoost(
	TEXT("r.RayTracing.RestirGI.Spatial.SamplesBoost"), 8,
	TEXT("Spatial samples per pixel when invalid history is detected"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRestirGISpatialNormalRejectionThreshold(
	TEXT("r.RayTracing.RestirGI.Spatial.NormalRejectionThreshold"), 0.5f,
	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRestirGISpatialDepthRejectionThreshold(
	TEXT("r.RayTracing.RestirGI.Spatial.DepthRejectionThreshold"), 0.1f,
	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGISpatialApplyApproxVisibility(
	TEXT("r.RayTracing.RestirGI.Spatial.ApplyApproxVisibility"), 0,
	TEXT("Apply an approximate visibility test on sample selected during spatial sampling"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGITemporalMaxHistory(
	TEXT("r.RayTracing.RestirGI.Temporal.MaxHistory"), 10,
	TEXT("Maximum temporal history for samples (default 10)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRestirGITemporalNormalRejectionThreshold(
	TEXT("r.RayTracing.RestirGI.Temporal.NormalRejectionThreshold"), 0.5f,
	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRestirGITemporalDepthRejectionThreshold(
	TEXT("r.RayTracing.RestirGI.Temporal.DepthRejectionThreshold"), 0.1f,
	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGITemporalApplyApproxVisibility(
	TEXT("r.RayTracing.RestirGI.Temporal.ApplyApproxVisibility"), 0,
	TEXT("Apply an approximate visibility test on sample selected during reprojection"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIDemodulateMaterials(
	TEXT("r.RayTracing.RestirGI.DemodulateMaterials"), 1,
	TEXT("Whether to demodulate the material contributiuon from the signal for denoising"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIFaceCull(
	TEXT("r.RayTracing.RestirGI.FaceCull"), 0,
	TEXT("Face culling to use for visibility tests\n")
	TEXT("  0 - none (Default)\n")
	TEXT("  1 - front faces (equivalent to backface culling in shadow maps)\n")
	TEXT("  2 - back faces"),
	ECVF_RenderThreadSafe);

static float GRayTracingRestirGIMultipleBounceRatio = 0.25;
static TAutoConsoleVariable<float> CVarRestirGILongPathRatio(
	TEXT("r.RayTracing.RestirGI.MultipleBounceRatio"),
	GRayTracingRestirGIMultipleBounceRatio,
	TEXT("long path ratio\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIApproximateVisibilityMode(
	TEXT("r.RayTracing.RestirGI.ApproximateVisibilityMode"), 0,
	TEXT("Visibility mode for approximate visibility tests (default 0/accurate)\n")
	TEXT("  0 - Accurate, any hit shaders process alpha coverage\n")
	TEXT("  1 - Force opaque, anyhit shaders ignored, alpha coverage considered 100%\n")
	TEXT("  2 - Force transparent, anyhit shaders ignored, alpha coverage considered 0%"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGINumReservoirs(
	TEXT("r.RayTracing.RestirGI.NumReservoirs"), -1,
	TEXT("Number of independent light reservoirs per pixel\n")
	TEXT("  1-N - Explicit number of reservoirs\n")
	TEXT("  -1 - Auto-select based on subsampling (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIMinReservoirs(
	TEXT("r.RayTracing.RestirGI.MinReservoirs"), 1,
	TEXT("Minimum number of light reservoirs when auto-seleting(default 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIMaxReservoirs(
	TEXT("r.RayTracing.RestirGI.MaxReservoirs"), 2,
	TEXT("Maximum number of light reservoirs when auto-seleting (default 2)"),
	ECVF_RenderThreadSafe);


static TAutoConsoleVariable<int32> CVarRayTracingRestirGIFeedbackVisibility(
	TEXT("r.RayTracing.RestirGI.FeedbackVisibility"),
	1,
	TEXT("Whether to feedback the final visibility result to the history (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIUseSurfel(
	TEXT("r.RayTracing.RestirGI.UseSurfel"),
	1,
	TEXT("Whether to Use Surfel"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIDefered(
	TEXT("r.RayTracing.RestirGI.ExperimentalDeferred"),
	1,
	TEXT("Whether to Use ExperimentalDeferred for performance"),
	ECVF_RenderThreadSafe);

	
static TAutoConsoleVariable<int32> CVarRayTracingGIGenerateRaysWithRGS(
	TEXT("r.RayTracing.GlobalIllumination.ExperimentalDeferred.GenerateRaysWithRGS"),
	1,
	TEXT("Whether to generate gi rays directly in RGS or in a separate compute shader (default: 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingGIMipBias(
	TEXT("r.RayTracing.GlobalIllumination.ExperimentalDeferred.MipBias"),
	0.0,
	TEXT("Global texture mip bias applied during ray tracing material evaluation. (default: 0)\n")
	TEXT("Improves ray tracing globalIllumination performance at the cost of lower resolution textures in gi. Values are clamped to range [0..15].\n"),
	ECVF_RenderThreadSafe
);

DECLARE_GPU_STAT_NAMED(RayTracingDeferedGI, TEXT("Ray Tracing GI: Defered"));

namespace 
{
	struct FSortedGIRay
	{
		float  Origin[3];
		uint32 PixelCoordinates; // X in low 16 bits, Y in high 16 bits
		uint32 Direction[2]; // FP16
		float  Pdf;
		float  Roughness; // Only technically need 8 bits, the rest could be repurposed
	};

	struct FGIRayIntersectionBookmark
	{
		uint32 Data[2];
	};
} // anon namespace

class FGenerateGIRaysCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateGIRaysCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateGIRaysCS, FGlobalShader);

	class FWaveOps : SHADER_PERMUTATION_BOOL("DIM_WAVE_OPS");
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, RayTracingResolution)
		SHADER_PARAMETER(FIntPoint, TileAlignedResolution)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSortedGIRay>, RayBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static uint32 GetGroupSize()
	{
		return 1024; // this shader generates rays and sorts them in 32x32 tiles using LDS
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}

};
IMPLEMENT_GLOBAL_SHADER(FGenerateGIRaysCS, "/Engine/Private/RayTracing/RayTracingGIGenerateRaysCS.usf", "GenerateGIRaysCS", SF_Compute);


class FRayTracingDeferredGIRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingDeferredGIRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingDeferredGIRGS, FGlobalShader)

	class FDeferredMaterialMode : SHADER_PERMUTATION_ENUM_CLASS("DIM_DEFERRED_MATERIAL_MODE", EDeferredMaterialMode);
	class FGenerateRays : SHADER_PERMUTATION_BOOL("DIM_GENERATE_RAYS"); // Whether to generate rays in the RGS or in a separate CS
	class FAMDHitToken : SHADER_PERMUTATION_BOOL("DIM_AMD_HIT_TOKEN");
	using FPermutationDomain = TShaderPermutationDomain<FDeferredMaterialMode, FGenerateRays, FAMDHitToken>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, RayTracingResolution)
		SHADER_PARAMETER(FIntPoint, TileAlignedResolution)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(float, TextureMipBias)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)

		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

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

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSortedGIRay>, RayBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIRayIntersectionBookmark>, BookmarkBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDeferredMaterialPayload>, MaterialBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!ShouldCompileRayTracingShadersForProject(Parameters.Platform))
		{
			return false;
		}

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FDeferredMaterialMode>() == EDeferredMaterialMode::None)
		{
			return false;
		}

		if (PermutationVector.Get<FDeferredMaterialMode>() != EDeferredMaterialMode::Gather
			&& PermutationVector.Get<FGenerateRays>())
		{
			// DIM_GENERATE_RAYS only makes sense for "gather" mode
			return false;
		}

		if (PermutationVector.Get<FAMDHitToken>() && !(IsD3DPlatform(Parameters.Platform) && IsPCPlatform(Parameters.Platform)))
		{
			return false;
		}

		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_DISPATCH_1D"), 1); // Always using 1D dispatches
		OutEnvironment.SetDefine(TEXT("ENABLE_TWO_SIDED_GEOMETRY"), 1); // Always using double-sided ray tracing for shadow rays
	}
};
IMPLEMENT_GLOBAL_SHADER(FRayTracingDeferredGIRGS, "/Engine/Private/RayTracing/RayTracingDeferedGI.usf", "GlobalIlluminationRGS", SF_RayGen);

static void AddGenerateGIRaysPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGBufferRef RayBuffer,
	const FRayTracingDeferredGIRGS::FParameters& CommonParameters)
{
	FGenerateGIRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateGIRaysCS::FParameters>();
	PassParameters->RayTracingResolution = CommonParameters.RayTracingResolution;
	PassParameters->TileAlignedResolution = CommonParameters.TileAlignedResolution;
	PassParameters->MaxNormalBias = CommonParameters.MaxNormalBias;
	PassParameters->UpscaleFactor = CommonParameters.UpscaleFactor;
	PassParameters->ViewUniformBuffer = CommonParameters.ViewUniformBuffer;
	PassParameters->SceneTextures = CommonParameters.SceneTextures;
	PassParameters->RayBuffer = GraphBuilder.CreateUAV(RayBuffer);

	FGenerateGIRaysCS::FPermutationDomain PermutationVector;
	const bool bUseWaveOps = GRHISupportsWaveOperations && GRHIMinimumWaveSize >= 32 && RHISupportsWaveOperations(View.GetShaderPlatform());
	PermutationVector.Set<FGenerateGIRaysCS::FWaveOps>(bUseWaveOps);

	auto ComputeShader = View.ShaderMap->GetShader<FGenerateGIRaysCS>(PermutationVector);
	ClearUnusedGraphResources(ComputeShader, PassParameters);

	const uint32 NumRays = CommonParameters.TileAlignedResolution.X * CommonParameters.TileAlignedResolution.Y;
	FIntVector GroupCount;
	GroupCount.X = FMath::DivideAndRoundUp(NumRays, FGenerateGIRaysCS::GetGroupSize());
	GroupCount.Y = 1;
	GroupCount.Z = 1;
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GenerateGIRays"), ComputeShader, PassParameters, GroupCount);
}

#if RHI_RAYTRACING
void FDeferredShadingSceneRenderer::RenderRayTracingDeferedGI(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingDeferedGI);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: Defered");

	const bool bGenerateRaysWithRGS = CVarRayTracingGIGenerateRaysWithRGS.GetValueOnRenderThread() == 1;

	FIntPoint RayTracingResolution = View.ViewRect.Size();
	FIntPoint RayTracingBufferSize = SceneTextures.SceneDepthTexture->Desc.Extent;

	{
		RayTracingResolution = FIntPoint::DivideAndRoundUp(RayTracingResolution, UpscaleFactor);
		RayTracingBufferSize = RayTracingBufferSize / UpscaleFactor;
	}

	FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		RayTracingBufferSize,
		PF_FloatRGBA, FClearValueBinding(FLinearColor(0, 0, 0, 0)),
		TexCreate_ShaderResource | TexCreate_UAV);

	auto TestGI = GraphBuilder.CreateTexture(OutputDesc,TEXT("RayTracingGI"));

	//FRDGTextureRef RayHitDistance;
	//{
	//	OutputDesc.Format = PF_R16F;
	//	RayHitDistance = GraphBuilder.CreateTexture(OutputDesc, TEXT("RayTracingGIHitDistance"));
	//	OutDenoiserInputs->RayHitDistance = RayHitDistance;
	//}

	const uint32 SortTileSize = 64; // Ray sort tile is 32x32, material sort tile is 64x64, so we use 64 here (tile size is not configurable).
	const FIntPoint TileAlignedResolution = FIntPoint::DivideAndRoundUp(RayTracingResolution, SortTileSize) * SortTileSize;

	FRayTracingDeferredGIRGS::FParameters CommonParameters;
	CommonParameters.UpscaleFactor = UpscaleFactor;
	CommonParameters.RayTracingResolution = RayTracingResolution;
	CommonParameters.TileAlignedResolution = TileAlignedResolution;
	CommonParameters.TextureMipBias = FMath::Clamp(CVarRayTracingGIMipBias.GetValueOnRenderThread(), 0.0f, 15.0f);
	CommonParameters.TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	CommonParameters.SceneTextures = SceneTextures;
	CommonParameters.ViewUniformBuffer = View.ViewUniformBuffer;

	CommonParameters.MaxNormalBias = GetRaytracingMaxNormalBias();
	float MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
	if (MaxRayDistanceForGI == -1.0)
	{
		MaxRayDistanceForGI = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	}
	// Shade gi points
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
	CommonParameters.MaxRayDistanceForGI = MaxRayDistanceForGI;
	CommonParameters.MaxRayDistanceForAO = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	CommonParameters.MaxShadowDistance = MaxShadowDistance;
	CommonParameters.EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	CommonParameters.UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	CommonParameters.UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	CommonParameters.DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	CommonParameters.NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	// TODO: should be converted to RDG
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	CommonParameters.SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	CommonParameters.TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	if (!CommonParameters.SceneTextures.GBufferVelocityTexture)
	{
		CommonParameters.SceneTextures.GBufferVelocityTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
	}

	const bool bHitTokenEnabled = CanUseRayTracingAMDHitToken();

	// Generate sorted gi rays

	const uint32 TileAlignedNumRays = TileAlignedResolution.X * TileAlignedResolution.Y;
	const FRDGBufferDesc SortedRayBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FSortedGIRay), TileAlignedNumRays);
	FRDGBufferRef SortedRayBuffer = GraphBuilder.CreateBuffer(SortedRayBufferDesc, TEXT("GIRayBuffer"));

	const FRDGBufferDesc DeferredMaterialBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDeferredMaterialPayload), TileAlignedNumRays);
	FRDGBufferRef DeferredMaterialBuffer = GraphBuilder.CreateBuffer(DeferredMaterialBufferDesc, TEXT("RayTracingGIMaterialBuffer"));

	const FRDGBufferDesc BookmarkBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIRayIntersectionBookmark), TileAlignedNumRays);
	FRDGBufferRef BookmarkBuffer = GraphBuilder.CreateBuffer(BookmarkBufferDesc, TEXT("RayTracingGIBookmarkBuffer"));

	if (!bGenerateRaysWithRGS)
	{
		AddGenerateGIRaysPass(GraphBuilder, View, SortedRayBuffer, CommonParameters);
	}

	// Trace gi material gather rays

	{
		FRayTracingDeferredGIRGS::FParameters& PassParameters = *GraphBuilder.AllocParameters<FRayTracingDeferredGIRGS::FParameters>();
		PassParameters = CommonParameters;
		PassParameters.MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters.RayBuffer = GraphBuilder.CreateUAV(SortedRayBuffer);
		PassParameters.BookmarkBuffer = GraphBuilder.CreateUAV(BookmarkBuffer);
		PassParameters.RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(TestGI);

		FRayTracingDeferredGIRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingDeferredGIRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(bGenerateRaysWithRGS);

		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenShader, &PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RayTracingDeferredGIGather %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			&PassParameters,
			ERDGPassFlags::Compute,
			[&PassParameters, this, &View, TileAlignedNumRays, RayGenShader](FRHICommandList& RHICmdList)
			{
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialGatherPipeline;

				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, PassParameters);
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, TileAlignedNumRays, 1);
			});
	}

	// Sort hit points by material within 64x64 (4096 element) tiles

	SortDeferredMaterials(GraphBuilder, View, 5, TileAlignedNumRays, DeferredMaterialBuffer);

	{
		FRayTracingDeferredGIRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingDeferredGIRGS::FParameters>();
		*PassParameters = CommonParameters;
		PassParameters->MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters->RayBuffer = GraphBuilder.CreateUAV(SortedRayBuffer);
		PassParameters->BookmarkBuffer = GraphBuilder.CreateUAV(BookmarkBuffer);

		SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);

		PassParameters->RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
		PassParameters->RWGlobalIlluminationRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);

		FRayTracingDeferredGIRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingDeferredGIRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(false);

		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenShader, PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RayTracingDeferredGIShade %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, TileAlignedNumRays, RayGenShader](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, TileAlignedNumRays, 1);
			});
	}

}
#else // RHI_RAYTRACING
void FDeferredShadingSceneRenderer::RenderRayTracingDeferedGI(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
{
	checkNoEntry();
}
#endif

DECLARE_GPU_STAT_NAMED(RayTracingGIRestir, TEXT("Ray Tracing GI: Restir"));

DECLARE_GPU_STAT_NAMED(RestirGenerateSample, TEXT("RestirGI: GenerateSample"));
DECLARE_GPU_STAT_NAMED(RestirGenerateSampleDefered, TEXT("RestirGI: GenerateSampleDefered"));
DECLARE_GPU_STAT_NAMED(RestirTemporalResampling, TEXT("RestirGI: TemporalResampling"));
DECLARE_GPU_STAT_NAMED(RestirSpatioalResampling, TEXT("RestirGI: SpatioalResampling"));
DECLARE_GPU_STAT_NAMED(RestirEvaluateGI, TEXT("RestirGI: EvaluateGI"));

struct RTXGI_PackedReservoir
{
	// Internal compressed GI sample data
	FIntVector4		CreationGeometry;
	FIntVector4		HitGeometry;
	FIntVector4		LightInfo;
};

BEGIN_SHADER_PARAMETER_STRUCT(FRestirGICommonParameters, )
SHADER_PARAMETER(float, MaxNormalBias)
SHADER_PARAMETER(float, MaxShadowDistance)
SHADER_PARAMETER(int32, VisibilityApproximateTestMode)
SHADER_PARAMETER(int32, VisibilityFaceCull)
SHADER_PARAMETER(int32, SupportTranslucency)
SHADER_PARAMETER(int32, MaxTemporalHistory)
SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<RTXGI_PackedReservoir>, RWGIReservoirUAV)
SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
SHADER_PARAMETER(uint32, UpscaleFactor)
SHADER_PARAMETER(float, DiffuseThreshold)
END_SHADER_PARAMETER_STRUCT()

static void ApplyRestirGIGlobalSettings(FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("USE_ALTERNATE_RNG"), 0);
	OutEnvironment.SetDefine(TEXT("USE_LDS_FOR_SPATIAL_RESAMPLE"), 1);
	// We need the skylight to do its own form of MIS because RTGI doesn't do its own
	OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);
}

class FRestirGIInitialSamplesRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRestirGIInitialSamplesRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGIInitialSamplesRGS, FGlobalShader)

		class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	class FUseSurfelDim : SHADER_PERMUTATION_BOOL("USE_SURFEL");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim, FUseSurfelDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplyRestirGIGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(int32, InitialCandidates)

		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)

		SHADER_PARAMETER(float, LongPathRatio)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, NextEventEstimationSamples)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRestirGIInitialSamplesRGS, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "GenerateInitialSamplesRGS", SF_RayGen);

class FRestirGIInitialSamplesForDeferedRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRestirGIInitialSamplesForDeferedRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGIInitialSamplesForDeferedRGS, FGlobalShader)

	// class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	// class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	class FUseSurfelDim : SHADER_PERMUTATION_BOOL("USE_SURFEL");
	class FDeferredMaterialMode : SHADER_PERMUTATION_ENUM_CLASS("DIM_DEFERRED_MATERIAL_MODE", EDeferredMaterialMode);
	class FAMDHitToken : SHADER_PERMUTATION_BOOL("DIM_AMD_HIT_TOKEN");
	using FPermutationDomain = TShaderPermutationDomain<FDeferredMaterialMode,FAMDHitToken,FUseSurfelDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!ShouldCompileRayTracingShadersForProject(Parameters.Platform))
		{
			return false;
		}

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FDeferredMaterialMode>() == EDeferredMaterialMode::None)
		{
			return false;
		}

		if (PermutationVector.Get<FAMDHitToken>() && !(IsD3DPlatform(Parameters.Platform) && IsPCPlatform(Parameters.Platform)))
		{
			return false;
		}

		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplyRestirGIGlobalSettings(OutEnvironment);
		OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_DISPATCH_1D"), 1); // Always using 1D dispatches
		OutEnvironment.SetDefine(TEXT("USE_DEFERED_GI"), 1);
		OutEnvironment.SetDefine(TEXT("ENABLE_TWO_SIDED_GEOMETRY"), 1);
		OutEnvironment.SetDefine(TEXT("ENABLE_TRANSMISSION"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(int32, InitialCandidates)

		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)

		SHADER_PARAMETER(float, LongPathRatio)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, NextEventEstimationSamples)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)

		SHADER_PARAMETER(FIntPoint, RayTracingResolution)
		SHADER_PARAMETER(FIntPoint, TileAlignedResolution)
		SHADER_PARAMETER(float, TextureMipBias)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSortedGIRay>, RayBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGIRayIntersectionBookmark>, BookmarkBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDeferredMaterialPayload>, MaterialBuffer)

		// SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDebugDiffuseUAV)
		// SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWDebugRayDistanceUAV)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRestirGIInitialSamplesForDeferedRGS, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "GenerateInitialSamplesForDeferedGIRGS", SF_RayGen);


class FRestirGITemporalResampling : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRestirGITemporalResampling)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGITemporalResampling, FGlobalShader)

	class FFUseRestirBiasDim : SHADER_PERMUTATION_INT("TEMPORAL_RESTIR_BIAS", 2);


	using FPermutationDomain = TShaderPermutationDomain<FFUseRestirBiasDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplyRestirGIGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(int32, HistoryReservoir)
		SHADER_PARAMETER(float, TemporalDepthRejectionThreshold)
		SHADER_PARAMETER(float, TemporalNormalRejectionThreshold)
		SHADER_PARAMETER(int32, ApplyApproximateVisibilityTest)
		SHADER_PARAMETER(int32, InitialCandidates)
		//SHADER_PARAMETER(int32, InitialSampleVisibility)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<RTXGI_PackedReservoir>, GIReservoirHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, NormalHistory)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRestirGITemporalResampling, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "ApplyTemporalResamplingRGS", SF_RayGen);

class FEvaluateRestirGIRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FEvaluateRestirGIRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FEvaluateRestirGIRGS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplyRestirGIGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, NumReservoirs)
		SHADER_PARAMETER(int32, DemodulateMaterials)
		//SHADER_PARAMETER(int32, DebugOutput)
		SHADER_PARAMETER(int32, FeedbackVisibility)
		SHADER_PARAMETER(uint32, bUseHairVoxel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWRayDistanceUAV)
		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<RTXGI_PackedReservoir>, RWGIReservoirHistoryUAV)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCategorizationTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairLightChannelMaskTexture)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FEvaluateRestirGIRGS, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "EvaluateRestirGILightingRGS", SF_RayGen);

class FRestirGISpatialResampling : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRestirGISpatialResampling)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGISpatialResampling, FGlobalShader)

		class FFUseRestirBiasDim : SHADER_PERMUTATION_INT("SPATIAL_RESTIR_BIAS", 2);

	using FPermutationDomain = TShaderPermutationDomain<FFUseRestirBiasDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		ApplyRestirGIGlobalSettings(OutEnvironment);
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
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)

		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)

		SHADER_PARAMETER_SRV(Buffer<float2>, NeighborOffsets)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRestirGISpatialResampling, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "ApplySpatialResamplingRGS", SF_RayGen);


class FRestirGIApplyBoilingFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRestirGIApplyBoilingFilterCS)
	SHADER_USE_PARAMETER_STRUCT(FRestirGIApplyBoilingFilterCS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		ApplyRestirGIGlobalSettings(OutEnvironment);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int32, InputSlice)
		SHADER_PARAMETER(int32, OutputSlice)
		SHADER_PARAMETER(float, BoilingFilterStrength)
		SHADER_PARAMETER(uint32, UpscaleFactor)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<RTXGI_PackedReservoir>, RWGIReservoirUAV)
		SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRestirGIApplyBoilingFilterCS, "/Engine/Private/RestirGI/BoilingFilter.usf", "BoilingFilterCS", SF_Compute);

/**
 * This buffer provides a table with a low discrepency sequence
 */
class FRestirGIDiscSampleBuffer : public FRenderResource
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
TGlobalResource<FRestirGIDiscSampleBuffer> GRestiGIDiscSampleBuffer;

void FDeferredShadingSceneRenderer::PrepareRayTracingRestirGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	if (!ShouldRenderRayTracingGlobalIllumination(View))
	{
		return;
	}
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		for (int UseSurfel = 0; UseSurfel < 2; ++UseSurfel)
		{
			FRestirGIInitialSamplesRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRestirGIInitialSamplesRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FRestirGIInitialSamplesRGS::FEnableTransmissionDim>(EnableTransmission);
			PermutationVector.Set<FRestirGIInitialSamplesRGS::FUseSurfelDim>(UseSurfel == 1);
			TShaderMapRef<FRestirGIInitialSamplesRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
	auto EnableSpatialBias = CVarRayTracingRestirGIEnableSpatialBias.GetValueOnRenderThread();
	{
		FRestirGISpatialResampling::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGISpatialResampling::FFUseRestirBiasDim>(EnableSpatialBias);
		TShaderMapRef<FRestirGISpatialResampling> RayGenShader(View.ShaderMap, PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
	auto EnableTemporalBias = CVarRayTracingRestirGIEnableTemporalBias.GetValueOnRenderThread();
	{
		FRestirGITemporalResampling::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGITemporalResampling::FFUseRestirBiasDim>(EnableTemporalBias);
		TShaderMapRef<FRestirGITemporalResampling> RayGenShader(View.ShaderMap, PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}

	{
		FEvaluateRestirGIRGS::FPermutationDomain PermutationVector;
		TShaderMapRef<FEvaluateRestirGIRGS> RayGenShader(View.ShaderMap, PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareRayTracingDeferedGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	FRayTracingDeferredGIRGS::FPermutationDomain PermutationVector;

	const bool bGenerateRaysWithRGS = CVarRayTracingGIGenerateRaysWithRGS.GetValueOnRenderThread() == 1;
	const bool bHitTokenEnabled = CanUseRayTracingAMDHitToken();

	PermutationVector.Set<FRayTracingDeferredGIRGS::FAMDHitToken>(bHitTokenEnabled);

	{
		PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(bGenerateRaysWithRGS);
		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}

	{
		PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(false); // shading is independent of how rays are generated
		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
	
	FRestirGIInitialSamplesForDeferedRGS::FPermutationDomain RestirPermutationVector;
	RestirPermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FAMDHitToken>(bHitTokenEnabled);
	{
		RestirPermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		RestirPermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FUseSurfelDim>(false);
		auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesForDeferedRGS>(RestirPermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
	for (int UseSurfel = 0; UseSurfel < 2; ++UseSurfel)
	{
		RestirPermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
		RestirPermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FUseSurfelDim>(UseSurfel == 1);
		TShaderMapRef<FRestirGIInitialSamplesForDeferedRGS> RayGenerationShader(View.ShaderMap, RestirPermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareRayTracingDeferredGIDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	const bool bGenerateRaysWithRGS = CVarRayTracingGIGenerateRaysWithRGS.GetValueOnRenderThread() == 1;
	const bool bHitTokenEnabled = CanUseRayTracingAMDHitToken();

	{
		FRayTracingDeferredGIRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingDeferredGIRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(bGenerateRaysWithRGS);

		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
	// const bool bUseSurfel = CVarRestirGIUseSurfel.GetValueOnRenderThread() == 1;
	{
		FRestirGIInitialSamplesForDeferedRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FUseSurfelDim>(false);

		auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesForDeferedRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}

}

#else
#endif

void GenerateInitialSample(FRDGBuilder& GraphBuilder, 
	FSceneTextureParameters& SceneTextures,
	FScene* Scene,
	FViewInfo& View,
	const FRestirGICommonParameters& CommonParameters, 
	SurfelBufResources* SurfelRes,
	int32 Reservoir,
	int32 InitialCandidates,
	FIntPoint RayTracingResolution)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RestirGenerateSample);
	RDG_EVENT_SCOPE(GraphBuilder, "RestirGI: GenerateSample");
	{
		FRestirGIInitialSamplesRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIInitialSamplesRGS::FParameters>();

		int32 CVarRayTracingGlobalIlluminationMaxBouncesValue = CVarRayTracingGlobalIlluminationMaxBounces.GetValueOnRenderThread();
		PassParameters->MaxBounces = CVarRayTracingGlobalIlluminationMaxBouncesValue > -1 ? CVarRayTracingGlobalIlluminationMaxBouncesValue : View.FinalPostProcessSettings.RayTracingGIMaxBounces;
		float MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
		if (MaxRayDistanceForGI == -1.0)
		{
			MaxRayDistanceForGI = View.FinalPostProcessSettings.AmbientOcclusionRadius;
		}
		PassParameters->LongPathRatio = CVarRestirGILongPathRatio.GetValueOnRenderThread();
		PassParameters->MaxRayDistanceForGI = MaxRayDistanceForGI;
		PassParameters->MaxRayDistanceForAO = View.FinalPostProcessSettings.AmbientOcclusionRadius;
		PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
		PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
		PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
		PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
		PassParameters->SceneTextures = SceneTextures;
		PassParameters->OutputSlice = Reservoir;
		PassParameters->HistoryReservoir = Reservoir;
		PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
		//PassParameters->InitialSampleVisibility = CVarRayTracingRestirGITestInitialVisibility.GetValueOnRenderThread();

		PassParameters->RestirGICommonParameters = CommonParameters;

		// TODO: should be converted to RDG
		TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
		if (!SubsurfaceProfileRT)
		{
			SubsurfaceProfileRT = GSystemTextures.BlackDummy;
		}
		PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
		//PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		bool UseSurfel = IsSurfelGIEnabled(View) && CVarRestirGIUseSurfel.GetValueOnRenderThread() != 0;
		if (SurfelRes && UseSurfel)
		{
			auto SurfelMetaBuf = SurfelRes->SurfelMetaBuf;
			auto SurfelHashKeyBuf = SurfelRes->SurfelHashKeyBuf;
			auto SurfelHashValueBuf = SurfelRes->SurfelHashValueBuf;
			auto CellIndexOffsetBuf = SurfelRes->CellIndexOffsetBuf;
			auto SurfelIndexBuf = SurfelRes->SurfelIndexBuf;
			auto SurfelVertexBuf = SurfelRes->SurfelVertexBuf;
			auto SurfelIrradianceBuf = SurfelRes->SurfelIrradianceBuf;

			PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateSRV(SurfelIrradianceBuf);
			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateSRV(CellIndexOffsetBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelIndexBuf = GraphBuilder.CreateSRV(SurfelIndexBuf, EPixelFormat::PF_R8_UINT);

			PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
		}

		FRestirGIInitialSamplesRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGIInitialSamplesRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FRestirGIInitialSamplesRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
		PermutationVector.Set<FRestirGIInitialSamplesRGS::FUseSurfelDim>(UseSurfel);
		TShaderMapRef<FRestirGIInitialSamplesRGS> RayGenShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
		ClearUnusedGraphResources(RayGenShader, PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RestirgGI-CreateInitialSamples"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters,&View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
			});
	}
}

void GenerateInitialSampleForDefered(FRDGBuilder& GraphBuilder, 
	FSceneTextureParameters& SceneTextures,
	FScene* Scene,
	FViewInfo& View,
	const FRestirGICommonParameters& RestirGICommonParameters, 
	SurfelBufResources* SurfelRes,
	int32 Reservoir,
	int32 InitialCandidates,
	const FIntPoint& RayTracingResolution)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RestirGenerateSampleDefered);
	RDG_EVENT_SCOPE(GraphBuilder, "RestirGI: GenerateSampleDefered");

	const bool bGenerateRaysWithRGS = CVarRayTracingGIGenerateRaysWithRGS.GetValueOnRenderThread() == 1;

	//FIntPoint RayTracingResolution = View.ViewRect.Size();
	//FIntPoint RayTracingBufferSize = SceneTextures.SceneDepthTexture->Desc.Extent;

	//{
	//	RayTracingResolution = FIntPoint::DivideAndRoundUp(RayTracingResolution, UpscaleFactor);
	//	RayTracingBufferSize = RayTracingBufferSize / UpscaleFactor;
	//}

	// FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
	// 	RayTracingBufferSize,
	// 	PF_FloatRGBA, FClearValueBinding(FLinearColor(0, 0, 0, 0)),
	// 	TexCreate_ShaderResource | TexCreate_UAV);

	const uint32 SortTileSize = 64; // Ray sort tile is 32x32, material sort tile is 64x64, so we use 64 here (tile size is not configurable).
	const FIntPoint TileAlignedResolution = FIntPoint::DivideAndRoundUp(RayTracingResolution, SortTileSize) * SortTileSize;

	FRestirGIInitialSamplesForDeferedRGS::FParameters CommonParameters;
	float MaxRayDistanceForGI = GRayTracingGlobalIlluminationMaxRayDistance;
	if (MaxRayDistanceForGI == -1.0)
	{
		MaxRayDistanceForGI = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	}

	CommonParameters.MaxRayDistanceForGI = MaxRayDistanceForGI;
	CommonParameters.MaxRayDistanceForAO = View.FinalPostProcessSettings.AmbientOcclusionRadius;
	CommonParameters.EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	CommonParameters.UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	CommonParameters.UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	CommonParameters.NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	CommonParameters.ViewUniformBuffer = View.ViewUniformBuffer;
	CommonParameters.SceneTextures = SceneTextures;
	CommonParameters.MaxBounces = 1;
	CommonParameters.RayTracingResolution = RayTracingResolution;
	CommonParameters.TileAlignedResolution = TileAlignedResolution;
	CommonParameters.TextureMipBias = FMath::Clamp(CVarRayTracingGIMipBias.GetValueOnRenderThread(), 0.0f, 15.0f);

	// TODO: should be converted to RDG
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	CommonParameters.SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);
	//CommonParameters.TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	if (!CommonParameters.SceneTextures.GBufferVelocityTexture)
	{
		CommonParameters.SceneTextures.GBufferVelocityTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
	}

	const bool bHitTokenEnabled = CanUseRayTracingAMDHitToken();

	// Generate sorted gi rays

	const uint32 TileAlignedNumRays = TileAlignedResolution.X * TileAlignedResolution.Y;
	const FRDGBufferDesc SortedRayBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FSortedGIRay), TileAlignedNumRays);
	FRDGBufferRef SortedRayBuffer = GraphBuilder.CreateBuffer(SortedRayBufferDesc, TEXT("GIRayBuffer"));

	const FRDGBufferDesc DeferredMaterialBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDeferredMaterialPayload), TileAlignedNumRays);
	FRDGBufferRef DeferredMaterialBuffer = GraphBuilder.CreateBuffer(DeferredMaterialBufferDesc, TEXT("RayTracingGIMaterialBuffer"));

	const FRDGBufferDesc BookmarkBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGIRayIntersectionBookmark), TileAlignedNumRays);
	FRDGBufferRef BookmarkBuffer = GraphBuilder.CreateBuffer(BookmarkBufferDesc, TEXT("RayTracingGIBookmarkBuffer"));

	// Trace gi material gather rays
	{
		FRestirGIInitialSamplesForDeferedRGS::FParameters& PassParameters = *GraphBuilder.AllocParameters<FRestirGIInitialSamplesForDeferedRGS::FParameters>();
		PassParameters = CommonParameters;
		PassParameters.MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters.RayBuffer = GraphBuilder.CreateUAV(SortedRayBuffer);
		PassParameters.BookmarkBuffer = GraphBuilder.CreateUAV(BookmarkBuffer);
		PassParameters.RestirGICommonParameters = RestirGICommonParameters;

		FRestirGIInitialSamplesForDeferedRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FUseSurfelDim>(0);

		auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesForDeferedRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenShader, &PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("InitialSamplesForDefered %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			&PassParameters,
			ERDGPassFlags::Compute,
			[&PassParameters,  &View, TileAlignedNumRays, RayGenShader](FRHICommandList& RHICmdList)
			{
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialGatherPipeline;

				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, PassParameters);
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(Pipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, TileAlignedNumRays, 1);
			});
	}

	// Sort hit points by material within 64x64 (4096 element) tiles

	SortDeferredMaterials(GraphBuilder, View, 5, TileAlignedNumRays, DeferredMaterialBuffer);

	{
		FRestirGIInitialSamplesForDeferedRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIInitialSamplesForDeferedRGS::FParameters>();
		*PassParameters = CommonParameters;
		PassParameters->MaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters->RayBuffer = GraphBuilder.CreateUAV(SortedRayBuffer);
		PassParameters->BookmarkBuffer = GraphBuilder.CreateUAV(BookmarkBuffer);
		PassParameters->OutputSlice = Reservoir;
		PassParameters->HistoryReservoir = Reservoir;
		PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
		PassParameters->RestirGICommonParameters = RestirGICommonParameters;
		SetupLightParameters(Scene, View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
		
		// FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		// RayTracingResolution,
		// PF_FloatRGBA,
		// FClearValueBinding::None,
		// TexCreate_ShaderResource | TexCreate_UAV);

		// FRDGTextureRef Diffuse = GraphBuilder.CreateTexture(Desc, TEXT("RestirGIDebugDiffuse"));

		//PassParameters->RWDebugDiffuseUAV = GraphBuilder.CreateUAV(Diffuse);

		bool UseSurfel = IsSurfelGIEnabled(View) && CVarRestirGIUseSurfel.GetValueOnRenderThread() != 0;
		if (SurfelRes && UseSurfel)
		{
			auto SurfelMetaBuf = SurfelRes->SurfelMetaBuf;
			auto SurfelHashKeyBuf = SurfelRes->SurfelHashKeyBuf;
			auto SurfelHashValueBuf = SurfelRes->SurfelHashValueBuf;
			auto CellIndexOffsetBuf = SurfelRes->CellIndexOffsetBuf;
			auto SurfelIndexBuf = SurfelRes->SurfelIndexBuf;
			auto SurfelVertexBuf = SurfelRes->SurfelVertexBuf;
			auto SurfelIrradianceBuf = SurfelRes->SurfelIrradianceBuf;

			PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateSRV(SurfelIrradianceBuf);
			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateSRV(CellIndexOffsetBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelIndexBuf = GraphBuilder.CreateSRV(SurfelIndexBuf, EPixelFormat::PF_R8_UINT);

			PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
		}

		FRestirGIInitialSamplesForDeferedRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FAMDHitToken>(bHitTokenEnabled);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Shade);
		PermutationVector.Set<FRestirGIInitialSamplesForDeferedRGS::FUseSurfelDim>(UseSurfel);
		auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesForDeferedRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenShader, PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RestirGIDeferredGIShade %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, TileAlignedNumRays, RayGenShader](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, TileAlignedNumRays, 1);
			});
	}
}

void FDeferredShadingSceneRenderer::RenderRestirGI(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
	SurfelBufResources* SurfelRes)
#if RHI_RAYTRACING
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingGIRestir);
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: RestirGI");

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

	// Intermediate lighting targets
	// FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
	// 	SceneTextures.SceneDepthTexture->Desc.Extent / UpscaleFactor,
	// 	PF_FloatRGBA,
	// 	FClearValueBinding::None,
	// 	TexCreate_ShaderResource | TexCreate_UAV);

	// FRDGTextureRef Diffuse = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIDiffuse"));

	// Desc.Format = PF_G16R16F;
	// FRDGTextureRef RayHitDistance = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIHitDistance"));

	const int32 RequestedReservoirs = CVarRestirGINumReservoirs.GetValueOnAnyThread();
	const int32 MinReservoirs = FMath::Max(CVarRestirGIMinReservoirs.GetValueOnAnyThread(), 1);
	const int32 MaxReservoirs = FMath::Max(CVarRestirGIMaxReservoirs.GetValueOnAnyThread(), 1);
	const bool SubsampledView = View.GetSecondaryViewRectSize() != View.ViewRect.Size();
	const int32 AutoReservoirs = SubsampledView ? MaxReservoirs : MinReservoirs;
	const int32 NumReservoirs = RequestedReservoirs < 0 ? AutoReservoirs : FMath::Max(RequestedReservoirs, 1);
	FIntPoint PaddedSize = FMath::DivideAndRoundUp<FIntPoint>(SceneTextures.SceneDepthTexture->Desc.Extent / UpscaleFactor, 4) * 4;

	FIntVector ReservoirBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs + 1);
	FRDGBufferDesc ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(RTXGI_PackedReservoir), ReservoirBufferDim.X * ReservoirBufferDim.Y * ReservoirBufferDim.Z);

	FRDGBufferRef GIReservoirs = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("GIReservoirs"));

	FIntVector ReservoirHistoryBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs);
	FRDGBufferDesc ReservoirHistoryDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(RTXGI_PackedReservoir), ReservoirHistoryBufferDim.X * ReservoirHistoryBufferDim.Y * ReservoirHistoryBufferDim.Z);
	FRDGBufferRef GIReservoirsHistory = GraphBuilder.CreateBuffer(ReservoirHistoryDesc, TEXT("GIReservoirsHistory"));
	// Parameters shared by ray tracing passes
	FRestirGICommonParameters CommonParameters;
	CommonParameters.MaxNormalBias = GetRaytracingMaxNormalBias();
	CommonParameters.TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	CommonParameters.RWGIReservoirUAV = GraphBuilder.CreateUAV(GIReservoirs);
	CommonParameters.ReservoirBufferDim = ReservoirBufferDim;
	CommonParameters.VisibilityApproximateTestMode = CVarRestirGIApproximateVisibilityMode.GetValueOnRenderThread();
	CommonParameters.VisibilityFaceCull = CVarRestirGIFaceCull.GetValueOnRenderThread();
	CommonParameters.SupportTranslucency = 0;
	CommonParameters.MaxTemporalHistory = FMath::Max(1, CVarRestirGITemporalMaxHistory.GetValueOnRenderThread());
	CommonParameters.UpscaleFactor = UpscaleFactor;
	CommonParameters.MaxShadowDistance = MaxShadowDistance;
	CommonParameters.DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;;
	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);

	const bool bCameraCut = !View.PrevViewInfo.SampledGIHistory.GIReservoirs.IsValid() || View.bCameraCut;
	const int32 InitialCandidates = bCameraCut ? CVarRestirGIInitialCandidatesBoost.GetValueOnRenderThread() : CVarRestirGIInitialCandidates.GetValueOnRenderThread();
	//const int32 InitialCandidates = 1;
	int32 InitialSlice = 0;
	const int32 PrevHistoryCount = View.PrevViewInfo.SampledGIHistory.ReservoirDimensions.Z;
	for (int32 Reservoir = 0; Reservoir < NumReservoirs; Reservoir++)
	{
		if( CVarRestirGIDefered.GetValueOnRenderThread() > 0)
		{
			GenerateInitialSampleForDefered(GraphBuilder, SceneTextures, Scene, View, CommonParameters, SurfelRes, Reservoir, InitialCandidates, RayTracingResolution);
		}
		else
			GenerateInitialSample(GraphBuilder, SceneTextures, Scene, View, CommonParameters, SurfelRes, Reservoir, InitialCandidates, RayTracingResolution);
		//Temporal candidate merge pass, optionally merged with initial candidate pass
		if (CVarRestirGITemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount)
		{
			RDG_GPU_STAT_SCOPE(GraphBuilder, RestirTemporalResampling);
			RDG_EVENT_SCOPE(GraphBuilder, "RestirGI: TemporalResampling");
			{
				FRestirGITemporalResampling::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGITemporalResampling::FParameters>();

				PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
				PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
				PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);

				PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
				PassParameters->InputSlice = Reservoir;
				PassParameters->OutputSlice = Reservoir;
				PassParameters->HistoryReservoir = Reservoir;
				PassParameters->TemporalDepthRejectionThreshold = FMath::Clamp(CVarRestirGITemporalDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
				PassParameters->TemporalNormalRejectionThreshold = FMath::Clamp(CVarRestirGITemporalNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
				PassParameters->ApplyApproximateVisibilityTest = CVarRestirGITemporalApplyApproxVisibility.GetValueOnAnyThread();
				PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
				//PassParameters->InitialSampleVisibility = CVarRayTracingRestirGITestInitialVisibility.GetValueOnRenderThread();

				PassParameters->GIReservoirHistory = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(View.PrevViewInfo.SampledGIHistory.GIReservoirs));
				PassParameters->NormalHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.GBufferA, GSystemTextures.BlackDummy);
				PassParameters->DepthHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.DepthBuffer, GSystemTextures.BlackDummy);

				PassParameters->RestirGICommonParameters = CommonParameters;

				FRestirGITemporalResampling::FPermutationDomain PermutationVector;
				PermutationVector.Set<FRestirGITemporalResampling::FFUseRestirBiasDim>(CVarRayTracingRestirGIEnableTemporalBias.GetValueOnRenderThread());

				auto RayGenShader = View.ShaderMap->GetShader<FRestirGITemporalResampling>(PermutationVector);
				//auto RayGenShader = GetShaderPermutation<FRestirGITemporalResampling>(PermutationVector,Options, View);

				ClearUnusedGraphResources(RayGenShader, PassParameters);

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("RestirGI-TemporalResample"),
					PassParameters,
					ERDGPassFlags::Compute,
					[PassParameters, this, &View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
					{
						FRayTracingShaderBindingsWriter GlobalResources;
						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);

					});
			}
			// Boiling filter pass to prevent runaway samples
			if (CVarRestirGIApplyBoilingFilter.GetValueOnRenderThread() != 0)
			{
				FRestirGIApplyBoilingFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIApplyBoilingFilterCS::FParameters>();

				PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

				PassParameters->RWGIReservoirUAV = GraphBuilder.CreateUAV(GIReservoirs);
				PassParameters->ReservoirBufferDim = ReservoirBufferDim;
				PassParameters->InputSlice = Reservoir;
				PassParameters->OutputSlice = Reservoir;
				PassParameters->BoilingFilterStrength = FMath::Clamp(CVarRestirGIBoilingFilterStrength.GetValueOnRenderThread(), 0.00001f, 1.0f);
				PassParameters->UpscaleFactor = UpscaleFactor;
				auto ComputeShader = View.ShaderMap->GetShader<FRestirGIApplyBoilingFilterCS>();

				ClearUnusedGraphResources(ComputeShader, PassParameters);

				FIntPoint GridSize = FMath::DivideAndRoundUp<FIntPoint>(RayTracingResolution, 16);

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoilingFilter"), ComputeShader, PassParameters, FIntVector(GridSize.X, GridSize.Y, 1));
			}
		}
	}

	// Spatial resampling passes, one per reservoir
	if (CVarRestirGISpatial.GetValueOnRenderThread() != 0)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, RestirSpatioalResampling);
		RDG_EVENT_SCOPE(GraphBuilder, "RestirGI: SpatioalResampling");
		for (int32 Reservoir = NumReservoirs; Reservoir > 0; Reservoir--)
		{
		if (CVarRestirGISpatial.GetValueOnRenderThread() != 0)
		{
			FRestirGISpatialResampling::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGISpatialResampling::FParameters>();

			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
			PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);

			PassParameters->InputSlice = Reservoir - 1;
			PassParameters->OutputSlice = Reservoir;
			PassParameters->HistoryReservoir = Reservoir - 1;
			PassParameters->SpatialSamples = FMath::Max(CVarRestirGISpatialSamples.GetValueOnRenderThread(), 1);
			PassParameters->SpatialSamplesBoost = FMath::Max(CVarRestirGISpatialSamplesBoost.GetValueOnRenderThread(), 1);
			PassParameters->SpatialSamplingRadius = FMath::Max(1.0f, CVarRestirGISpatialSamplingRadius.GetValueOnRenderThread());
			PassParameters->SpatialDepthRejectionThreshold = FMath::Clamp(CVarRestirGISpatialDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
			PassParameters->SpatialNormalRejectionThreshold = FMath::Clamp(CVarRestirGISpatialNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
			PassParameters->ApplyApproximateVisibilityTest = CVarRestirGISpatialApplyApproxVisibility.GetValueOnRenderThread();

			PassParameters->NeighborOffsetMask = GRestiGIDiscSampleBuffer.NumSamples - 1;
			PassParameters->NeighborOffsets = GRestiGIDiscSampleBuffer.DiscSampleBufferSRV;

			PassParameters->RestirGICommonParameters = CommonParameters;

			FRestirGISpatialResampling::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRestirGISpatialResampling::FFUseRestirBiasDim>(CVarRayTracingRestirGIEnableSpatialBias.GetValueOnRenderThread());

			auto RayGenShader = View.ShaderMap->GetShader<FRestirGISpatialResampling>(PermutationVector);
			//auto RayGenShader = GetShaderPermutation<FRestirGISpatialResampling>(Options, View);

			ClearUnusedGraphResources(RayGenShader, PassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RestirGI-SpatialResample"),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, this, &View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
				{
					FRayTracingShaderBindingsWriter GlobalResources;
					SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

					FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
					RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);

				});
			InitialSlice = Reservoir;
		}
		}
	}
	// Shading evaluation pass
	{
	RDG_GPU_STAT_SCOPE(GraphBuilder, RestirEvaluateGI);
	RDG_EVENT_SCOPE(GraphBuilder, "RestirGI: EvaluateGI");
	const bool bUseHairLighting = false;
	FEvaluateRestirGIRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FEvaluateRestirGIRGS::FParameters>();

	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);

	//PassParameters->RWDiffuseUAV = GraphBuilder.CreateUAV(Diffuse);
	//PassParameters->RWRayDistanceUAV = GraphBuilder.CreateUAV(RayHitDistance);
	PassParameters->RWDiffuseUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
	PassParameters->RWRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);
	PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
	PassParameters->RWGIReservoirHistoryUAV = GraphBuilder.CreateUAV(GIReservoirsHistory);
	PassParameters->InputSlice = InitialSlice;
	PassParameters->NumReservoirs = NumReservoirs;
	PassParameters->DemodulateMaterials = CVarRestirGIDemodulateMaterials.GetValueOnRenderThread();
	//PassParameters->DebugOutput = CVarRayTracingRestirGIDebugMode.GetValueOnRenderThread();
	PassParameters->FeedbackVisibility = CVarRayTracingRestirGIFeedbackVisibility.GetValueOnRenderThread();
	PassParameters->RestirGICommonParameters = CommonParameters;

	FEvaluateRestirGIRGS::FPermutationDomain PermutationVector;
	auto RayGenShader = View.ShaderMap->GetShader<FEvaluateRestirGIRGS>();
	ClearUnusedGraphResources(RayGenShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RestirGI-ShadeSamples"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, this, &View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
		{
			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
		});
	}

	if (!View.bStatePrevViewInfoIsReadOnly)
	{
		//Extract history feedback here
		GraphBuilder.QueueBufferExtraction(GIReservoirsHistory, &View.ViewState->PrevFrameViewInfo.SampledGIHistory.GIReservoirs);

		View.ViewState->PrevFrameViewInfo.SampledGIHistory.ReservoirDimensions = ReservoirHistoryBufferDim;
	}
}
#else
void FDeferredShadingSceneRenderer::RenderRestirGI(
FRDGBuilder& GraphBuilder,
FSceneTextureParameters& SceneTextures,
FViewInfo& View,
const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
int32 UpscaleFactor,
IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
SurfelBufResources* SurfelRes)
{
	checkNoEntry();
}
#endif