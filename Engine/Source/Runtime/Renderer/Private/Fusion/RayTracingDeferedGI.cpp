#include "Fusion.h"

#include "RayTracing/RayTracingLighting.h"
#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"
#include "SceneRenderTargets.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "RHIResources.h"
#include "UniformBuffer.h"
#include "RayGenShaderUtils.h"
#include "ScreenSpaceDenoise.h"
#include "ClearQuad.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"
#include "RayTracing/RaytracingOptions.h"
#include "BlueNoise.h"
#include "RayTracingDefinitions.h"
#include "RayTracing/RayTracingDeferredMaterials.h"
#include "RayTracingTypes.h"
#include "PathTracingDefinitions.h"

#if RHI_RAYTRACING

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
}

void FDeferredShadingSceneRenderer::PrepareRayTracingDeferredGIDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	const bool bGenerateRaysWithRGS = CVarRayTracingGIGenerateRaysWithRGS.GetValueOnRenderThread() == 1;
	const bool bHitTokenEnabled = CanUseRayTracingAMDHitToken();

	FRayTracingDeferredGIRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRayTracingDeferredGIRGS::FAMDHitToken>(bHitTokenEnabled);
	PermutationVector.Set<FRayTracingDeferredGIRGS::FDeferredMaterialMode>(EDeferredMaterialMode::Gather);
	PermutationVector.Set<FRayTracingDeferredGIRGS::FGenerateRays>(bGenerateRaysWithRGS);

	auto RayGenShader = View.ShaderMap->GetShader<FRayTracingDeferredGIRGS>(PermutationVector);
	OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
}

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