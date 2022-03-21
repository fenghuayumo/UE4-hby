#include "Fusion.h"
// UE4 public interfaces
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

#include <cmath>
// local includes
#include "WRCVolumeComponent.h"
// #include "WRCVolumeDescGPU.h"


static TAutoConsoleVariable<int> CVarWRCProbesTextureVis(
	TEXT("r.RayTracing.WRC.ProbesTextureVis"),
	0,
	TEXT("If 1, will render what the probes see. If 2, will show misses (blue), hits (green), backfaces (red). \'vis WRCProbesTexure\' to see the output.\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int> CVarWRCUseSurfel(
	TEXT("r.RayTracing.WRC.UseSurfel"),
	1,
	TEXT("If 1, will use surfel to calculate radiance cache\n"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingRadianceCacheSamplesPerPixel = 1;
static FAutoConsoleVariableRef CVarRayTracingRadianceCacheSamplesPerPixel(
	TEXT("r.RayTracing.WRC.SamplesPerPixel"),
	GRayTracingRadianceCacheSamplesPerPixel,
	TEXT("Samples per pixel (default = 1 )"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int> CVarWRCDebugProbeRadiance(
	TEXT("r.RayTracing.WRC.DebugProbeRadiance"),
	0,
	TEXT("If 1, will show radiance cache\n"),
	ECVF_RenderThreadSafe);

	
DECLARE_GPU_STAT_NAMED(WRCGI_Update, TEXT("WRC Update "))
DECLARE_GPU_STAT_NAMED(WRCGI_DebugRadiance, TEXT("WRC DebugRadiance "))

#if RHI_RAYTRACING
#endif

extern void WRCProbeRenderDiffuseIndirectVisualizations(
	const FScene& Scene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder);

BEGIN_SHADER_PARAMETER_STRUCT(FWRCProbeParameterCommon, )

	SHADER_PARAMETER(FVector,       VolumeProbeOrigin)
	SHADER_PARAMETER(int,           NumRaysPerProbe)
	SHADER_PARAMETER(FVector4,      VolumeProbeRotation)
	SHADER_PARAMETER(FVector,       ProbeGridSpacing)
	SHADER_PARAMETER(float,         ProbeMaxRayDistance)
	SHADER_PARAMETER(FIntVector,    ProbeGridCounts)
	SHADER_PARAMETER(uint32, 		ProbeDim)
	SHADER_PARAMETER(uint32,		AtlasProbeCount)
END_SHADER_PARAMETER_STRUCT()


class FRadianceProbeTraceRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRadianceProbeTraceRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRadianceProbeTraceRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	class FUseSurfelDim : SHADER_PERMUTATION_BOOL("USE_SURFEL");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim, FUseSurfelDim>;
	
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);

		// Set to 1 to be able to visualize this in the editor by typing "vis DDGIVolumeUpdateDebug" and later "vis none" to make it go away.
		// Set to 0 to disable and deadstrip everything related
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);

	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_STRUCT_INCLUDE(FWRCProbeParameterCommon, ProbeData)

		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, AccumulateEmissive)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RadianceDataUAV)
		SHADER_PARAMETER(FVector, 	Volume_Radius)
		SHADER_PARAMETER(float, 	ProbeBlendHistoryWeight)

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRadianceProbeTraceRGS, "/Engine/Private/WRC/ProbeTraceRGS.usf", "ProbeTraceRGS", SF_RayGen);


class FDebugProbeRadianceRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDebugProbeRadianceRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FDebugProbeRadianceRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	class FUseSurfelDim : SHADER_PERMUTATION_BOOL("USE_SURFEL");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim, FUseSurfelDim>;
	
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);

		// Set to 1 to be able to visualize this in the editor by typing "vis DDGIVolumeUpdateDebug" and later "vis none" to make it go away.
		// Set to 0 to disable and deadstrip everything related
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);

	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_STRUCT_INCLUDE(FWRCProbeParameterCommon, ProbeData)

		SHADER_PARAMETER(uint32, SamplesPerPixel)
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(float, MaxRayDistanceForGI)
		SHADER_PARAMETER(float, MaxRayDistanceForAO)
		SHADER_PARAMETER(float, MaxShadowDistance)
		SHADER_PARAMETER(float, NextEventEstimationSamples)
		SHADER_PARAMETER(float, DiffuseThreshold)
		SHADER_PARAMETER(uint32, EvalSkyLight)
		SHADER_PARAMETER(uint32, UseRussianRoulette)
		SHADER_PARAMETER(uint32, UseFireflySuppression)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(uint32, AccumulateEmissive)
		SHADER_PARAMETER(uint32, UpscaleFactor)
		
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceAtlasTex)
		SHADER_PARAMETER_SAMPLER(SamplerState,  ProbeSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDebugGIUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWGIRayDistanceUAV)
		
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FDebugProbeRadianceRGS, "/Engine/Private/WRC/ProbeDebugRadiance.usf", "GlobalIlluminationRGS", SF_RayGen);


void FDeferredShadingSceneRenderer::PrepareRayTracingWRCGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();
	//const bool bEnableAdaptive = CVarGlobalIlluminationAdaptiveSamplingEnable.GetValueOnRenderThread();
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		for (int UseSurfel = 0; UseSurfel < 2; ++UseSurfel)
		{
			{
				FRadianceProbeTraceRGS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FRadianceProbeTraceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
				PermutationVector.Set<FRadianceProbeTraceRGS::FEnableTransmissionDim>(EnableTransmission);
				PermutationVector.Set<FRadianceProbeTraceRGS::FUseSurfelDim>(UseSurfel == 1);

				TShaderMapRef<FRadianceProbeTraceRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
				OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
			}
			{

				FDebugProbeRadianceRGS::FPermutationDomain DebugProbePermutationVector;
				DebugProbePermutationVector.Set<FDebugProbeRadianceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
				DebugProbePermutationVector.Set<FDebugProbeRadianceRGS::FEnableTransmissionDim>(EnableTransmission);
				DebugProbePermutationVector.Set<FDebugProbeRadianceRGS::FUseSurfelDim>(UseSurfel == 1);

				TShaderMapRef<FDebugProbeRadianceRGS> DebugRayGenerationShader(View.ShaderMap, DebugProbePermutationVector);
				OutRayGenShaders.Add(DebugRayGenerationShader.GetRayTracingShader());

			}
		}
	}
}

void WRCUpdateVolume_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FSceneTextureParameters& SceneTextures, FWRCVolumeSceneProxy* VolProxy);
void WRCUpdateVolume_RenderThread_RTRadiance(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FSceneTextureParameters& SceneTextures, FWRCVolumeSceneProxy* VolProxy);

void WRCDebugProbeRadiance(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs, 
	FWRCVolumeSceneProxy* VolProxy);


void WRCUpdateVolume_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FSceneTextureParameters& SceneTextures, FWRCVolumeSceneProxy* VolProxy)
{
	// Early out if ray tracing is not enabled
	if (!ShouldRenderRayTracingEffect(true) || View.RayTracingScene.RayTracingSceneRHI == nullptr) return;
	// ASSUMES RENDERTHREAD
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	check(VolProxy);


	WRCUpdateVolume_RenderThread_RTRadiance(Scene, View, GraphBuilder, SceneTextures, VolProxy);

}

