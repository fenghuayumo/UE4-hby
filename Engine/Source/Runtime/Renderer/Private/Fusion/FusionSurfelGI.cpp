#include "Fusion.h"
#include "SurfelTypes.h"
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

static int32 GSurfelGISamplesPerPixel = 1;
static FAutoConsoleVariableRef CVarSurfelGISamplesPerPixel(
	TEXT("r.RayTracing.SurfelGI.SamplesPerPixel"),
	GSurfelGISamplesPerPixel,
	TEXT("Samples per pixel (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarSurfelGIUseSurfel(
	TEXT("r.RayTracing.SurfelGI.UseSurfel"),
	1,
	TEXT("Whether to Use Surfel"),
	ECVF_RenderThreadSafe);

DECLARE_GPU_STAT_NAMED(SurfelGITotal, TEXT("Surfel GI"))
DECLARE_GPU_STAT_NAMED(SurfelAllocate, TEXT("Surfel GI: AllocateSurfel"));
DECLARE_GPU_STAT_NAMED(SurfelBin, TEXT("Surfel GI: SurfelBin"));
DECLARE_GPU_STAT_NAMED(TraceSurfel, TEXT("Surfel GI: SurfelTrace"));
DECLARE_GPU_STAT_NAMED(ApplySurfel, TEXT("Surfel GI: ApplySurfel"));
DECLARE_GPU_STAT_NAMED(SurfelInclusivePrefixScan, TEXT("Surfel GI: InclusivePrefixScan"));


struct FSurfelVertexPacked
{
	FVector4   data0;
	FVector4   data1;
};

class FAllocateSurfelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAllocateSurfelCS)
	SHADER_USE_PARAMETER_STRUCT(FAllocateSurfelCS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("ALLOCATE_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelGridMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelEntryCellBuf)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelLifeBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelPoolBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelRePositionBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelRePositionCountBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)



		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDebugOutTex)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)

		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FAllocateSurfelCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "AllocateSurfels", SF_Compute);

class FAgeSurfelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAgeSurfelCS)
	SHADER_USE_PARAMETER_STRUCT(FAgeSurfelCS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("AGE_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 64;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelGridMetaBuf)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelLifeBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelPoolBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelRePositionBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelRePositionCountBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelEntryCellBuf)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER(ByteAddressBuffer, IndirectDispatchArgs)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FAgeSurfelCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "AgeSurfelCS", SF_Compute);

class FClearSurfelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearSurfelCS)
	SHADER_USE_PARAMETER_STRUCT(FClearSurfelCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("CLEAR_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 64;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelLifeBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelPoolBuf)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FClearSurfelCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "ClearSurfelCS", SF_Compute);

class FDispatchSurfelArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDispatchSurfelArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FDispatchSurfelArgsCS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.SetDefine(TEXT("PRE_DISPATCH_SURFEL_ARGS"), 1);
	}


	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, IndirectDispatchArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FDispatchSurfelArgsCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "PrepareDispatchArgs", SF_Compute);

class FSurfelTraceRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSurfelTraceRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FSurfelTraceRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
		class FUseSurfelDim : SHADER_PERMUTATION_BOOL("USE_SURFEL");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim,FUseSurfelDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		//OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		// We need the skylight to do its own form of MIS because RTGI doesn't do its own
		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);
		//OutEnvironment.SetDefine(TEXT("LIGHT_SAMPLING_TYPE"), 1);
		OutEnvironment.SetDefine(TEXT("SURFEL_TRACE"), 1);
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

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)
		//surfel gi
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelGridMetaBuf)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelLifeBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelPoolBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelRePositionBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelRePositionCountBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint> , SurfelEntryCellBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelAuxiBuf)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSurfelTraceRGS, "/Engine/Private/SurfelGI/SurfelTraceRGS.usf", "SurfelTraceRGS", SF_RayGen);

