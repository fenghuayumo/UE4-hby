

#include "RayTracingVisualizeBVH.h"
#include "RHIDefinitions.h"
#include "RenderCore.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "SceneRenderTargets.h"
#include "RenderGraphUtils.h"
#include "DeferredShadingRenderer.h"
#include "PipelineStateCache.h"
#include "SceneTextureParameters.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"

#if RHI_RAYTRACING

static TAutoConsoleVariable<int32> CVarRayTracingVisualizeBVHColorMap(
	TEXT("r.RayTracing.VisualizeBVH.ColorMap"),
	0,
	TEXT("Color map to use for visualization\n")
	TEXT("  0 - Simple color ramp (default)\n")
	TEXT("  1 - Jet-like encoding\n")
	TEXT("  2 - Turbo-like encoding\n")
	TEXT("  3 - Viridis-like\n")
	TEXT("  4 - Plasma-like\n")
	TEXT("  5 - Magma-like\n")
	TEXT("  6 - Inferno-like\n")
	TEXT("  7 - Grayscale"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingVisualizeBVHEncoding(
	TEXT("r.RayTracing.VisualizeBVH.Encoding"),
	2,
	TEXT("How to encode the overlap count\n")
	TEXT("  0 - linear\n")
	TEXT("  1 - log of linear\n")
	TEXT("  2 - logarithmic (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracingVisualizeBVHRange(
	TEXT("r.RayTracing.VisualizeBVH.Range"),
	32.0f,
	TEXT("Upper bound on number of volumes accumulated"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracingVisualizeBVHRangeMin(
	TEXT("r.RayTracing.VisualizeBVH.RangeMin"),
	0.0f,
	TEXT("Lower bound on number of volumes accumulated"),
	ECVF_RenderThreadSafe);

class FVisualizeBVHHitGroup : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeBVHHitGroup)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FVisualizeBVHHitGroup, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	using FParameters = FEmptyShaderParameters;
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeBVHHitGroup, "/Engine/Private/RayTracing/RayTracingVisualizeBVH.usf", "closesthit=VisualizeBVHCHS anyhit=VisualizeBVHAHS intersection=VisualizeBVHIS", SF_RayHitGroup);

class FVisualizeBVHRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeBVHRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FVisualizeBVHRGS, FGlobalShader)


	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_TEXTURE(Texture2D, MiniFontTexture)

		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER(int32, ColorMap)
		SHADER_PARAMETER(uint32, Encoding)
		SHADER_PARAMETER(float, Range)
		SHADER_PARAMETER(float, RangeMin)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorOutput)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeBVHRGS, "/Engine/Private/RayTracing/RayTracingVisualizeBVH.usf", "VisualizeBVHRGS", SF_RayGen);


// empty miss shader, because a miss shader is required
class FVisualizeBVHMS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeBVHMS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FVisualizeBVHMS, FGlobalShader)


	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	using FParameters = FEmptyShaderParameters;
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeBVHMS, "/Engine/Private/RayTracing/RayTracingVisualizeBVH.usf", "VisualizeBVHMS", SF_RayMiss);


void FUnitCubeAABB::InitRHI()
{
#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		FRHIResourceCreateInfo CreateInfo;
		
		static const FVector Vertices[2] =
		{
			FVector(0.0f, 0.0f, 0.0f),
			FVector(1.0f, 1.0f, 1.0f)
		};

		CreateInfo.DebugName = TEXT("UnitCubeAABB");
		VertexData = RHICreateVertexBuffer(sizeof(float) * 6, BUF_Static, CreateInfo);
		void* VoidPtr = RHILockVertexBuffer(VertexData, 0, sizeof(FVector) * 2, RLM_WriteOnly);
		FMemory::Memcpy(VoidPtr, Vertices, sizeof(FVector) * 2);
		RHIUnlockVertexBuffer(VertexData);
		
		FRayTracingGeometryInitializer Initializer;
		
		Initializer.IndexBuffer = nullptr;
		Initializer.TotalPrimitiveCount = 1; // one AABB
		Initializer.GeometryType = RTGT_Procedural;
		Initializer.bFastBuild = false;

		// Need explicit segment to mark no duplicate AnyHit
		FRayTracingGeometrySegment Segment;
		Segment.bForceOpaque = false;
		Segment.bAllowDuplicateAnyHitShaderInvocation = false;
		Segment.FirstPrimitive = 0;
		Segment.NumPrimitives = 1;
		Segment.VertexBuffer = VertexData;
		Segment.VertexBufferElementType = VET_Float3;
		Segment.VertexBufferStride = sizeof(FVector) * 2;
		Segment.VertexBufferOffset = 0;

		Initializer.Segments.Add(Segment);

		RayTracingGeometry.SetInitializer(Initializer);
		RayTracingGeometry.InitResource();
		
	}