void WRCUpdateVolume_RenderThread_RTRadiance(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FSceneTextureParameters& SceneTextures,
	FWRCVolumeSceneProxy* VolProxy)
{
	// Deal with probe ray budgets, and updating probes in a round robin fashion within the volume

	int ProbeCount = VolProxy->ComponentData.GetProbeCount();

	if (!View.ViewState) return ;

	int32 RayTracingGISamplesPerPixel = GRayTracingRadianceCacheSamplesPerPixel;
	if (RayTracingGISamplesPerPixel <= 0) return ;

	// TODO: use raydispatchindirect will be better for perfermance
	
	float MaxShadowDistance = 1.0e27;
	if (GRayTracingGlobalIlluminationMaxShadowDistance > 0.0)
	{
		MaxShadowDistance = GRayTracingGlobalIlluminationMaxShadowDistance;
	}
	else if (Scene.SkyLight)
	{
		// Adjust ray TMax so shadow rays do not hit the sky sphere 
		MaxShadowDistance = FMath::Max(0.0, 0.99 * Scene.SkyLight->SkyDistanceThreshold);
	}

	FRadianceProbeTraceRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRadianceProbeTraceRGS::FParameters>();
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
	PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	SetupLightParameters(const_cast<FScene*>(&Scene), View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
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
	FWRCProbeParameterCommon ProbeDataParam;

	FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
	FVector probeGridSpacing;
	probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
	probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
	probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

	int AtlasProbeCount =  1 << FMath::CeilLogTwo(sqrt(ProbeCount));
	int TexelDim = VolProxy->ComponentData.GetProbeTexelDim();
	ProbeDataParam.VolumeProbeOrigin = VolProxy->ComponentData.Origin;
	FQuat rotation = VolProxy->ComponentData.Transform.GetRotation();
	ProbeDataParam.VolumeProbeRotation = FVector4{ rotation.X, rotation.Y, rotation.Z, rotation.W };
	ProbeDataParam.ProbeGridSpacing = probeGridSpacing;
	ProbeDataParam.ProbeGridCounts = VolProxy->ComponentData.ProbeCounts;
	ProbeDataParam.NumRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
	ProbeDataParam.ProbeDim = TexelDim;
	ProbeDataParam.AtlasProbeCount = AtlasProbeCount;
	ProbeDataParam.ProbeMaxRayDistance = VolProxy->ComponentData.ProbeMaxRayDistance;

	PassParameters->ProbeData = ProbeDataParam;

	FRDGTextureRef ProbesRadianceTex = GraphBuilder.RegisterExternalTexture(VolProxy->ProbesRadiance);
	PassParameters->RadianceDataUAV = GraphBuilder.CreateUAV(ProbesRadianceTex);
	
	PassParameters->Volume_Radius = VolProxy->ComponentData.Transform.GetScale3D() * 100.0f;

	PassParameters->ProbeBlendHistoryWeight = VolProxy->ComponentData.ProbeHistoryWeight;

	bool UseSurfel = IsSurfelGIEnabled(View) && CVarWRCUseSurfel.GetValueOnRenderThread() != 0 && View.ViewState->SurfelIrradianceBuf;
	if ( UseSurfel)
	{
		auto SurfelMetaBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelMetaBuf, TEXT("SurfelMetaBuf"));
		auto SurfelHashKeyBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashKeyBuf, TEXT("SurfelHashKeyBuf"));
		auto SurfelHashValueBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashValueBuf, TEXT("SurfelHashValueBuf"));
		auto CellIndexOffsetBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->CellIndexOffsetBuf, TEXT("CellIndexOffsetBuf"));
		auto SurfelIndexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIndexBuf, TEXT("SurfelIndexBuf"));
		auto SurfelVertexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelVertexBuf, TEXT("SurfelVertexBuf"));
		auto SurfelIrradianceBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIrradianceBuf, TEXT("SurfelIrradianceBuf"));

		PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateSRV(SurfelIrradianceBuf);
		PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateSRV(CellIndexOffsetBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelIndexBuf = GraphBuilder.CreateSRV(SurfelIndexBuf, EPixelFormat::PF_R8_UINT);

		PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
	}
	FRadianceProbeTraceRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRadianceProbeTraceRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
	PermutationVector.Set<FRadianceProbeTraceRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
	PermutationVector.Set<FRadianceProbeTraceRGS::FUseSurfelDim>(UseSurfel);

	const auto FeatureLevel = GMaxRHIFeatureLevel;
	auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FRadianceProbeTraceRGS> RayGenerationShader(ShaderMap, PermutationVector);
	ClearUnusedGraphResources(RayGenerationShader, PassParameters);

	FIntPoint DispatchSize = FIntPoint(VolProxy->ComponentData.GetNumRaysPerProbe(), ProbeCount);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("WRC RTRadiance %dx%d", DispatchSize.X, DispatchSize.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, &View, RayGenerationShader, DispatchSize](FRHICommandList& RHICmdList)
		{
			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchSize.X, DispatchSize.Y);
		}
	);

	GraphBuilder.QueueTextureExtraction(ProbesRadianceTex, &VolProxy->ProbesRadiance);
}