// class FApplySurfelRGS : public FGlobalShader
// {
// 	DECLARE_GLOBAL_SHADER(FApplySurfelRGS)
// 	SHADER_USE_ROOT_PARAMETER_STRUCT(FApplySurfelRGS, FGlobalShader)

// 	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
// 	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
// 	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim>;

// 	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
// 	{
// 		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
// 	}

// 	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
// 	{
// 		//OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
// 		// We need the skylight to do its own form of MIS because RTGI doesn't do its own
// 		OutEnvironment.SetDefine(TEXT("PATHTRACING_SKY_MIS"), 1);
// 		//OutEnvironment.SetDefine(TEXT("LIGHT_SAMPLING_TYPE"), 1);
// 		//OutEnvironment.SetDefine(TEXT("SURFEL_TRACE"), 1);
// 	}

// 	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
// 		SHADER_PARAMETER(uint32, SamplesPerPixel)
// 		SHADER_PARAMETER(uint32, MaxBounces)
// 		SHADER_PARAMETER(uint32, UpscaleFactor)
// 		SHADER_PARAMETER(float, MaxRayDistanceForGI)
// 		SHADER_PARAMETER(float, MaxRayDistanceForAO)
// 		SHADER_PARAMETER(float, MaxShadowDistance)
// 		SHADER_PARAMETER(float, NextEventEstimationSamples)
// 		SHADER_PARAMETER(float, DiffuseThreshold)
// 		SHADER_PARAMETER(uint32, EvalSkyLight)
// 		SHADER_PARAMETER(uint32, UseRussianRoulette)
// 		SHADER_PARAMETER(uint32, UseFireflySuppression)
// 		SHADER_PARAMETER(float, MaxNormalBias)
// 		SHADER_PARAMETER(uint32, RenderTileOffsetX)
// 		SHADER_PARAMETER(uint32, RenderTileOffsetY)
// 		SHADER_PARAMETER(uint32, AccumulateEmissive)

// 		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)

// 		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
// 		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
// 		SHADER_PARAMETER(uint32, SceneLightCount)
// 		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)

// 		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
// 		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
// 		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
// 		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)
// 		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
// 		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationRayDistanceUAV)

// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelGridMetaBuf)

// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelLifeBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelPoolBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelRePositionBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, SurfelRePositionCountBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)
// 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint> , SurfelEntryCellBuf)
// 	END_SHADER_PARAMETER_STRUCT()
// };
// IMPLEMENT_GLOBAL_SHADER(FApplySurfelRGS, "/Engine/Private/SurfelGI/ApplySurfelRGS.usf", "GlobalIlluminationRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareRayTracingSurfelGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();
	//const bool bEnableAdaptive = CVarGlobalIlluminationAdaptiveSamplingEnable.GetValueOnRenderThread();
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		//for (uint32 i = 0; i < (uint32)(ELightSamplingType::MAX); i++)
		for(int EnableSurfel = 0; EnableSurfel < 2; ++EnableSurfel)
		{
			FSurfelTraceRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FSurfelTraceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FSurfelTraceRGS::FEnableTransmissionDim>(EnableTransmission);
			//PermutationVector.Set< FGlobalIlluminationRGS::FLightSamplingTypeDim>((ELightSamplingType)i);
			PermutationVector.Set<FSurfelTraceRGS::FUseSurfelDim>(EnableSurfel == 1);
			TShaderMapRef<FSurfelTraceRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
}

