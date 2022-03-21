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

DECLARE_GPU_STAT_NAMED(SurfelGITotal, TEXT("Surfel GI"))
DECLARE_GPU_STAT_NAMED(SurfelAllocate, TEXT("Surfel GI: AllocateSurfel"));
DECLARE_GPU_STAT_NAMED(SurfelBin, TEXT("Surfel GI: SurfelBin"));
DECLARE_GPU_STAT_NAMED(TraceSurfel, TEXT("Surfel GI: SurfelTrace"));
DECLARE_GPU_STAT_NAMED(ApplySurfel, TEXT("Surfel GI: ApplySurfel"));
DECLARE_GPU_STAT_NAMED(SurfelInclusivePrefixScan, TEXT("Surfel GI: InclusivePrefixScan"));


struct SurfelVertexPacked
{
	FVector4   data0;
	FVector4   data1;
};


class FFindMissSurfelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFindMissSurfelCS)
	SHADER_USE_PARAMETER_STRUCT(FFindMissSurfelCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("FIND_MISS_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelSHBuf)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDebugOutTex)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>, RWTileSurfelAllocTex)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MetallicTexture)

		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FFindMissSurfelCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "FindMissSurfel", SF_Compute);

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
		//SHADER_PARAMETER_STRUCT_INCLUDE(FSurfelGridHashParameters, SurfelHashParameter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelMetaBuf)
		//SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		//SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		//SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TileSurfelAllocTex)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MetallicTexture)

		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(FIntPoint, TileSurfelAllocTexSize)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FAllocateSurfelCS, "/Engine/Private/SurfelGI/AllocateSurfel.usf", "AllocateSurfels", SF_Compute);

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
IMPLEMENT_GLOBAL_SHADER(FDispatchSurfelArgsCS, "/Engine/Private/SurfelGI/AssignSurfelsToGridCells.usf", "PrepareDispatchArgs", SF_Compute);

class FCountSurfelPerCellCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCountSurfelPerCellCS)
	SHADER_USE_PARAMETER_STRUCT(FCountSurfelPerCellCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("COUNT_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 64;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER(ByteAddressBuffer, IndirectDispatchArgs)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCountSurfelPerCellCS, "/Engine/Private/SurfelGI/AssignSurfelsToGridCells.usf", "CountSurfelsPerCell", SF_Compute);

class FSlotSurfelIntoCellCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSlotSurfelIntoCellCS)
	SHADER_USE_PARAMETER_STRUCT(FSlotSurfelIntoCellCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("SLOT_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 64;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(RWByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER(ByteAddressBuffer, IndirectDispatchArgs)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSlotSurfelIntoCellCS, "/Engine/Private/SurfelGI/AssignSurfelsToGridCells.usf", "SlotSurfelIntoCell", SF_Compute);

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
		OutEnvironment.SetDefine(TEXT("CLEAR_SURFEL"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 64;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER(ByteAddressBuffer, IndirectDispatchArgs)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FClearSurfelCS, "/Engine/Private/SurfelGI/AssignSurfelsToGridCells.usf", "ClearSurfels", SF_Compute);

class FPrefixScanCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixScanCS)
	SHADER_USE_PARAMETER_STRUCT(FPrefixScanCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("PREFIX_SCAN"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 512;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, InoutBuf)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FPrefixScanCS, "/Engine/Private/SurfelGI/PrefixSum.usf", "PrefixScan", SF_Compute);

class FPrefixScanSegmentCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixScanSegmentCS)
	SHADER_USE_PARAMETER_STRUCT(FPrefixScanSegmentCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("PREFIX_SCAN_SEGMENT"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 512;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, InputBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, OutputBuf)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FPrefixScanSegmentCS, "/Engine/Private/SurfelGI/PrefixSum.usf", "PrefixScanSegment", SF_Compute);

class FPrefixScanMergeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixScanMergeCS)
	SHADER_USE_PARAMETER_STRUCT(FPrefixScanMergeCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("PREFIX_SCAN_MERGE"), 1);
	}

	static uint32 GetThreadBlockSize()
	{
		return 512;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, InoutBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SegmentSumBuf)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FPrefixScanMergeCS, "/Engine/Private/SurfelGI/PrefixSum.usf", "PrefixScanMerge", SF_Compute);


class FPreDispatchTraceArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPreDispatchTraceArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FPreDispatchTraceArgsCS, FGlobalShader)

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
		OutEnvironment.SetDefine(TEXT("PRE_DISPATCH_TRACE"), 1);
	}


	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, DispatchArgs)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FPreDispatchTraceArgsCS, "/Engine/Private/SurfelGI/PreDispathTrace.usf", "main", SF_Compute);

class FSurfelTraceRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSurfelTraceRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FSurfelTraceRGS, FGlobalShader)

		class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim>;

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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelIrradianceBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<SurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSurfelTraceRGS, "/Engine/Private/SurfelGI/SurfelTraceRGS.usf", "SurfelTraceRGS", SF_RayGen);

class FApplySurfelRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FApplySurfelRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FApplySurfelRGS, FGlobalShader)

		class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableTransmissionDim : SHADER_PERMUTATION_INT("ENABLE_TRANSMISSION", 2);
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableTransmissionDim>;

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
		//OutEnvironment.SetDefine(TEXT("SURFEL_TRACE"), 1);
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
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWGlobalIlluminationRayDistanceUAV)

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelMetaBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashKeyBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelHashValueBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSurfelVertexPacked>, SurfelVertexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CellIndexOffsetBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, SurfelIndexBuf)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SurfelIrradianceBuf)
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FApplySurfelRGS, "/Engine/Private/SurfelGI/ApplySurfelRGS.usf", "GlobalIlluminationRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareRayTracingSurfelGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	int EnableTransmission = CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread();
	//const bool bEnableAdaptive = CVarGlobalIlluminationAdaptiveSamplingEnable.GetValueOnRenderThread();
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	for (int EnableTwoSidedGeometry = 0; EnableTwoSidedGeometry < 2; ++EnableTwoSidedGeometry)
	{
		//for (uint32 i = 0; i < (uint32)(ELightSamplingType::MAX); i++)
		{
			FSurfelTraceRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FSurfelTraceRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FSurfelTraceRGS::FEnableTransmissionDim>(EnableTransmission);
			//PermutationVector.Set< FGlobalIlluminationRGS::FLightSamplingTypeDim>((ELightSamplingType)i);
			//PermutationVector.Set<FGlobalIlluminationRGS::FEnableAdaptiveDim>(bEnableAdaptive);
			TShaderMapRef<FSurfelTraceRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}

		{
			FApplySurfelRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FApplySurfelRGS::FEnableTwoSidedGeometryDim>(EnableTwoSidedGeometry == 1);
			PermutationVector.Set<FApplySurfelRGS::FEnableTransmissionDim>(EnableTransmission);
			//PermutationVector.Set< FGlobalIlluminationRGS::FLightSamplingTypeDim>((ELightSamplingType)i);
			//PermutationVector.Set<FGlobalIlluminationRGS::FEnableAdaptiveDim>(bEnableAdaptive);
			TShaderMapRef<FApplySurfelRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
}

void InclusivePrefixScan(FRDGBuilder& GraphBuilder, FRDGBufferRef& InputBuf)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, SurfelInclusivePrefixScan);
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel GI: InclusivePrefixScan");
	const int32 SEGMENT_SIZE = 1024;

	{
		TShaderMapRef<FPrefixScanCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FPrefixScanCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPrefixScanCS::FParameters>();
		PassParameters->InoutBuf = GraphBuilder.CreateUAV(InputBuf);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("PrefixScanCS"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(SEGMENT_SIZE * SEGMENT_SIZE / 2, 1, 1), FIntVector(FPrefixScanCS::GetThreadBlockSize())));
	}

	auto SegmentBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * SEGMENT_SIZE), TEXT("SegmentBuf"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SegmentBuf), 0);
	{
		TShaderMapRef<FPrefixScanSegmentCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FPrefixScanSegmentCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPrefixScanSegmentCS::FParameters>();
		PassParameters->InputBuf = GraphBuilder.CreateSRV(InputBuf);
		PassParameters->OutputBuf = GraphBuilder.CreateUAV(SegmentBuf);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("PrefixScanSegmentCS"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(SEGMENT_SIZE / 2, 1, 1), FIntVector(FPrefixScanSegmentCS::GetThreadBlockSize())));
	}
	{
		TShaderMapRef<FPrefixScanMergeCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
		FPrefixScanMergeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPrefixScanMergeCS::FParameters>();
		PassParameters->InoutBuf = GraphBuilder.CreateUAV(InputBuf);
		PassParameters->SegmentSumBuf = GraphBuilder.CreateSRV(SegmentBuf);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("PrefixScanMergeCS"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(SEGMENT_SIZE * SEGMENT_SIZE / 2, 1, 1), FIntVector(FPrefixScanMergeCS::GetThreadBlockSize())));
	}
}