void WRCDebugProbeRadiance(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const FScene& Scene,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
	FWRCVolumeSceneProxy* VolProxy)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, WRCGI_DebugRadiance);
	RDG_EVENT_SCOPE(GraphBuilder, "WRCGI Debug");
	// Deal with probe ray budgets, and updating probes in a round robin fashion within the volume
	int ProbeCount = VolProxy->ComponentData.GetProbeCount();

	if (!View.ViewState) return ;

	int32 RayTracingGISamplesPerPixel = GRayTracingRadianceCacheSamplesPerPixel;
	if (RayTracingGISamplesPerPixel <= 0) return ;

	// TODO: use raydispatchindirect will be better for perfermance
	
	float MaxShadowDistance = 1.0e27;
	if (GRayTracingGlobalIlluminationMaxShadowDistance > 0.0)
	{
		MaxShadowDistance = GRayTracingGlobalIlluminationMaxShadowDistance;
	}
	else if (Scene.SkyLight)
	{
		// Adjust ray TMax so shadow rays do not hit the sky sphere 
		MaxShadowDistance = FMath::Max(0.0, 0.99 * Scene.SkyLight->SkyDistanceThreshold);
	}

	FDebugProbeRadianceRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDebugProbeRadianceRGS::FParameters>();
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
	PassParameters->EvalSkyLight = GRayTracingGlobalIlluminationEvalSkyLight != 0;
	PassParameters->UseRussianRoulette = GRayTracingGlobalIlluminationUseRussianRoulette != 0;
	PassParameters->UseFireflySuppression = CVarRayTracingGlobalIlluminationFireflySuppression.GetValueOnRenderThread() != 0;
	PassParameters->DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;
	PassParameters->NextEventEstimationSamples = GRayTracingGlobalIlluminationNextEventEstimationSamples;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	SetupLightParameters(const_cast<FScene*>(&Scene), View, GraphBuilder, &PassParameters->SceneLights, &PassParameters->SceneLightCount, &PassParameters->SkylightParameters);
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
	FWRCProbeParameterCommon ProbeDataParam;
	FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
	FVector probeGridSpacing;
	probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
	probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
	probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

	int AtlasProbeCount =  1 << FMath::CeilLogTwo(sqrt(ProbeCount));
	int TexelDim = VolProxy->ComponentData.GetProbeTexelDim();
	ProbeDataParam.VolumeProbeOrigin = VolProxy->ComponentData.Origin;
	FQuat rotation = VolProxy->ComponentData.Transform.GetRotation();
	ProbeDataParam.VolumeProbeRotation = FVector4{ rotation.X, rotation.Y, rotation.Z, rotation.W };
	ProbeDataParam.ProbeGridSpacing = probeGridSpacing;
	ProbeDataParam.ProbeGridCounts = VolProxy->ComponentData.ProbeCounts;
	ProbeDataParam.NumRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
	ProbeDataParam.ProbeDim = TexelDim;
	ProbeDataParam.AtlasProbeCount = AtlasProbeCount;
	ProbeDataParam.ProbeMaxRayDistance = VolProxy->ComponentData.ProbeMaxRayDistance;

	PassParameters->ProbeData = ProbeDataParam;

	FRDGTextureRef ProbesRadianceTex = GraphBuilder.RegisterExternalTexture(VolProxy->ProbesRadiance);
	PassParameters->RadianceAtlasTex = ProbesRadianceTex;
	PassParameters->ProbeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->RWDebugGIUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->Color);
	PassParameters->RWGIRayDistanceUAV = GraphBuilder.CreateUAV(OutDenoiserInputs->RayHitDistance);
	bool UseSurfel = IsSurfelGIEnabled(View) && CVarWRCUseSurfel.GetValueOnRenderThread() != 0 && View.ViewState->SurfelIrradianceBuf;
	if ( UseSurfel)
	{
		auto SurfelMetaBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelMetaBuf, TEXT("SurfelMetaBuf"));
		auto SurfelHashKeyBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashKeyBuf, TEXT("SurfelHashKeyBuf"));
		auto SurfelHashValueBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashValueBuf, TEXT("SurfelHashValueBuf"));
		auto CellIndexOffsetBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->CellIndexOffsetBuf, TEXT("CellIndexOffsetBuf"));
		auto SurfelIndexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIndexBuf, TEXT("SurfelIndexBuf"));
		auto SurfelVertexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelVertexBuf, TEXT("SurfelVertexBuf"));
		auto SurfelIrradianceBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIrradianceBuf, TEXT("SurfelIrradianceBuf"));

		PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateSRV(SurfelIrradianceBuf);
		PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateSRV(CellIndexOffsetBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelIndexBuf = GraphBuilder.CreateSRV(SurfelIndexBuf, EPixelFormat::PF_R8_UINT);

		PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
	}
	FDebugProbeRadianceRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FDebugProbeRadianceRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
	PermutationVector.Set<FDebugProbeRadianceRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
	PermutationVector.Set<FDebugProbeRadianceRGS::FUseSurfelDim>(UseSurfel);

	const auto FeatureLevel = GMaxRHIFeatureLevel;
	auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FDebugProbeRadianceRGS> RayGenerationShader(ShaderMap, PermutationVector);
	ClearUnusedGraphResources(RayGenerationShader, PassParameters);

	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("DebugProbeRadiance %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
	{
		FRayTracingShaderBindingsWriter GlobalResources;
		SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

		FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
		RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
	});

}