void FDeferredShadingSceneRenderer::AllocateSurfels(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	FSurfelBufResources& SurfelRes)
{
	auto Size = View.ViewRect.Size();

	FRDGTextureDesc DebugDesc = FRDGTextureDesc::Create2D(
		Size,
		PF_A32B32G32R32F,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	auto DebugTex = GraphBuilder.CreateTexture(DebugDesc, TEXT("SurfelDebugTex"));


	FRDGBufferRef SurfelMetaBuf = nullptr;
	FRDGBufferRef SurfelGridMetaBuf = nullptr;
	FRDGBufferRef SurfelPoolBuf = nullptr;
	FRDGBufferRef SurfelLifeBuf = nullptr;
	FRDGBufferRef SurfelEntryCellBuf = nullptr;
	FRDGBufferRef SurfelVertexBuf = nullptr;
	FRDGBufferRef SurfelIrradianceBuf = nullptr;
	FRDGBufferRef SurfelRePositionBuf = nullptr;
	FRDGBufferRef SurfelRePositionCountBuf = nullptr;
	FRDGBufferRef SurfelAuxiBuf = nullptr;
	if (View.ViewState->SurfelMetaBuf)
	{
		SurfelMetaBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelMetaBuf, TEXT("SurfelMetaBuf"));
		SurfelGridMetaBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelGridMetaBuf, TEXT("SurfelGridMetaBuf"));
		SurfelPoolBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelPoolBuf, TEXT("SurfelPoolBuf"));
		SurfelLifeBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelLifeBuf, TEXT("SurfelLifeBuf"));
		SurfelEntryCellBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelEntryCellBuf, TEXT("SurfelEntryCellBuf"));
		SurfelVertexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelVertexBuf, TEXT("SurfelVertexBuf"));
		SurfelIrradianceBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIrradianceBuf, TEXT("SurfelIrradianceBuf"));
		SurfelRePositionBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelRePositionBuf, TEXT("SurfelRePositionBuf"));
		SurfelRePositionCountBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelRePositionCountBuf, TEXT("SurfelRePositionCountBuf"));
		SurfelAuxiBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelAuxiBuf, TEXT("SurfelAuxiBuf"));
	}
	else
	{
		SurfelMetaBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * 3), TEXT("SurfelMetaBuf"), ERDGBufferFlags::MultiFrame);
		SurfelGridMetaBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(FVector4) * MAX_SURFEL_CELLS), TEXT("SurfelGridMetaBuf"), ERDGBufferFlags::MultiFrame);
		SurfelPoolBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFELS), TEXT("SurfelPoolBuf"), ERDGBufferFlags::MultiFrame);
		SurfelLifeBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFELS), TEXT("SurfelLifeBuf"), ERDGBufferFlags::MultiFrame);
		SurfelEntryCellBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFELS * MAX_SURFELS_PER_CELL), TEXT("SurfelEntryCellBuf"), ERDGBufferFlags::MultiFrame);
		SurfelVertexBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FSurfelVertexPacked), MAX_SURFELS), TEXT("SurfelVertexBuf"), ERDGBufferFlags::MultiFrame);

		SurfelIrradianceBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4), MAX_SURFELS), TEXT("SurfelIrradianceBuf"), ERDGBufferFlags::MultiFrame);
		SurfelRePositionBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FSurfelVertexPacked), MAX_SURFELS), TEXT("SurfelRePositionBuf"), ERDGBufferFlags::MultiFrame);
		SurfelRePositionCountBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) , MAX_SURFELS), TEXT("SurfelRePositionCountBuf"), ERDGBufferFlags::MultiFrame);
		SurfelAuxiBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4) * 2 , MAX_SURFELS), TEXT("SurfelAuxiBuf"), ERDGBufferFlags::MultiFrame);

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelMetaBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelGridMetaBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelPoolBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelLifeBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelEntryCellBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelRePositionBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelRePositionCountBuf), 0);


		TShaderMapRef<FClearSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FClearSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearSurfelCS::FParameters>();
		
		PassParameters->SurfelPoolBuf = GraphBuilder.CreateUAV(SurfelPoolBuf);
		PassParameters->SurfelLifeBuf = GraphBuilder.CreateUAV(SurfelLifeBuf);

		ClearUnusedGraphResources(ComputeShader, PassParameters);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearSurfel"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(MAX_SURFELS, FClearSurfelCS::GetThreadBlockSize()));
	}

	//temp res
	SurfelRes.SurfelMetaBuf = SurfelMetaBuf;
	SurfelRes.SurfelGridMetaBuf = SurfelGridMetaBuf;
	SurfelRes.SurfelPoolBuf = SurfelPoolBuf;
	SurfelRes.SurfelLifeBuf = SurfelLifeBuf;
	SurfelRes.SurfelEntryCellBuf = SurfelEntryCellBuf;
	SurfelRes.SurfelVertexBuf = SurfelVertexBuf;
	SurfelRes.SurfelIrradianceBuf = SurfelIrradianceBuf;
	SurfelRes.SurfelRePositionBuf = SurfelRePositionBuf;
	SurfelRes.SurfelRePositionCountBuf = SurfelRePositionCountBuf;
	SurfelRes.SurfelAuxiBuf = SurfelAuxiBuf;
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, SurfelAllocate);
		RDG_EVENT_SCOPE(GraphBuilder, "Surfel GI: AllocateSurfel");
		//Allocate Surfel
		{
			TShaderMapRef<FAllocateSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FAllocateSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAllocateSurfelCS::FParameters>();

			PassParameters->SurfelMetaBuf = GraphBuilder.CreateUAV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelGridMetaBuf = GraphBuilder.CreateUAV(SurfelGridMetaBuf, EPixelFormat::PF_R8_UINT);

			PassParameters->SurfelPoolBuf = GraphBuilder.CreateUAV(SurfelPoolBuf);
			PassParameters->SurfelLifeBuf = GraphBuilder.CreateUAV(SurfelLifeBuf);
			PassParameters->SurfelEntryCellBuf = GraphBuilder.CreateUAV(SurfelEntryCellBuf);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateUAV(SurfelVertexBuf);
			PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateUAV(SurfelIrradianceBuf);
			PassParameters->SurfelRePositionBuf = GraphBuilder.CreateUAV(SurfelRePositionBuf);
			PassParameters->SurfelRePositionCountBuf = GraphBuilder.CreateUAV(SurfelRePositionCountBuf);
			
			PassParameters->RWDebugOutTex = GraphBuilder.CreateUAV(DebugTex);

			float ScreenScale = 1.0;
			uint32 ScaledViewSizeX = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().X * ScreenScale));
			uint32 ScaledViewSizeY = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().Y * ScreenScale));
			FIntPoint ScaledViewSize = FIntPoint(ScaledViewSizeX, ScaledViewSizeY);
			FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
			FRDGTextureRef GBufferATexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferA);
			FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);
			PassParameters->NormalTexture = GBufferATexture;
			PassParameters->DepthTexture = SceneDepthTexture;
			PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->ScaledViewSizeAndInvSize = FVector4(ScaledViewSize.X, ScaledViewSize.Y, 1.0f / ScaledViewSize.X, 1.0f / ScaledViewSize.Y);
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FAllocateSurfelCS"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(Size, FAllocateSurfelCS::GetThreadBlockSize()));
		}
		auto DispatchArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(2), TEXT("SurfelIndirectArgs"));
		{
			TShaderMapRef<FDispatchSurfelArgsCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FDispatchSurfelArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDispatchSurfelArgsCS::FParameters>();
			PassParameters->IndirectDispatchArgs = GraphBuilder.CreateUAV(DispatchArgs, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf);

			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("DispatchSurfelArgsCS"),
				ComputeShader,
				PassParameters,
				FIntVector(1, 1, 1));
		}
		
		{
			TShaderMapRef<FAgeSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FAgeSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAgeSurfelCS::FParameters>();
			PassParameters->IndirectDispatchArgs = DispatchArgs;

			PassParameters->SurfelMetaBuf = GraphBuilder.CreateUAV(SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
			PassParameters->SurfelGridMetaBuf = GraphBuilder.CreateUAV(SurfelGridMetaBuf, EPixelFormat::PF_R8_UINT);

			PassParameters->SurfelPoolBuf = GraphBuilder.CreateUAV(SurfelPoolBuf);
			PassParameters->SurfelLifeBuf = GraphBuilder.CreateUAV(SurfelLifeBuf);
			PassParameters->SurfelEntryCellBuf = GraphBuilder.CreateUAV(SurfelEntryCellBuf);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateUAV(SurfelVertexBuf);
			PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateUAV(SurfelIrradianceBuf);
			PassParameters->SurfelRePositionBuf = GraphBuilder.CreateUAV(SurfelRePositionBuf);
			PassParameters->SurfelRePositionCountBuf = GraphBuilder.CreateUAV(SurfelRePositionCountBuf);
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FAgeSurfelCS"),
				ComputeShader,
				PassParameters,
				DispatchArgs,
				0);
		}
	}

}

