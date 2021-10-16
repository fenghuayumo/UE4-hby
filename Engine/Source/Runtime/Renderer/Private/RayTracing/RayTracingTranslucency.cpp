// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"

static TAutoConsoleVariable<int32> CVarRayTracingHybridTranslucencySupport(
	TEXT("r.RayTracing.HybridTranslucencySupport"),
	0,
	TEXT("Configure shader support for hybrid translucency"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

#if RHI_RAYTRACING

#include "ClearQuad.h"
#include "SceneRendering.h"
#include "SceneRenderTargets.h"
#include "RHIResources.h"
#include "SystemTextures.h"
#include "ScreenSpaceDenoise.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "RayTracing/RaytracingOptions.h"
#include "Raytracing/RaytracingLighting.h"


static TAutoConsoleVariable<int32> CVarRayTracingTranslucency(
	TEXT("r.RayTracing.Translucency"),
	-1,
	TEXT("-1: Value driven by postprocess volume (default) \n")
	TEXT(" 0: ray tracing translucency off (use raster) \n")
	TEXT(" 1: ray tracing translucency enabled"),
	ECVF_RenderThreadSafe);

static float GRayTracingTranslucencyMaxRoughness = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRoughness(
	TEXT("r.RayTracing.Translucency.MaxRoughness"),
	GRayTracingTranslucencyMaxRoughness,
	TEXT("Sets the maximum roughness until which ray tracing reflections will be visible (default = -1 (max roughness driven by postprocessing volume))")
);

static int32 GRayTracingTranslucencyMaxRefractionRays = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRefractionRays(
	TEXT("r.RayTracing.Translucency.MaxRefractionRays"),
	GRayTracingTranslucencyMaxRefractionRays,
	TEXT("Sets the maximum number of refraction rays for ray traced translucency (default = -1 (max bounces driven by postprocessing volume)"));

static int32 GRayTracingTranslucencyEmissiveAndIndirectLighting = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyEmissiveAndIndirectLighting(
	TEXT("r.RayTracing.Translucency.EmissiveAndIndirectLighting"),
	GRayTracingTranslucencyEmissiveAndIndirectLighting,
	TEXT("Enables ray tracing translucency emissive and indirect lighting (default = 1)")
);

static int32 GRayTracingTranslucencyDirectLighting = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyDirectLighting(
	TEXT("r.RayTracing.Translucency.DirectLighting"),
	GRayTracingTranslucencyDirectLighting,
	TEXT("Enables ray tracing translucency direct lighting (default = 1)")
);

static int32 GRayTracingTranslucencyShadows = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyShadows(
	TEXT("r.RayTracing.Translucency.Shadows"),
	GRayTracingTranslucencyShadows,
	TEXT("Enables shadows in ray tracing translucency)")
	TEXT(" -1: Shadows driven by postprocessing volume (default)")
	TEXT(" 0: Shadows disabled ")
	TEXT(" 1: Hard shadows")
	TEXT(" 2: Soft area shadows")
);

static float GRayTracingTranslucencyMinRayDistance = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMinRayDistance(
	TEXT("r.RayTracing.Translucency.MinRayDistance"),
	GRayTracingTranslucencyMinRayDistance,
	TEXT("Sets the minimum ray distance for ray traced translucency rays. Actual translucency ray length is computed as Lerp(MaxRayDistance, MinRayDistance, Roughness), i.e. translucency rays become shorter when traced from rougher surfaces. (default = -1 (infinite rays))")
);

static TAutoConsoleVariable<float> CVarRayTracingTranslucencyRoughnessMultiplier(
	TEXT("r.RayTracing.Translucency.RoughnessMultiplier"),
	1.0f,
	TEXT("Multiplies reflected RT roughness, can be used to reduce the sampling cone (min=0, max=1, default=1)"),
	ECVF_RenderThreadSafe);

static float GRayTracingTranslucencyMaxRayDistance = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRayDistance(
	TEXT("r.RayTracing.Translucency.MaxRayDistance"),
	GRayTracingTranslucencyMaxRayDistance,
	TEXT("Sets the maximum ray distance for ray traced translucency rays. When ray shortening is used, skybox will not be sampled in RT translucency pass and will be composited later, together with local reflection captures. Negative values turn off this optimization. (default = -1 (infinite rays))")
);

static int32 GRayTracingTranslucencySamplesPerPixel = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencySamplesPerPixel(
	TEXT("r.RayTracing.Translucency.SamplesPerPixel"),
	GRayTracingTranslucencySamplesPerPixel,
	TEXT("Sets the samples-per-pixel for Translucency (default = 1)"));

static int32 GRayTracingTranslucencyHeightFog = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyHeightFog(
	TEXT("r.RayTracing.Translucency.HeightFog"),
	GRayTracingTranslucencyHeightFog,
	TEXT("Enables height fog in ray traced Translucency (default = 1)"));

static int32 GRayTracingTranslucencyRefraction = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyRefraction(
	TEXT("r.RayTracing.Translucency.Refraction"),
	GRayTracingTranslucencyRefraction,
	TEXT("Enables refraction in ray traced Translucency (default = 1)"));

static float GRayTracingTranslucencyPrimaryRayBias = 1e-5;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyPrimaryRayBias(
	TEXT("r.RayTracing.Translucency.PrimaryRayBias"),
	GRayTracingTranslucencyPrimaryRayBias,
	TEXT("Sets the bias to be subtracted from the primary ray TMax in ray traced Translucency. Larger bias reduces the chance of opaque objects being intersected in ray traversal, saving performance, but at the risk of skipping some thin translucent objects in proximity of opaque objects. (recommended range: 0.00001 - 0.1) (default = 0.00001)"));

static TAutoConsoleVariable<int32> CVarRayTracingHybridTranslucency(
	TEXT("r.RayTracing.HybridTranslucency"),
	0,
	TEXT(" 0: use raytraced translucency \n")
	TEXT(" 1: use hybrid raytraced translucency"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingHybridTranslucencyLayers(
	TEXT("r.RayTracing.HybridTranslucency.Layers"),
	1,
	TEXT("Number of layers of hybrid translucency"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracingHybridTranslucencyDepthThreshold(
	TEXT("r.RayTracing.HybridTranslucency.DepthThreshold"),
	0.0005f,
	TEXT("Separation ratio at which translucency samples are considered distinct\n")
	TEXT(" Default value = 0.0005 (0.05% or 5 cm on a surface 100m away)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingHybridTranslucencyHalfRes(
	TEXT("r.RayTracing.HybridTranslucency.HalfRes"),
	0,
	TEXT("Whether to render hybrid translucency at half resolution (default = 0)\n")
	TEXT(" 0 - full resolution\n")
	TEXT(" 1 - half resolution interleaved (2-tap vertical filter)\n")
	TEXT(" 2 - half resolution checkerboard (4-tap filter)\n")
	TEXT(" 3 - half resolution checkerboard (2-tap vertical filter"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingTranslucencyTiming(
	TEXT("r.RayTracing.Translucency.Timing"),
	1,
	TEXT("Time cost of ray traced translucency"),
	ECVF_RenderThreadSafe);

DECLARE_GPU_STAT_NAMED(RayTracingTranslucency, TEXT("Ray Tracing Translucency"));

#if RHI_RAYTRACING

FRayTracingPrimaryRaysOptions GetRayTracingTranslucencyOptions(const FViewInfo& View)
{
	FRayTracingPrimaryRaysOptions Options;

	Options.bEnabled = ShouldRenderRayTracingEffect(CVarRayTracingTranslucency.GetValueOnRenderThread() != 0);
	Options.SamplerPerPixel = GRayTracingTranslucencySamplesPerPixel >= 0 ? GRayTracingTranslucencySamplesPerPixel : View.FinalPostProcessSettings.RayTracingTranslucencySamplesPerPixel;
	Options.ApplyHeightFog = GRayTracingTranslucencyHeightFog;
	Options.PrimaryRayBias = GRayTracingTranslucencyPrimaryRayBias;
	Options.MaxRoughness = GRayTracingTranslucencyMaxRoughness >= 0 ? GRayTracingTranslucencyMaxRoughness : View.FinalPostProcessSettings.RayTracingTranslucencyMaxRoughness;
	Options.MaxRefractionRays = GRayTracingTranslucencyMaxRefractionRays >= 0 ? GRayTracingTranslucencyMaxRefractionRays : View.FinalPostProcessSettings.RayTracingTranslucencyRefractionRays;
	Options.EnableEmmissiveAndIndirectLighting = GRayTracingTranslucencyEmissiveAndIndirectLighting;
	Options.EnableDirectLighting = GRayTracingTranslucencyDirectLighting;
	Options.EnableShadows = GRayTracingTranslucencyShadows >= 0 ? GRayTracingTranslucencyShadows : (int) View.FinalPostProcessSettings.RayTracingTranslucencyShadows;
	Options.MinRayDistance = GRayTracingTranslucencyMinRayDistance;
	Options.MaxRayDistance = GRayTracingTranslucencyMaxRayDistance;
	Options.EnableRefraction = GRayTracingTranslucencyRefraction >= 0 ? GRayTracingTranslucencyRefraction : View.FinalPostProcessSettings.RayTracingTranslucencyRefraction;;
	Options.RoughnessMultiplier = CVarRayTracingTranslucencyRoughnessMultiplier.GetValueOnRenderThread();

	return Options;
}

bool ShouldRenderRayTracingTranslucency(const FViewInfo& View)
{
	const bool bViewWithRaytracingTranslucency = View.FinalPostProcessSettings.TranslucencyType == ETranslucencyType::RayTracing;

	const int32 RayTracingTranslucencyMode = CVarRayTracingTranslucency.GetValueOnRenderThread();
	
	const bool bTranslucencyEnabled = RayTracingTranslucencyMode < 0
		? bViewWithRaytracingTranslucency
		: RayTracingTranslucencyMode != 0;

	return ShouldRenderRayTracingEffect(bTranslucencyEnabled);
}

bool ShouldRenderRayTracingHybridTranslucency()
{
	return CVarRayTracingHybridTranslucencySupport.GetValueOnRenderThread() != 0 && CVarRayTracingHybridTranslucency.GetValueOnRenderThread() != 0;
}

int32 MaxHybridTranslucencyLayers()
{
	return FMath::Clamp( CVarRayTracingHybridTranslucencyLayers.GetValueOnRenderThread(), 1, 8);
}

#endif // RHI_RAYTRACING

void FDeferredShadingSceneRenderer::RenderRayTracingTranslucency(FRDGBuilder& GraphBuilder, FRDGTextureMSAA SceneColorTexture)
{
	const bool bHybridTransparency = ShouldRenderRayTracingHybridTranslucency();

	//#dxr_todo: check DOF support, do we need to call RenderRayTracingTranslucency twice?
	if (!ShouldRenderTranslucency(ETranslucencyPass::TPT_StandardTranslucency)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_TranslucencyAfterDOF)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_TranslucencyAfterDOFModulate)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_AllTranslucency)
		)
	{
		return; // Early exit if nothing needs to be done.
	}

	AddResolveSceneColorPass(GraphBuilder, Views, SceneColorTexture);
	
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	ERayTracingPrimaryRaysFlag Flags = ERayTracingPrimaryRaysFlag::None;

	if (bHybridTransparency)
	{
		// clear layered targets to -1 / green to enable easier identification of errant data
		GraphBuilder.RHICmdList.ClearUAVFloat(SceneContext.TranslucencyLayersDepth->GetRenderTargetItem().UAV, FVector4(-1.0f, -1.0f, -1.0f, -1.0f));
		GraphBuilder.RHICmdList.ClearUAVFloat( SceneContext.TranslucencyLayersColor->GetRenderTargetItem().UAV, FVector4(0.0f, 1.0f, 0.0f, 0.25f));
		Flags = ERayTracingPrimaryRaysFlag::HybridTranslucency;
		
		int32 HalfResMode = CVarRayTracingHybridTranslucencyHalfRes.GetValueOnRenderThread();
		if (HalfResMode != 0)
		{
			Flags |= ERayTracingPrimaryRaysFlag::HalfResolution;
			if (HalfResMode == 2)
			{
				Flags |= ERayTracingPrimaryRaysFlag::CheckerboardSampling;
			}
		}
	}

	if (VisualizeRayTracingTiming(Views[0]) && CVarRayTracingTranslucencyTiming.GetValueOnRenderThread())
	{
		Flags |= ERayTracingPrimaryRaysFlag::TimeTracing;
	}

	static const auto TranslucencyMaskCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RayTracing.Translucency.Mask"));

	if (TranslucencyMaskCVar && TranslucencyMaskCVar->GetValueOnRenderThread())
	{
		Flags |= ERayTracingPrimaryRaysFlag::StencilMask;
	}

	
	FRDGTextureRef ColorLayers = GraphBuilder.RegisterExternalTexture(SceneContext.TranslucencyLayersColor);
	FRDGTextureRef DepthLayers = GraphBuilder.RegisterExternalTexture(SceneContext.TranslucencyLayersDepth);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "RayTracingTranslucency");
		RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingTranslucency)

		for (int32 ViewIndex = 0, Num = Views.Num(); ViewIndex < Num; ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];

			if (!View.ShouldRenderView() || !ShouldRenderRayTracingTranslucency(View))
			{
				continue;
			}

			const FScreenPassRenderTarget Output(SceneColorTexture.Target, View.ViewRect, ERenderTargetLoadAction::ELoad);

			//#dxr_todo: UE-72581 do not use reflections denoiser structs but separated ones
			IScreenSpaceDenoiser::FReflectionsInputs DenoiserInputs;
			float ResolutionFraction = 1.0f;
			int32 TranslucencySPP = GRayTracingTranslucencySamplesPerPixel > -1 ? GRayTracingTranslucencySamplesPerPixel : View.FinalPostProcessSettings.RayTracingTranslucencySamplesPerPixel;
		
			RenderRayTracingPrimaryRaysView(
				GraphBuilder,
				View, &DenoiserInputs.Color, &DenoiserInputs.RayHitDistance,
				TranslucencySPP, GRayTracingTranslucencyHeightFog, ResolutionFraction, 
				&ColorLayers, &DepthLayers,
				ERayTracingPrimaryRaysFlag::AllowSkipSkySample | ERayTracingPrimaryRaysFlag::UseGBufferForMaxDistance | ERayTracingPrimaryRaysFlag::TranslucentTopLayer | Flags);

			const FScreenPassTexture SceneColor(DenoiserInputs.Color, View.ViewRect);
			// Only composite full transparency
			if (!bHybridTransparency)
			{
				AddDrawTexturePass(GraphBuilder, View, SceneColor, Output);
			}
		}
		
		// EHartNV : TODO are these per view?
		if (bHybridTransparency)
		{
			GraphBuilder.QueueTextureExtraction(ColorLayers, &SceneContext.TranslucencyLayersColor);
			GraphBuilder.QueueTextureExtraction(DepthLayers, &SceneContext.TranslucencyLayersDepth);
		}
	}

	AddResolveSceneColorPass(GraphBuilder, Views, SceneColorTexture);
}

#endif // RHI_RAYTRACING