void FDeferredShadingSceneRenderer::WRCTrace(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	// Gather the list of volumes to update and load data if it's available.
	TArray<FWRCVolumeSceneProxy*> sceneVolumes;
	for (FWRCVolumeSceneProxy* proxy : FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread)
	{
		// Don't update the volume if it isn't part of the current scene
		if (proxy->OwningScene != Scene) continue;

		// Don't update the volume if it is disabled
		if (!proxy->ComponentData.EnableVolume) continue;

		sceneVolumes.Add(proxy);
	}

#if RHI_RAYTRACING
	if (sceneVolumes.Num() <= 0) return;
	//allways use index 0
	WRCUpdateVolume_RenderThread(*Scene, View, GraphBuilder, SceneTextures, sceneVolumes[0]);

#endif // RHI_RAYTRACING

}


void FDeferredShadingSceneRenderer::RenderWRC(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
{
	if (!View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, WRCGI_Update);
		RDG_EVENT_SCOPE(GraphBuilder, "WRCGI Update");
		// WRCTrace(GraphBuilder, SceneTextures,View);
		TArray<FWRCVolumeSceneProxy*> sceneVolumes;
		for (FWRCVolumeSceneProxy* proxy : FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread)
		{
			// Don't update the volume if it isn't part of the current scene
			if (proxy->OwningScene != Scene) continue;

			// Don't update the volume if it is disabled
			if (!proxy->ComponentData.EnableVolume) continue;

			sceneVolumes.Add(proxy);
		}
		if (sceneVolumes.Num() <= 0) return;
		//allways use index 0
		WRCUpdateVolume_RenderThread(*Scene, View, GraphBuilder, SceneTextures, sceneVolumes[0]);

		if (CVarWRCDebugProbeRadiance.GetValueOnRenderThread() > 0)
		{
			WRCDebugProbeRadiance(GraphBuilder, SceneTextures, View, *Scene, UpscaleFactor, OutDenoiserInputs, sceneVolumes[0]);
		}

	}

	//
	
	
	WRCProbeRenderDiffuseIndirectVisualizations(*Scene, View, GraphBuilder);
}