bool FDeferredShadingSceneRenderer::SurfelTrace(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
	FSurfelBufResources& SurfelRes)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, TraceSurfel);
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel GI: SurfelTrace");

	if (!View.ViewState) return false;

	int32 RayTracingGISamplesPerPixel = GSurfelGISamplesPerPixel;
	if (RayTracingGISamplesPerPixel <= 0) return false;

	// TODO: use raydispatchindirect will be better for perfermance
	{
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

		FSurfelTraceRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSurfelTraceRGS::FParameters>();
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
		PassParameters->RenderTileOffsetX = 0;
		PassParameters->RenderTileOffsetY = 0;

		//surfel

		PassParameters->SurfelMetaBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelMetaBuf, EPixelFormat::PF_R8_UINT);
		PassParameters->SurfelGridMetaBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelGridMetaBuf, EPixelFormat::PF_R8_UINT);

		PassParameters->SurfelPoolBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelPoolBuf);
		PassParameters->SurfelLifeBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelLifeBuf);
		PassParameters->SurfelEntryCellBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelEntryCellBuf);
		PassParameters->SurfelVertexBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelVertexBuf);
		PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelIrradianceBuf);
		PassParameters->SurfelRePositionBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelRePositionBuf);
		PassParameters->SurfelRePositionCountBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelRePositionCountBuf);
		PassParameters->SurfelAuxiBuf =  GraphBuilder.CreateUAV(SurfelRes.SurfelAuxiBuf);

		FSurfelTraceRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FSurfelTraceRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FSurfelTraceRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
		PermutationVector.Set<FSurfelTraceRGS::FUseSurfelDim>(CVarSurfelGIUseSurfel.GetValueOnRenderThread() != 0);
		TShaderMapRef<FSurfelTraceRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		FIntPoint RayTracingResolution = FIntPoint(MAX_SURFEL_CELLS, 1);
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

	return true;
}

