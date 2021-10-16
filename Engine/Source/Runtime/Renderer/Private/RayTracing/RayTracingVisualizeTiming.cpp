


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
#include "RayTracingDefinitions.h"

#if RHI_RAYTRACING


static TAutoConsoleVariable<int32> CVarRayTracingVisualizeTimingColorMap(
	TEXT("r.RayTracing.VisualizeTiming.ColorMap"),
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

static TAutoConsoleVariable<int32> CVarRayTracingVisualizeTimingEncoding(
	TEXT("r.RayTracing.VisualizeTiming.Encoding"),
	0,
	TEXT("How to encode the timing visualization scale\n")
	TEXT("  0 - linear (default)\n")
	TEXT("  1 - logarithmic\n")
	TEXT("  2 - exponential"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracingVisualizeTimingRange(
	TEXT("r.RayTracing.VisualizeTiming.Range"),
	100000.0f,
	TEXT("Maximum timing value"),
	ECVF_RenderThreadSafe);


bool VisualizeRayTracingTiming(const FViewInfo& View)
{
	// requires NVAPI timing extensions
	return RAY_TRACING_SUPPORT_TIMING && GRHISupportsRayTracingShaderExtensions && View.Family->EngineShowFlags.VisualizeRayTiming;
}

class FVisualizeTimingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeTimingCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeTimingCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static int32 GetGroupSize()
	{
		return 16;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, RayTiming)

		SHADER_PARAMETER_TEXTURE(Texture2D, MiniFontTexture)

		SHADER_PARAMETER(int32, ColorMap)
		SHADER_PARAMETER(uint32, Encoding)
		SHADER_PARAMETER(float, Range)
		SHADER_PARAMETER(float, RangeMin)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorOutput)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeTimingCS, "/Engine/Private/RayTracing/VisualizeTiming.usf", "VisualizeTiming", SF_Compute);

void RenderRayTracingVisualizeTimingView(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& OutColorTexture
)
{
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	FIntPoint Resolution = View.ViewRect.Size();

	FVisualizeTimingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeTimingCS::FParameters>();

	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	PassParameters->ColorMap = CVarRayTracingVisualizeTimingColorMap.GetValueOnRenderThread();
	PassParameters->Encoding = FMath::Clamp<uint32>(CVarRayTracingVisualizeTimingEncoding.GetValueOnRenderThread(), 0, 2);
	PassParameters->Range = CVarRayTracingVisualizeTimingRange.GetValueOnRenderThread();
	PassParameters->RangeMin = 0.0f; 

	PassParameters->MiniFontTexture = GEngine->MiniFontTexture ? GEngine->MiniFontTexture->Resource->TextureRHI : GSystemTextures.WhiteDummy->GetRenderTargetItem().TargetableTexture;
	PassParameters->RayTiming = GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming, TEXT("RayTracingTiming"));

	PassParameters->ColorOutput = GraphBuilder.CreateUAV(OutColorTexture);

	auto VisualizeShader = View.ShaderMap->GetShader<FVisualizeTimingCS>();
	ClearUnusedGraphResources(VisualizeShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Visualize Ray Tracing Timing"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, &View, VisualizeShader, Resolution](FRHICommandList& RHICmdList)
	{
		FIntVector GroupCount;
		GroupCount.X = FMath::DivideAndRoundUp(Resolution.X, FVisualizeTimingCS::GetGroupSize());
		GroupCount.Y = FMath::DivideAndRoundUp(Resolution.Y, FVisualizeTimingCS::GetGroupSize());
		GroupCount.Z = 1;
		FComputeShaderUtils::Dispatch(RHICmdList, VisualizeShader, *PassParameters, GroupCount);

	});
}

void FDeferredShadingSceneRenderer::RenderVisualizeRayTracingTiming(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneColorOutputTexture)
{
	for (int32 ViewIndex = 0, Num = Views.Num(); ViewIndex < Num; ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		IScreenSpaceDenoiser::FReflectionsInputs DenoiserInputs;

		RenderRayTracingVisualizeTimingView(
			GraphBuilder,
			View, SceneColorOutputTexture);

	}
}

#endif // RHI_RAYTRACING