void FDeferredShadingSceneRenderer::AllocateSurfels(FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextures,
	FViewInfo& View,
	FSurfelBufResources& SurfelRes)
{
	auto Size = View.ViewRect.Size();
	//auto Size = SceneTextures.SceneDepthTexture->Desc.Extent;
	auto TileSurfelAllocSize = FIntPoint::DivideAndRoundUp(Size, 8);

	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		TileSurfelAllocSize,
		PF_R32G32_UINT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	auto TileSurfelAllocTex = GraphBuilder.CreateTexture(Desc, TEXT("TileSurfelAllocTex"));

	FRDGTextureDesc DebugDesc = FRDGTextureDesc::Create2D(
		Size,
		PF_A32B32G32R32F,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	auto DebugTex = GraphBuilder.CreateTexture(DebugDesc, TEXT("SurfelDebugTex"));


	FRDGBufferRef SurfelMetaBuf = nullptr;
	FRDGBufferRef SurfelHashKeyBuf = nullptr;
	FRDGBufferRef SurfelHashValueBuf = nullptr;
	FRDGBufferRef CellIndexOffsetBuf = nullptr;
	FRDGBufferRef SurfelIndexBuf = nullptr;
	FRDGBufferRef SurfelVertexBuf = nullptr;
	FRDGBufferRef SurfelIrradianceBuf = nullptr;
	FRDGBufferRef SurfelSHBuf = nullptr;

	if (View.ViewState->SurfelMetaBuf)
	{
		SurfelMetaBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelMetaBuf, TEXT("SurfelMetaBuf"));
		SurfelHashKeyBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashKeyBuf, TEXT("SurfelHashKeyBuf"));
		SurfelHashValueBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelHashValueBuf, TEXT("SurfelHashValueBuf"));
		CellIndexOffsetBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->CellIndexOffsetBuf, TEXT("CellIndexOffsetBuf"));
		SurfelIndexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIndexBuf, TEXT("SurfelIndexBuf"));
		SurfelVertexBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelVertexBuf, TEXT("SurfelVertexBuf"));
		SurfelIrradianceBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelIrradianceBuf, TEXT("SurfelIrradianceBuf"));
		//SurfelSHBuf = GraphBuilder.RegisterExternalBuffer(View.ViewState->SurfelSHBuf, TEXT("SurfelSHBuf"));
	}
	else
	{
		//uint32_t SurfelMetaBufData[8] = { 0 }; uint32_t SurfelHashKeyBufData[MAX_SURFEL_CELLS] = { 0 }; uint32_t SurfelCellIndexData[MAX_SURFEL_CELLS+1] = { 0 };
		//SurfelMetaBuf = CreateVertexBuffer(GraphBuilder, TEXT("SurfelMetaBuf"), FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 8), SurfelMetaBufData, sizeof(SurfelMetaBufData));
		//SurfelHashKeyBuf = CreateVertexBuffer(GraphBuilder, TEXT("SurfelHashKeyBuf"), FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MAX_SURFEL_CELLS), SurfelHashKeyBufData, sizeof(SurfelHashKeyBufData));
		//SurfelHashValueBuf = CreateVertexBuffer(GraphBuilder, TEXT("SurfelHashValueBuf"), FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MAX_SURFEL_CELLS), SurfelHashKeyBufData, sizeof(SurfelHashKeyBufData));
		//CellIndexOffsetBuf = CreateVertexBuffer(GraphBuilder, TEXT("CellIndexOffsetBuf"), FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MAX_SURFEL_CELLS + 1), SurfelCellIndexData, sizeof(SurfelCellIndexData));

		SurfelMetaBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * 2), TEXT("SurfelMetaBuf"), ERDGBufferFlags::MultiFrame);
		SurfelHashKeyBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFEL_CELLS), TEXT("SurfelHashKeyBuf"), ERDGBufferFlags::MultiFrame);
		SurfelHashValueBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFEL_CELLS), TEXT("SurfelHashValueBuf"), ERDGBufferFlags::MultiFrame);
		CellIndexOffsetBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * (MAX_SURFEL_CELLS + 1)), TEXT("CellIndexOffsetBuf"), ERDGBufferFlags::MultiFrame);
		SurfelIndexBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(sizeof(uint32) * MAX_SURFEL_CELLS * MAX_SURFELS_PER_CELL), TEXT("SurfelIndexBuf"), ERDGBufferFlags::MultiFrame);
		SurfelVertexBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(SurfelVertexPacked), MAX_SURFELS), TEXT("SurfelVertexBuf"), ERDGBufferFlags::MultiFrame);

		SurfelIrradianceBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4), MAX_SURFELS), TEXT("SurfelIrradianceBuf"), ERDGBufferFlags::MultiFrame);
		SurfelSHBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4) * 3, MAX_SURFELS), TEXT("SurfelSHBuf"), ERDGBufferFlags::MultiFrame);

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelMetaBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelHashKeyBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelHashValueBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CellIndexOffsetBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelIndexBuf), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelIrradianceBuf), 0);
	}

	uint32 ClearValues[4] = { 0, 0, 0, 0 };
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TileSurfelAllocTex)), ClearValues);


	//temp res
	SurfelRes.SurfelMetaBuf = SurfelMetaBuf;
	SurfelRes.SurfelHashKeyBuf = SurfelHashKeyBuf;
	SurfelRes.SurfelHashValueBuf = SurfelHashValueBuf;
	SurfelRes.CellIndexOffsetBuf = CellIndexOffsetBuf;
	SurfelRes.SurfelIndexBuf = SurfelIndexBuf;
	SurfelRes.SurfelVertexBuf = SurfelVertexBuf;
	SurfelRes.SurfelIrradianceBuf = SurfelIrradianceBuf;
	SurfelRes.SurfelSHBuf = SurfelSHBuf;
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, SurfelAllocate);
		RDG_EVENT_SCOPE(GraphBuilder, "Surfel GI: AllocateSurfel");
		{
			TShaderMapRef<FFindMissSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FFindMissSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFindMissSurfelCS::FParameters>();
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateUAV(SurfelMetaBuf);
			PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateUAV(SurfelHashKeyBuf);
			PassParameters->SurfelHashValueBuf = GraphBuilder.CreateUAV(SurfelHashValueBuf);
			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateUAV(CellIndexOffsetBuf);
			PassParameters->SurfelIndexBuf = GraphBuilder.CreateUAV(SurfelIndexBuf);

			PassParameters->SurfelVertexBuf = GraphBuilder.CreateUAV(SurfelVertexBuf);
			PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateUAV(SurfelIrradianceBuf);
			PassParameters->RWDebugOutTex = GraphBuilder.CreateUAV(DebugTex);
			PassParameters->RWTileSurfelAllocTex = GraphBuilder.CreateUAV(TileSurfelAllocTex);

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
				RDG_EVENT_NAME("FFindMissSurfelCS"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(Size, FFindMissSurfelCS::GetThreadBlockSize()));

		}
		//Allocate Surfel
		{
			TShaderMapRef<FAllocateSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FAllocateSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAllocateSurfelCS::FParameters>();
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateUAV(SurfelMetaBuf);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateUAV(SurfelVertexBuf);
			//PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf);
			//PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf);
			//PassParameters->SurfelIndexBuf = GraphBuilder.CreateSRV(SurfelIndexBuf);

			PassParameters->TileSurfelAllocTex = TileSurfelAllocTex;

			float ScreenScale = 1.0;
			uint32 ScaledViewSizeX = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().X * ScreenScale));
			uint32 ScaledViewSizeY = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().Y * ScreenScale));
			FIntPoint ScaledViewSize = FIntPoint(ScaledViewSizeX, ScaledViewSizeY);
			FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
			FRDGTextureRef GBufferATexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferA);
			FRDGTextureRef GBufferBTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferB);
			FRDGTextureRef GBufferCTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferC);
			FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);
			PassParameters->NormalTexture = GBufferATexture;
			PassParameters->DepthTexture = SceneDepthTexture;
			PassParameters->BaseColorTexture = GBufferCTexture;
			PassParameters->MetallicTexture = GBufferBTexture;
			PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->TileSurfelAllocTexSize = TileSurfelAllocSize;
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FAllocateSurfelCS"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TileSurfelAllocSize, FAllocateSurfelCS::GetThreadBlockSize()));
		}
	}
	//Dispatch
	auto DispatchArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(2), TEXT("SurfelIndirectArgs"));

	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, SurfelBin);
		RDG_EVENT_SCOPE(GraphBuilder, "Surfel GI: SurfelBin");
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
		//Clear Surfels
		{
			TShaderMapRef<FClearSurfelCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FClearSurfelCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearSurfelCS::FParameters>();
			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateUAV(CellIndexOffsetBuf);
			PassParameters->IndirectDispatchArgs = DispatchArgs;
			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ClearSurfelCS"),
				ComputeShader,
				PassParameters,
				DispatchArgs,
				0);
		}
		//Count
		{
			TShaderMapRef<FCountSurfelPerCellCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FCountSurfelPerCellCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCountSurfelPerCellCS::FParameters>();
			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateUAV(CellIndexOffsetBuf);
			PassParameters->SurfelIndexBuf = GraphBuilder.CreateUAV(SurfelIndexBuf);

			PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf);
			PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf);
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			PassParameters->IndirectDispatchArgs = DispatchArgs;
			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CountSurfelPerCellCS"),
				ComputeShader,
				PassParameters,
				DispatchArgs,
				12);
		}
		InclusivePrefixScan(GraphBuilder, CellIndexOffsetBuf);

		//slot
		{
			TShaderMapRef<FSlotSurfelIntoCellCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FSlotSurfelIntoCellCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSlotSurfelIntoCellCS::FParameters>();

			PassParameters->CellIndexOffsetBuf = GraphBuilder.CreateUAV(CellIndexOffsetBuf);
			PassParameters->SurfelIndexBuf = GraphBuilder.CreateUAV(SurfelIndexBuf);

			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			PassParameters->SurfelHashKeyBuf = GraphBuilder.CreateSRV(SurfelHashKeyBuf);
			PassParameters->SurfelHashValueBuf = GraphBuilder.CreateSRV(SurfelHashValueBuf);
			PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelMetaBuf);
			PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelVertexBuf);
			PassParameters->IndirectDispatchArgs = DispatchArgs;
			ClearUnusedGraphResources(ComputeShader, PassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SlotSurfelIntoCellCS"),
				ComputeShader,
				PassParameters,
				DispatchArgs,
				12);
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
		PassParameters->SurfelIrradianceBuf = GraphBuilder.CreateUAV(SurfelRes.SurfelIrradianceBuf);
		PassParameters->SurfelVertexBuf = GraphBuilder.CreateSRV(SurfelRes.SurfelVertexBuf);
		PassParameters->SurfelMetaBuf = GraphBuilder.CreateSRV(SurfelRes.SurfelMetaBuf);
		FSurfelTraceRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FSurfelTraceRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingGlobalIlluminationEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FSurfelTraceRGS::FEnableTransmissionDim>(CVarRayTracingGlobalIlluminationEnableTransmission.GetValueOnRenderThread());
		//PermutationVector.Set<FSurfelTraceRGS::FLightSamplingTypeDim>((ELightSamplingType)CVarLightSamplingType.GetValueOnRenderThread());
		TShaderMapRef<FSurfelTraceRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		FIntPoint RayTracingResolution = FIntPoint(MAX_SURFEL_CELLS * MAX_SURFELS_PER_CELL, 1);
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
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelHashKeyBuf, &View.ViewState->SurfelHashKeyBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelHashValueBuf, &View.ViewState->SurfelHashValueBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.CellIndexOffsetBuf, &View.ViewState->CellIndexOffsetBuf);
	GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelIndexBuf, &View.ViewState->SurfelIndexBuf);
	//GraphBuilder.QueueBufferExtraction(SurfelRes.SurfelSHBuf, &View.ViewState->SurfelSHBuf);

	//RenderRestirGI(GraphBuilder, SceneTextures, View, RayTracingConfig, UpscaleFactor, OutDenoiserInputs, &SurfelRes);
	return true;
}

#endif