#endif // RHI_RAYTRACING
}

void FUnitCubeAABB::ReleaseRHI()
{
#if RHI_RAYTRACING
	BeginReleaseResource(&RayTracingGeometry);
#endif
}

static FRayTracingPipelineState* BindRayTracingVisualizeBVHPipeline(FRHICommandList& RHICmdList, const FViewInfo& ReferenceView, const FViewInfo& View, FRHIRayTracingShader* RayGenShader)
{
	SCOPE_CYCLE_COUNTER(STAT_BindRayTracingPipeline);

	FRayTracingPipelineStateInitializer Initializer;

	FRHIRayTracingShader* RayGenShaderTable[] = { RayGenShader };
	Initializer.SetRayGenShaderTable(RayGenShaderTable);

	Initializer.MaxPayloadSizeInBytes = 12; // could be reduced
	Initializer.bAllowHitGroupIndexing = false;  // just one intersection type

	// Get the ray tracing materials
	auto ClosestHitShader = View.ShaderMap->GetShader<FVisualizeBVHHitGroup>();
	FRHIRayTracingShader* HitShaderTable[] = { ClosestHitShader.GetRayTracingShader() };
	Initializer.SetHitGroupTable(HitShaderTable);

	auto MissShader = View.ShaderMap->GetShader<FVisualizeBVHMS>();
	FRHIRayTracingShader* MissShaderTable[] = { MissShader.GetRayTracingShader() };
	Initializer.SetMissShaderTable(MissShaderTable);

	FRayTracingPipelineState* PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, Initializer);

	return PipelineState;
}

void FDeferredShadingSceneRenderer::RenderRayTracingVisualizeBVH(
	FRDGBuilder& GraphBuilder,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	FRDGTextureRef OutColorTexture
)
{
	for (const FViewInfo& View: Views)
	{
		SCOPED_DRAW_EVENT(GraphBuilder.RHICmdList, RayTracingVisualizeBVH);

		RenderRayTracingVisualizeBVHView(
			GraphBuilder,
			View,
			SceneTexturesUniformBuffer,
			OutColorTexture
		);
	}
}

void FDeferredShadingSceneRenderer::RenderRayTracingVisualizeBVHView(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer,
	FRDGTextureRef OutColorTexture
)
{
	FIntPoint RayTracingResolution = View.ViewRect.Size();

	FVisualizeBVHRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeBVHRGS::FParameters>();

	PassParameters->TLAS = View.RayTracingSceneBVH.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	PassParameters->Mode = uint32(View.Family->EngineShowFlags.VisualizeBVHComplexity);
	PassParameters->ColorMap = CVarRayTracingVisualizeBVHColorMap.GetValueOnRenderThread();
	PassParameters->Encoding = FMath::Clamp<uint32>(CVarRayTracingVisualizeBVHEncoding.GetValueOnRenderThread(), 0, 2);
	PassParameters->Range = CVarRayTracingVisualizeBVHRange.GetValueOnRenderThread();
	PassParameters->RangeMin = CVarRayTracingVisualizeBVHRangeMin.GetValueOnRenderThread();

	PassParameters->MiniFontTexture = GEngine->MiniFontTexture ? GEngine->MiniFontTexture->Resource->TextureRHI : GSystemTextures.WhiteDummy->GetRenderTargetItem().TargetableTexture;

	PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder, SceneTexturesUniformBuffer);
	PassParameters->ColorOutput = GraphBuilder.CreateUAV(OutColorTexture);

	const FViewInfo& ReferenceView = Views[0];

	auto RayGenShader = View.ShaderMap->GetShader<FVisualizeBVHRGS>();
	ClearUnusedGraphResources(RayGenShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("BVHRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, /*this,*/ &View, &ReferenceView, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
	{
		FRayTracingPipelineState* Pipeline = BindRayTracingVisualizeBVHPipeline(RHICmdList, ReferenceView, View, RayGenShader.GetRayTracingShader());

		FRayTracingShaderBindingsWriter GlobalResources;
		SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

		FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
		RHICmdList.RayTraceDispatch(Pipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
	});
}



#else // RHI_RAYTRACING


#endif // RHI_RAYTRACING
