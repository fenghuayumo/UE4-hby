
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
	TEXT("r.RayTracing.RestirGI.InitialSamples"), 2,
	TEXT("How many lights to test sample during the initial candidate search"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRestirGIInitialCandidatesBoost(
	TEXT("r.RayTracing.RestirGI.InitialSamplesBoost"), 8,
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

DECLARE_GPU_STAT_NAMED(RayTracingGIRestir, TEXT("Ray Tracing GI: Restir"));

DECLARE_GPU_STAT_NAMED(RestirGenerateSample, TEXT("Ray Tracing GI: GenerateSample"));
DECLARE_GPU_STAT_NAMED(RestirTemporalResampling, TEXT("Ray Tracing GI: TemporalResampling"));
DECLARE_GPU_STAT_NAMED(RestirSpatioalResampling, TEXT("Ray Tracing GI: SpatioalResampling"));
DECLARE_GPU_STAT_NAMED(RestirEvaluateGI, TEXT("Ray Tracing GI: EvaluateGI"));

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
SHADER_PARAMETER(int32, InexactShadows)
SHADER_PARAMETER(float, MaxBiasForInexactGeometry)
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
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWGlobalIlluminationRayDistanceUAV)
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
#else
#endif

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
	RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: Ressampling");

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
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		SceneTextures.SceneDepthTexture->Desc.Extent / UpscaleFactor,
		PF_FloatRGBA,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef Diffuse = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIDiffuse"));

	Desc.Format = PF_G16R16F;
	FRDGTextureRef RayHitDistance = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIHitDistance"));

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
	CommonParameters.InexactShadows = 0;
	CommonParameters.MaxBiasForInexactGeometry = 0.0f;
	CommonParameters.MaxTemporalHistory = FMath::Max(1, CVarRestirGITemporalMaxHistory.GetValueOnRenderThread());
	CommonParameters.UpscaleFactor = UpscaleFactor;
	CommonParameters.MaxShadowDistance = MaxShadowDistance;
	CommonParameters.DiffuseThreshold = GRayTracingGlobalIlluminationDiffuseThreshold;;
	FIntPoint LightingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);

	const bool bCameraCut = !View.PrevViewInfo.SampledGIHistory.GIReservoirs.IsValid() || View.bCameraCut;
	const int32 InitialCandidates = bCameraCut ? CVarRestirGIInitialCandidatesBoost.GetValueOnRenderThread() : CVarRestirGIInitialCandidates.GetValueOnRenderThread();
	//const int32 InitialCandidates = 1;
	int32 InitialSlice = 0;
	const int32 PrevHistoryCount = View.PrevViewInfo.SampledGIHistory.ReservoirDimensions.Z;
	for (int32 Reservoir = 0; Reservoir < NumReservoirs; Reservoir++)
	{
		{
			/*RDG_GPU_STAT_SCOPE(GraphBuilder, RestirGenerateSample);
			RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: GenerateSample");*/
			FRestirGIInitialSamplesRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIInitialSamplesRGS::FParameters>();

			PassParameters->InitialCandidates = InitialCandidates;
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
			PassParameters->RWGlobalIlluminationUAV = GraphBuilder.CreateUAV(Diffuse);
			PassParameters->RWGlobalIlluminationRayDistanceUAV = GraphBuilder.CreateUAV(RayHitDistance);

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
			TShaderMapRef<FRestirGIInitialSamplesRGS> RayGenShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
			ClearUnusedGraphResources(RayGenShader, PassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RestirgGI-CreateInitialSamples"),
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
		//Temporal candidate merge pass, optionally merged with initial candidate pass
		if (CVarRestirGITemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount)
		{
			/*	RDG_GPU_STAT_SCOPE(GraphBuilder, RestirTemporalResampling);
			RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: TemporalResampling");*/
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
					[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
					{
						FRayTracingShaderBindingsWriter GlobalResources;
						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);

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

				FIntPoint GridSize = FMath::DivideAndRoundUp<FIntPoint>(LightingResolution, 16);

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoilingFilter"), ComputeShader, PassParameters, FIntVector(GridSize.X, GridSize.Y, 1));
			}
		}
	}

	// Spatial resampling passes, one per reservoir
	if (CVarRestirGISpatial.GetValueOnRenderThread() != 0)
	{
		/*	RDG_GPU_STAT_SCOPE(GraphBuilder, RestirSpatioalResampling);
			RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: SpatioalResampling");*/
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
	}
	// Shading evaluation pass
	{
	//RDG_GPU_STAT_SCOPE(GraphBuilder, RestirEvaluateGI);
	//RDG_EVENT_SCOPE(GraphBuilder, "Ray Tracing GI: EvaluateGI");
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
		[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
		{
			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
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
{
	
}
#endif