bool FDeferredShadingSceneRenderer::SurfelGI(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
	int32 UpscaleFactor,
	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs,
	FSurfelBufResources& SurfelRes)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, SurfelGITotal);
	RDG_EVENT_SCOPE(GraphBuilder, "SurfelGI");

	AllocateSurfels(GraphBuilder, SceneTextures, View, SurfelRes);

	SurfelTrace(GraphBuilder, SceneTextures, View, RayTracingConfig, UpscaleFactor, OutDenoiserInputs, SurfelRes);

	// After we are done, make sure we remember our texture for next time so that we can accumulate samples across frames
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelIrradianceBuf, &View.ViewState->SurfelIrradianceBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelVertexBuf, &View.ViewState->SurfelVertexBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelMetaBuf, &View.ViewState->SurfelMetaBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelGridMetaBuf, &View.ViewState->SurfelGridMetaBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelLifeBuf, &View.ViewState->SurfelLifeBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelPoolBuf, &View.ViewState->SurfelPoolBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelEntryCellBuf, &View.ViewState->SurfelEntryCellBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelRePositionBuf, &View.ViewState->SurfelRePositionBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelRePositionCountBuf, &View.ViewState->SurfelRePositionCountBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelAuxiBuf, &View.ViewState->SurfelAuxiBuf);
	//RenderRestirGI(GraphBuilder, SceneTextures, View, RayTracingConfig, UpscaleFactor, OutDenoiserInputs, &SurfelRes);
	return true;
}

#endif