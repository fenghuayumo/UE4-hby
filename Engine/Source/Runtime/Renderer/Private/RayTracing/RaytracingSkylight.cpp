// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RaytracingSkyLight.cpp implements sky lighting with ray tracing
=============================================================================*/

#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"

static int32 GRayTracingSkyLight = -1;

#if RHI_RAYTRACING

#include "RayTracingSkyLight.h"
#include "RayTracingMaterialHitShaders.h"
#include "ClearQuad.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "SceneRenderTargets.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "VisualizeTexture.h"
#include "RayGenShaderUtils.h"
#include "SceneTextureParameters.h"
#include "ScreenSpaceDenoise.h"
#include "PathTracing.h"

#include "Raytracing/RaytracingOptions.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"
#include "HairStrands/HairStrandsRendering.h"

static FAutoConsoleVariableRef CVarRayTracingSkyLight(
	TEXT("r.RayTracing.SkyLight"),
	GRayTracingSkyLight,
	TEXT("Enables ray tracing SkyLight (default = 0)"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingSkyLightSamplesPerPixel = -1;
static FAutoConsoleVariableRef CVarRayTracingSkyLightSamplesPerPixel(
	TEXT("r.RayTracing.SkyLight.SamplesPerPixel"),
	GRayTracingSkyLightSamplesPerPixel,
	TEXT("Sets the samples-per-pixel for ray tracing SkyLight (default = -1)")
);

static float GRayTracingSkyLightMaxRayDistance = 1.0e7;
static FAutoConsoleVariableRef CVarRayTracingSkyLightMaxRayDistance(
	TEXT("r.RayTracing.SkyLight.MaxRayDistance"),
	GRayTracingSkyLightMaxRayDistance,
	TEXT("Sets the max ray distance for ray tracing SkyLight (default = 1.0e7)")
);

static float GRayTracingSkyLightMaxShadowThickness = 1.0e3;
static FAutoConsoleVariableRef CVarRayTracingSkyLightMaxShadowThickness(
	TEXT("r.RayTracing.SkyLight.MaxShadowThickness"),
	GRayTracingSkyLightMaxShadowThickness,
	TEXT("Sets the max shadow thickness for translucent materials for ray tracing SkyLight (default = 1.0e3)")
);

static int32 GRayTracingSkyLightSamplingStopLevel = 0;
static FAutoConsoleVariableRef CVarRayTracingSkyLightSamplingStopLevel(
	TEXT("r.RayTracing.SkyLight.Sampling.StopLevel"),
	GRayTracingSkyLightSamplingStopLevel,
	TEXT("Sets the stop level for MIP-sampling (default = 0)")
);

static int32 GRayTracingSkyLightDenoiser = 1;
static FAutoConsoleVariableRef CVarRayTracingSkyLightDenoiser(
	TEXT("r.RayTracing.SkyLight.Denoiser"),
	GRayTracingSkyLightDenoiser,
	TEXT("Denoising options (default = 1)")
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightEnableTwoSidedGeometry(
	TEXT("r.RayTracing.SkyLight.EnableTwoSidedGeometry"),
	1,
	TEXT("Enables two-sided geometry when tracing shadow rays (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightEnableMaterials(
	TEXT("r.RayTracing.SkyLight.EnableMaterials"),
	1,
	TEXT("Enables material shader binding for shadow rays. If this is disabled, then a default trivial shader is used. (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightDecoupleSampleGeneration(
	TEXT("r.RayTracing.SkyLight.DecoupleSampleGeneration"),
	1,
	TEXT("Decouples sample generation from ray traversal (default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightEnableHairVoxel(
	TEXT("r.RayTracing.SkyLight.HairVoxel"),
	1,
	TEXT("Include hair voxel representation to estimate sky occlusion"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingSkyLightScreenPercentage(
	TEXT("r.RayTracing.SkyLight.ScreenPercentage"),
	100.0f,
	TEXT("Screen percentage at which to evaluate sky occlusion"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightTiming(
	TEXT("r.RayTracing.SkyLight.Timing"),
	1,
	TEXT("Time cost of ray traced skylight ( 0 - off, 1 - on)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightMipMapTreeSampling(
	TEXT("r.RayTracing.SkyLight.MipMapTreeSampling"),
	0,
	TEXT("Sampling Use Importance Explict MipmapData"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingSkyLightAdaptiveSampling(
	TEXT("r.RayTracing.SkyLight.AdaptiveSampling"),
	0,
	TEXT("Adaptive Sampling for SkyLight"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarSkyLightAdaptiveSamplingMinimumSamplesPerPixel(
	TEXT("r.kyLight.AdaptiveSampling.MinimumSamplesPerPixel"),
	16,
	TEXT("Changes the minimum samples-per-pixel before applying adaptive sampling (default=16)\n"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int32> CVarSkyLightVarianceMapRebuildFrequency(
	TEXT("r.SkyLight.VarianceMapRebuildFrequency"),
	16,
	TEXT("Sets the variance map rebuild frequency (default = every 16 iterations)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<bool> CVarSkyLightVisualizeVarianceMap(
	TEXT("r.SkyLight.VisualizeVarianceMap"),
	false,
	TEXT("Visualize VarianceMipMap"),
	ECVF_RenderThreadSafe
);
TAutoConsoleVariable<int32> CVarSkyLightVisualizeVarianceMapLevel(
	TEXT("r.SkyLight.VisualizeVarianceMapLevel"),
	2,
	TEXT("Visualize VarianceMap Level"),
	ECVF_RenderThreadSafe
);

int32 GetRayTracingSkyLightDecoupleSampleGenerationCVarValue()
{
	return CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread();
}

int32 GetSkyLightSamplesPerPixel(const FSkyLightSceneProxy* SkyLightSceneProxy)
{
	check(SkyLightSceneProxy != nullptr);

	// Clamp to 2spp minimum due to poor 1spp denoised quality
	return GRayTracingSkyLightSamplesPerPixel >= 0 ? GRayTracingSkyLightSamplesPerPixel : FMath::Max(SkyLightSceneProxy->SamplesPerPixel, 2);
}

static bool IsRayTracingSkyLightAllowed()
{
	return ShouldRenderRayTracingEffect(GRayTracingSkyLight != 0);
}

bool ShouldRenderRayTracingSkyLight(const FSkyLightSceneProxy* SkyLightSceneProxy)
{
	if (SkyLightSceneProxy == nullptr)
	{
		return false;
	}

	bool bRayTracingSkyEnabled = GRayTracingSkyLight  < 0 
		? SkyLightSceneProxy->bCastRayTracedShadow
		: GRayTracingSkyLight != 0;

	bRayTracingSkyEnabled = bRayTracingSkyEnabled && (GetSkyLightSamplesPerPixel(SkyLightSceneProxy) > 0);

	return IsRayTracingSkyLightAllowed() && bRayTracingSkyEnabled;
}

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FSkyLightData, "SkyLight");
//IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FRayTracingAdaptiveSamplingData, "AdaptiveSamplingData");
struct FSkyLightVisibilityRays
{
	FVector4 DirectionAndPdf;
};

bool SetupSkyLightParameters(
	FRDGBuilder& GraphBuilder, FScene* Scene, const FViewInfo& View,
	bool bEnableSkylight,
	FPathTracingSkylight* SkylightParameters,
	FSkyLightData* SkyLightData)
{
	// Check if parameters should be set based on if the sky light's texture has been processed and if its mip tree has been built yet

	const bool bUseMISCompensation = true;
	if (PrepareSkyTexture(GraphBuilder, Scene, View, bEnableSkylight, bUseMISCompensation, SkylightParameters))
	{
		SkyLightData->SamplesPerPixel = GetSkyLightSamplesPerPixel(Scene->SkyLight);
		SkyLightData->MaxRayDistance = GRayTracingSkyLightMaxRayDistance;
		SkyLightData->MaxNormalBias = GetRaytracingMaxNormalBias();
		SkyLightData->bTransmission = Scene->SkyLight->bTransmission;
		SkyLightData->MaxShadowThickness = GRayTracingSkyLightMaxShadowThickness;
		SkyLightData->SamplingStopLevel = GRayTracingSkyLightSamplingStopLevel;
		ensure(SkyLightData->SamplesPerPixel > 0);

		SkyLightData->Color = FVector(Scene->SkyLight->GetEffectiveLightColor());
		SkyLightData->Texture = Scene->SkyLight->ProcessedTexture->TextureRHI;
		SkyLightData->TextureDimensions = FIntVector(Scene->SkyLight->ProcessedTexture->GetSizeX(), Scene->SkyLight->ProcessedTexture->GetSizeY(), 0);
		SkyLightData->TextureSampler = Scene->SkyLight->ProcessedTexture->SamplerStateRHI;
		SkyLightData->MipDimensions = Scene->SkyLight->ImportanceSamplingData->MipDimensions;

		SkyLightData->MipTreePosX = Scene->SkyLight->ImportanceSamplingData->MipTreePosX.SRV;
		SkyLightData->MipTreeNegX = Scene->SkyLight->ImportanceSamplingData->MipTreeNegX.SRV;
		SkyLightData->MipTreePosY = Scene->SkyLight->ImportanceSamplingData->MipTreePosY.SRV;
		SkyLightData->MipTreeNegY = Scene->SkyLight->ImportanceSamplingData->MipTreeNegY.SRV;
		SkyLightData->MipTreePosZ = Scene->SkyLight->ImportanceSamplingData->MipTreePosZ.SRV;
		SkyLightData->MipTreeNegZ = Scene->SkyLight->ImportanceSamplingData->MipTreeNegZ.SRV;

		SkyLightData->MipTreePdfPosX = Scene->SkyLight->ImportanceSamplingData->MipTreePdfPosX.SRV;
		SkyLightData->MipTreePdfNegX = Scene->SkyLight->ImportanceSamplingData->MipTreePdfNegX.SRV;
		SkyLightData->MipTreePdfPosY = Scene->SkyLight->ImportanceSamplingData->MipTreePdfPosY.SRV;
		SkyLightData->MipTreePdfNegY = Scene->SkyLight->ImportanceSamplingData->MipTreePdfNegY.SRV;
		SkyLightData->MipTreePdfPosZ = Scene->SkyLight->ImportanceSamplingData->MipTreePdfPosZ.SRV;
		SkyLightData->MipTreePdfNegZ = Scene->SkyLight->ImportanceSamplingData->MipTreePdfNegZ.SRV;
		SkyLightData->SolidAnglePdf = Scene->SkyLight->ImportanceSamplingData->SolidAnglePdf.SRV;

		return true;
	}
	else
	{
		// skylight is not enabled
		SkyLightData->SamplesPerPixel = -1;
		SkyLightData->MaxRayDistance = 0.0f;
		SkyLightData->MaxNormalBias = 0.0f;
		SkyLightData->MaxShadowThickness = 0.0f;

		SkyLightData->Color = FVector(0.0);
		SkyLightData->Texture = GBlackTextureCube->TextureRHI;
		SkyLightData->TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		SkyLightData->MipDimensions = FIntVector(0);

		SkyLightData->MipTreePosX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreeNegX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePosY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreeNegY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePosZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreeNegZ = GBlackTextureWithSRV->ShaderResourceViewRHI;

		SkyLightData->MipTreePdfPosX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePdfNegX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePdfPosY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePdfNegY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePdfPosZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->MipTreePdfNegZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData->SolidAnglePdf = GBlackTextureWithSRV->ShaderResourceViewRHI;
		return false;
	}
}

void SetupSkyLightVisibilityRaysParameters(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FSkyLightVisibilityRaysData* OutSkyLightVisibilityRaysData)
{
	// Get the Scene View State
	FSceneViewState* SceneViewState = (FSceneViewState*)View.State;

	TRefCountPtr<FRDGPooledBuffer> PooledSkyLightVisibilityRaysBuffer;
	FIntVector SkyLightVisibilityRaysDimensions;

	// Check if the Sky Light Visibility Ray Data should be set based on if decoupled sample generation is being used
	if (
		(SceneViewState != nullptr) &&
		(SceneViewState->SkyLightVisibilityRaysBuffer != nullptr) &&
		(CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() == 1))
	{
		// Set the Sky Light Visibility Ray pooled buffer to the stored pooled buffer
		PooledSkyLightVisibilityRaysBuffer = SceneViewState->SkyLightVisibilityRaysBuffer;

		// Set the Sky Light Visibility Ray Dimensions from the stored dimensions
		SkyLightVisibilityRaysDimensions = SceneViewState->SkyLightVisibilityRaysDimensions;
	}
	else
	{
		// Create a dummy Sky Light Visibility Ray buffer in a dummy RDG
		FRDGBuilder DummyGraphBuilder{ GraphBuilder.RHICmdList };
		FRDGBufferDesc DummyBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FSkyLightVisibilityRays), 1);
		FRDGBufferRef DummyRDGBuffer = DummyGraphBuilder.CreateBuffer(DummyBufferDesc, TEXT("DummySkyLightVisibilityRays"));
		FRDGBufferUAVRef DummyRDGBufferUAV = DummyGraphBuilder.CreateUAV(DummyRDGBuffer, EPixelFormat::PF_R32_UINT);

		// Clear the dummy Sky Light Visibility Ray buffer
		AddClearUAVPass(DummyGraphBuilder, DummyRDGBufferUAV, 0);

		// Set the Sky Light Visibility Ray pooled buffer to the extracted pooled dummy buffer
		DummyGraphBuilder.QueueBufferExtraction(DummyRDGBuffer, &PooledSkyLightVisibilityRaysBuffer);
		DummyGraphBuilder.Execute();

		// Set the Sky Light Visibility Ray Dimensions to a dummy value
		SkyLightVisibilityRaysDimensions = FIntVector(1);
	}

	// Set the Sky Light Visibility Ray Buffer to the pooled RDG buffer
	FRDGBufferRef SkyLightVisibilityRaysBuffer = GraphBuilder.RegisterExternalBuffer(PooledSkyLightVisibilityRaysBuffer);

	// Set Sky Light Visibility Ray Data information
	OutSkyLightVisibilityRaysData->SkyLightVisibilityRays = GraphBuilder.CreateSRV(SkyLightVisibilityRaysBuffer, EPixelFormat::PF_R32_UINT);
	OutSkyLightVisibilityRaysData->SkyLightVisibilityRaysDimensions = SkyLightVisibilityRaysDimensions;
}

class FRayTracingSkyLightRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingSkyLightRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingSkyLightRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableMaterialsDim : SHADER_PERMUTATION_BOOL("ENABLE_MATERIALS");
	class FDecoupleSampleGeneration : SHADER_PERMUTATION_BOOL("DECOUPLE_SAMPLE_GENERATION");
	class FHairLighting : SHADER_PERMUTATION_INT("USE_HAIR_LIGHTING", 2);
	class FEnableMipMapTreeDim : SHADER_PERMUTATION_BOOL("USE_MIPMAP_TREE");
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableMaterialsDim, FDecoupleSampleGeneration, FHairLighting, FEnableMipMapTreeDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWSkyOcclusionMaskUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWSkyOcclusionRayDistanceUAV)

		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FSkyLightData, SkyLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkyLightParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)
		
		SHADER_PARAMETER_STRUCT_INCLUDE(FSkyLightVisibilityRaysData, SkyLightVisibilityRaysData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingSkyLightRGS, "/Engine/Private/Raytracing/RaytracingSkylightRGS.usf", "SkyLightRGS", SF_RayGen);

class FRayTracingSkyLightAdaptiveRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingSkyLightAdaptiveRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingSkyLightAdaptiveRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FEnableMaterialsDim : SHADER_PERMUTATION_BOOL("ENABLE_MATERIALS");
	class FDecoupleSampleGeneration : SHADER_PERMUTATION_BOOL("DECOUPLE_SAMPLE_GENERATION");
	class FHairLighting : SHADER_PERMUTATION_INT("USE_HAIR_LIGHTING", 2);
	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableMaterialsDim, FDecoupleSampleGeneration, FHairLighting>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}


	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, UpscaleFactor)
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWSkyOcclusionMaskUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, SampleCountRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, PixelPositionRT)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FSkyLightData, SkyLightData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FRayTracingAdaptiveSamplingData, AdaptiveSamplingData)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkyLightParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TransmissionProfilesLinearSampler)
		SHADER_PARAMETER(FIntPoint, TexViewSize)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingSkyLightAdaptiveRGS, "/Engine/Private/Raytracing/RayTracingSkyLightAdaptiveSampling.usf", "SkyLightRGS", SF_RayGen);

class FRayTracingSkyLightPathCompactionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingSkyLightPathCompactionCS)
	SHADER_USE_PARAMETER_STRUCT(FRayTracingSkyLightPathCompactionCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), FComputeShaderUtils::kGolden2DGroupSize);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, SampleCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, PixelPositionTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RadianceSortedRedRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RadianceSortedGreenRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RadianceSortedBlueRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RadianceSortedAlphaRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, SampleCountSortedRT)
		SHADER_PARAMETER(FIntPoint, TexViewSize)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingSkyLightPathCompactionCS, "/Engine/Private/Raytracing/RayTracingCompaction.usf", "PathCompactionCS", SF_Compute);

class FCompositeSkyLightAdaptiveCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompositeSkyLightAdaptiveCS)
	SHADER_USE_PARAMETER_STRUCT(FCompositeSkyLightAdaptiveCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), FComputeShaderUtils::kGolden2DGroupSize);
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceRedTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceGreenTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceBlueTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceAlphaTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SampleCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CumulativeRadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint32>, CumulativeSampleCountTexture)
		SHADER_PARAMETER(FIntPoint, TexViewSize)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FCompositeSkyLightAdaptiveCS, "/Engine/Private/RayTracing/RayTracingAdaptiveComPosite.usf", "PathCompositeCS", SF_Compute);


class FRayTracingSkyLightVarianceTreeBuildCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingSkyLightVarianceTreeBuildCS)
	SHADER_USE_PARAMETER_STRUCT(FRayTracingSkyLightVarianceTreeBuildCS, FGlobalShader)


		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), FComputeShaderUtils::kGolden2DGroupSize);
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RadianceTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, RadianceTextureSampler)
		SHADER_PARAMETER(FIntPoint, ViewSize)
		SHADER_PARAMETER(FIntVector, VarianceMapDimensions)
		SHADER_PARAMETER(uint32, MipLevel)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWVarianceMipTree)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingSkyLightVarianceTreeBuildCS, "/Engine/Private/Raytracing/SkyLightVarianceMipTreeCS.usf", "BuildVarianceMipTreeCS", SF_Compute);

void FDeferredShadingSceneRenderer::PrepareRayTracingSkyLight(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (!IsRayTracingSkyLightAllowed())
	{
		return;
	}

	// Declare all RayGen shaders that require material closest hit shaders to be bound
	FRayTracingSkyLightRGS::FPermutationDomain PermutationVector;
	FRayTracingSkyLightAdaptiveRGS::FPermutationDomain PermutationVector1;
	for (uint32 TwoSidedGeometryIndex = 0; TwoSidedGeometryIndex < 2; ++TwoSidedGeometryIndex)
	{
		for (uint32 EnableMaterialsIndex = 0; EnableMaterialsIndex < 2; ++EnableMaterialsIndex)
		{
			for (uint32 DecoupleSampleGeneration = 0; DecoupleSampleGeneration < 2; ++DecoupleSampleGeneration)
			{
				for (int32 HairLighting = 0; HairLighting < 2; ++HairLighting)
				{
					for (int32 EnableMipmapTree = 0; EnableMipmapTree < 2; ++EnableMipmapTree)
					{
						PermutationVector.Set<FRayTracingSkyLightRGS::FEnableTwoSidedGeometryDim>(TwoSidedGeometryIndex != 0);
						PermutationVector.Set<FRayTracingSkyLightRGS::FEnableMaterialsDim>(EnableMaterialsIndex != 0);
						PermutationVector.Set<FRayTracingSkyLightRGS::FDecoupleSampleGeneration>(DecoupleSampleGeneration != 0);
						PermutationVector.Set<FRayTracingSkyLightRGS::FHairLighting>(HairLighting);
						PermutationVector.Set<FRayTracingSkyLightRGS::FEnableMipMapTreeDim>(EnableMipmapTree != 0);
						TShaderMapRef<FRayTracingSkyLightRGS> RayGenerationShader(View.ShaderMap, PermutationVector);
						OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
					}

					PermutationVector1.Set<FRayTracingSkyLightAdaptiveRGS::FEnableTwoSidedGeometryDim>(TwoSidedGeometryIndex != 0);
					PermutationVector1.Set<FRayTracingSkyLightAdaptiveRGS::FEnableMaterialsDim>(EnableMaterialsIndex != 0);
					PermutationVector1.Set<FRayTracingSkyLightAdaptiveRGS::FDecoupleSampleGeneration>(DecoupleSampleGeneration != 0);
					PermutationVector1.Set<FRayTracingSkyLightAdaptiveRGS::FHairLighting>(HairLighting);
					TShaderMapRef<FRayTracingSkyLightAdaptiveRGS> RayGenerationShader(View.ShaderMap, PermutationVector1);
					OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
				}
			}
		}
	}

}

class FGenerateSkyLightVisibilityRaysCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateSkyLightVisibilityRaysCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateSkyLightVisibilityRaysCS, FGlobalShader);

	static const uint32 kGroupSize = 16;
	using FPermutationDomain = FShaderPermutationNone;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("TILE_SIZE"), kGroupSize);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, SamplesPerPixel)

		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
		SHADER_PARAMETER_STRUCT_REF(FSkyLightData, SkyLightData)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Writable variant to allow for Sky Light Visibility Ray output
		SHADER_PARAMETER_STRUCT_INCLUDE(FWritableSkyLightVisibilityRaysData, WritableSkyLightVisibilityRaysData)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGenerateSkyLightVisibilityRaysCS, "/Engine/Private/RayTracing/GenerateSkyLightVisibilityRaysCS.usf", "MainCS", SF_Compute);


class FVisualizeVarianceMipTreeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeVarianceMipTreeCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeVarianceMipTreeCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), FComputeShaderUtils::kGolden2DGroupSize);
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, Dimensions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, MipTree)
		SHADER_PARAMETER(uint32, MipLevel)
		SHADER_PARAMETER(FIntPoint, TexViewSize)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, Output)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_SHADER_TYPE(, FVisualizeVarianceMipTreeCS, TEXT("/Engine/Private/RayTracing/VisualizeVarianceMipTreeCS.usf"), TEXT("VisualizeMipTreeCS"), SF_Compute);

static void GenerateSkyLightVisibilityRays(
	FRDGBuilder& GraphBuilder,
	FViewInfo& View,
	FPathTracingSkylight& SkylightParameters,
	FSkyLightData& SkyLightData,
	FRDGBufferRef& SkyLightVisibilityRaysBuffer,
	FIntVector& Dimensions
)
{
	FRHIResourceCreateInfo CreateInfo;
	CreateInfo.DebugName = TEXT("SkyLightVisibilityRays");

	// Allocating mask of 256 x 256 rays
	Dimensions = FIntVector(256, 256, 0);

	// Compute Pass parameter definition
	FGenerateSkyLightVisibilityRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateSkyLightVisibilityRaysCS::FParameters>();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->SkyLightData = CreateUniformBufferImmediate(SkyLightData, EUniformBufferUsage::UniformBuffer_SingleDraw);
	PassParameters->SkylightParameters = SkylightParameters;

	// Output structured buffer creation
	FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FSkyLightVisibilityRays), Dimensions.X * Dimensions.Y * SkyLightData.SamplesPerPixel);
	SkyLightVisibilityRaysBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("SkyLightVisibilityRays"));

	PassParameters->WritableSkyLightVisibilityRaysData.SkyLightVisibilityRaysDimensions = FIntVector(Dimensions.X, Dimensions.Y, 0);
	PassParameters->WritableSkyLightVisibilityRaysData.OutSkyLightVisibilityRays = GraphBuilder.CreateUAV(SkyLightVisibilityRaysBuffer, EPixelFormat::PF_R32_UINT);

	auto ComputeShader = View.ShaderMap->GetShader<FGenerateSkyLightVisibilityRaysCS>();

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateSkyLightVisibilityRays"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(FIntPoint(Dimensions.X, Dimensions.Y), FGenerateSkyLightVisibilityRaysCS::kGroupSize)
	);
}

DECLARE_GPU_STAT_NAMED(RayTracingSkyLight, TEXT("Ray Tracing SkyLight"));

void FDeferredShadingSceneRenderer::RenderRayTracingSkyLightProgressive(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef& OutSkyLightTexture,
	FRDGTextureRef& OutHitDistanceTexture,
	const FHairStrandsRenderingData* HairDatas)
{
	FSkyLightSceneProxy* SkyLight = Scene->SkyLight;

	// Fill Sky Light parameters
	const bool bShouldRenderRayTracingSkyLight = ShouldRenderRayTracingSkyLight(SkyLight);
	FPathTracingSkylight SkylightParameters;
	FSkyLightData SkyLightData;
	if (!SetupSkyLightParameters(GraphBuilder, Scene, Views[0], bShouldRenderRayTracingSkyLight, &SkylightParameters, &SkyLightData))
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "RayTracingSkyLight");
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingSkyLight);

	check(SceneColorTexture);

	float ResolutionFraction = 1.0f;
	if (GRayTracingSkyLightDenoiser != 0)
	{
		ResolutionFraction = FMath::Clamp(CVarRayTracingSkyLightScreenPercentage.GetValueOnRenderThread() / 100.0f, 0.25f, 1.0f);
	}

	int32 UpscaleFactor = int32(1.0 / ResolutionFraction);
	ResolutionFraction = 1.0f / UpscaleFactor;
	FRayTracingAdaptiveSamplingData AdaptiveSamplingData;
	FRDGTexture* RadianceSortedRed = nullptr;
	FRDGTexture* RadianceSortedGreen = nullptr;
	FRDGTexture* RadianceSortedBlue = nullptr;
	FRDGTexture* RadianceSortedAlpha = nullptr;
	FRDGTexture* SampleCountSorted = nullptr;
	FRDGTexture* CumulativeSampleCount = nullptr;
	FRDGTexture* SampleCount = nullptr;
	FRDGTexture* PixelPositionTexture = nullptr;
	FRDGTexture* RadianceTexture = nullptr;
	FRDGBufferRef	VarianceMipTree = nullptr;
	FIntVector		VarianceMipTreeDimensions;

	FIntPoint TexSize = Views[0].ViewRect.Size() / UpscaleFactor;
	uint32 MipLevelCount = FMath::Min(FMath::CeilLogTwo(TexSize.X), FMath::CeilLogTwo(TexSize.Y));
	VarianceMipTreeDimensions = FIntVector(1 << MipLevelCount, 1 << MipLevelCount, 1);
	uint32 NumElements = VarianceMipTreeDimensions.X * VarianceMipTreeDimensions.Y;

	FRDGTextureDesc Desc = SceneColorTexture->Desc;
	Desc.Reset();
	Desc.Format = PF_FloatRGBA;
	Desc.Flags |= TexCreate_UAV;
	Desc.Extent /= UpscaleFactor;
	RadianceTexture = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.RadianceTexture"));
	Desc.Format = PF_G16R16;
	OutHitDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingSkyLightHitDistance"), ERDGTextureFlags::MultiFrame);
	for (uint32 MipLevel = 1; MipLevel <= MipLevelCount; ++MipLevel)
	{
		NumElements += (VarianceMipTreeDimensions.X >> MipLevel) * (VarianceMipTreeDimensions.Y >> MipLevel);
	}

	if (Views.Num() > 0 && Views[0].ViewState->RayTracingSkyLightRadianceRT)
	{
		OutSkyLightTexture = GraphBuilder.RegisterExternalTexture(Views[0].ViewState->RayTracingSkyLightRadianceRT, TEXT("RayTracingSkylight"));
		CumulativeSampleCount = GraphBuilder.RegisterExternalTexture(Views[0].ViewState->RayTracingSkyLightSampleCountRT, TEXT("CumulativeSkylightSampleCount"));
	}
	else
	{
		Desc.Format = PF_FloatRGBA;
		OutSkyLightTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingSkylight"), ERDGTextureFlags::MultiFrame);
		Desc.Format = PF_R32_UINT;
		CumulativeSampleCount = GraphBuilder.CreateTexture(Desc, TEXT("CumulativeSkylightSampleCount"), ERDGTextureFlags::MultiFrame);
	}

	Desc.Reset();
	Desc.Format = PF_R32_UINT;
	Desc.Flags |= TexCreate_UAV;
	Desc.Extent /= UpscaleFactor;
	RadianceSortedRed = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.RadianceSortedRed"));
	RadianceSortedGreen = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.RadianceSortedGreen"));
	RadianceSortedBlue = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.RadianceSortedBlue"));
	RadianceSortedAlpha = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.RadianceSortedAlpha"));
	SampleCountSorted = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.SampleCountSorted"));
	SampleCount = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.SampleCount"));
	PixelPositionTexture = GraphBuilder.CreateTexture(Desc, TEXT("SkyLight.PixelPosition"));

	if (Views[0].ViewState->RayTracingSkyLightVarianceMipTree)
	{
		VarianceMipTree = GraphBuilder.RegisterExternalBuffer(Views[0].ViewState->RayTracingSkyLightVarianceMipTree, TEXT("VarianceMipTree"));
	}
	else
	{
		VarianceMipTree = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumElements), TEXT("VarianceMipTree"), ERDGBufferFlags::MultiFrame);
	}

	const FRDGTextureDesc& SceneColorDesc = SceneColorTexture->Desc;
	FRDGTextureUAV* SkyLightkUAV = GraphBuilder.CreateUAV(OutSkyLightTexture);

	// Fill Scene Texture parameters
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	FRHITexture* SubsurfaceProfileTexture = GBlackTexture->TextureRHI;
	if (IPooledRenderTarget* SubsurfaceProfileRT = GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList))
	{
		SubsurfaceProfileTexture = SubsurfaceProfileRT->GetShaderResourceRHI();
	}

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	int ViewIndex = 0;
	static uint32 Iteration = 0;

	float EmptyData[1] = { 0.0 };
	auto DummyBuffer = CreateVertexBuffer(GraphBuilder, TEXT("GPU DummyBuffer"), FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1), EmptyData, sizeof(EmptyData));
	for (FViewInfo& View : Views)
	{
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
		FSceneViewState* SceneViewState = (FSceneViewState*)View.State;
		const uint32 MinSample = CVarSkyLightAdaptiveSamplingMinimumSamplesPerPixel.GetValueOnRenderThread();
		{
			FRayTracingSkyLightAdaptiveRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingSkyLightAdaptiveRGS::FParameters>();
			PassParameters->RWSkyOcclusionMaskUAV = GraphBuilder.CreateUAV(RadianceTexture);
			PassParameters->PixelPositionRT = GraphBuilder.CreateUAV(PixelPositionTexture);
			PassParameters->SampleCountRT = GraphBuilder.CreateUAV(SampleCount);

			PassParameters->SkyLightParameters = SkylightParameters;
			PassParameters->SkyLightData = CreateUniformBufferImmediate(SkyLightData, EUniformBufferUsage::UniformBuffer_SingleDraw);

			PassParameters->SSProfilesTexture = SubsurfaceProfileTexture;
			PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->SceneTextures = SceneTextures;
			PassParameters->UpscaleFactor = UpscaleFactor;
			PassParameters->TexViewSize = TexSize;

			if (VarianceMipTree && (Iteration > MinSample))
			{
				PassParameters->AdaptiveSamplingData.RandomSeq = 0;
				PassParameters->AdaptiveSamplingData.Iteration = Iteration;
				PassParameters->AdaptiveSamplingData.TemporalSeed = 0;
				PassParameters->AdaptiveSamplingData.VarianceDimensions = VarianceMipTreeDimensions;
				PassParameters->AdaptiveSamplingData.VarianceMipTree = GraphBuilder.CreateSRV(VarianceMipTree, PF_R32_FLOAT);
				PassParameters->AdaptiveSamplingData.MinimumSamplesPerPixel = CVarSkyLightAdaptiveSamplingMinimumSamplesPerPixel.GetValueOnRenderThread();
			}
			else
			{
				PassParameters->AdaptiveSamplingData.RandomSeq = 0;
				PassParameters->AdaptiveSamplingData.Iteration = Iteration;
				PassParameters->AdaptiveSamplingData.TemporalSeed = 0;
				PassParameters->AdaptiveSamplingData.VarianceDimensions = FIntVector(0);

				PassParameters->AdaptiveSamplingData.VarianceMipTree = GraphBuilder.CreateSRV(DummyBuffer, PF_R32_FLOAT);
				PassParameters->AdaptiveSamplingData.MinimumSamplesPerPixel = CVarSkyLightAdaptiveSamplingMinimumSamplesPerPixel.GetValueOnRenderThread();
			}

			PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			const bool bUseHairLighting =
				HairDatas && ViewIndex < HairDatas->MacroGroupsPerViews.Views.Num() &&
				HairDatas->MacroGroupsPerViews.Views[ViewIndex].VirtualVoxelResources.IsValid() &&
				CVarRayTracingSkyLightEnableHairVoxel.GetValueOnRenderThread() > 0;
			if (bUseHairLighting)
			{
				PassParameters->VirtualVoxel = HairDatas->MacroGroupsPerViews.Views[ViewIndex].VirtualVoxelResources.UniformBuffer;
			}

			FRayTracingSkyLightAdaptiveRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRayTracingSkyLightAdaptiveRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingSkyLightEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
			PermutationVector.Set<FRayTracingSkyLightAdaptiveRGS::FEnableMaterialsDim>(CVarRayTracingSkyLightEnableMaterials.GetValueOnRenderThread() != 0);
			PermutationVector.Set<FRayTracingSkyLightAdaptiveRGS::FDecoupleSampleGeneration>(CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() != 0);
			PermutationVector.Set<FRayTracingSkyLightAdaptiveRGS::FHairLighting>(bUseHairLighting ? 1 : 0);
			TShaderMapRef<FRayTracingSkyLightAdaptiveRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
			ClearUnusedGraphResources(RayGenerationShader, PassParameters);

			FIntPoint RayTracingResolution = View.ViewRect.Size() / UpscaleFactor;
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("SkyLightRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
				PassParameters,
				ERDGPassFlags::Compute,
				[PassParameters, this, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
				{
					FRayTracingShaderBindingsWriter GlobalResources;
					SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

					FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
					if (CVarRayTracingSkyLightEnableMaterials.GetValueOnRenderThread() == 0)
					{
						// Declare default pipeline
						FRayTracingPipelineStateInitializer Initializer;
						Initializer.MaxPayloadSizeInBytes = 64; // sizeof(FPackedMaterialClosestHitPayload)
						FRHIRayTracingShader* RayGenShaderTable[] = { RayGenerationShader.GetRayTracingShader() };
						Initializer.SetRayGenShaderTable(RayGenShaderTable);

						FRHIRayTracingShader* HitGroupTable[] = { View.ShaderMap->GetShader<FOpaqueShadowHitGroup>().GetRayTracingShader() };
						Initializer.SetHitGroupTable(HitGroupTable);
						Initializer.bAllowHitGroupIndexing = false; // Use the same hit shader for all geometry in the scene by disabling SBT indexing.

						Pipeline = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, Initializer);
					}

					FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
					RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
				});
		}
		{
			auto RadianceSortedAlphaUAV = GraphBuilder.CreateUAV(RadianceSortedAlpha);
			auto RadianceSortedRedUAV = GraphBuilder.CreateUAV(RadianceSortedRed);
			auto RadianceSortedGreenUAV = GraphBuilder.CreateUAV(RadianceSortedGreen);
			auto RadianceSortedBlueUAV = GraphBuilder.CreateUAV(RadianceSortedBlue);
			auto SampleCountSortedUAV = GraphBuilder.CreateUAV(SampleCountSorted);

			uint32 balck[4] = { 0 };
			AddClearUAVPass(GraphBuilder, RadianceSortedAlphaUAV, balck);
			AddClearUAVPass(GraphBuilder, RadianceSortedRedUAV, balck);
			AddClearUAVPass(GraphBuilder, RadianceSortedGreenUAV, balck);
			AddClearUAVPass(GraphBuilder, RadianceSortedBlueUAV, balck);
			AddClearUAVPass(GraphBuilder, SampleCountSortedUAV, balck);

			TShaderMapRef<FRayTracingSkyLightPathCompactionCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FRayTracingSkyLightPathCompactionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingSkyLightPathCompactionCS::FParameters>();
			PassParameters->RadianceSortedAlphaRT = RadianceSortedAlphaUAV;
			PassParameters->RadianceSortedRedRT = RadianceSortedRedUAV;
			PassParameters->RadianceSortedGreenRT = RadianceSortedGreenUAV;
			PassParameters->RadianceSortedBlueRT = RadianceSortedBlueUAV;
			PassParameters->SampleCountSortedRT = SampleCountSortedUAV;

			PassParameters->RadianceTexture = RadianceTexture;
			PassParameters->PixelPositionTexture = PixelPositionTexture;
			PassParameters->SampleCountTexture = SampleCount;
			PassParameters->TexViewSize = TexSize;
			FIntPoint ComputeResolution = TexSize;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SkyLightCompaction"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(ComputeResolution, FComputeShaderUtils::kGolden2DGroupSize));
		}

		{
			TShaderMapRef<FCompositeSkyLightAdaptiveCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			FCompositeSkyLightAdaptiveCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCompositeSkyLightAdaptiveCS::FParameters>();
			PassParameters->CumulativeRadianceTexture = SkyLightkUAV;
			PassParameters->CumulativeSampleCountTexture = GraphBuilder.CreateUAV(CumulativeSampleCount);

			PassParameters->RadianceAlphaTexture = RadianceSortedAlpha;
			PassParameters->RadianceGreenTexture = RadianceSortedGreen;
			PassParameters->RadianceRedTexture = RadianceSortedRed;
			PassParameters->RadianceBlueTexture = RadianceSortedBlue;
			PassParameters->SampleCountTexture = SampleCountSorted;
			PassParameters->TexViewSize = TexSize;
			FIntPoint ComputeResolution = TexSize;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SkyLightAdaptiveComposite"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(ComputeResolution, FComputeShaderUtils::kGolden2DGroupSize));
		}
		if (Iteration % CVarSkyLightVarianceMapRebuildFrequency.GetValueOnRenderThread() == 0)
		{
			//Variance Build
			FIntPoint ComputeResolution = View.ViewRect.Size() / UpscaleFactor;
			for (uint32 MipLevel = 0; MipLevel <= MipLevelCount; ++MipLevel)
			{
				TShaderMapRef<FRayTracingSkyLightVarianceTreeBuildCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
				FRayTracingSkyLightVarianceTreeBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingSkyLightVarianceTreeBuildCS::FParameters>();
				PassParameters->RadianceTexture = OutSkyLightTexture;
				PassParameters->RWVarianceMipTree = GraphBuilder.CreateUAV(VarianceMipTree, PF_R32_FLOAT);
				PassParameters->MipLevel = MipLevel;
				PassParameters->VarianceMapDimensions = VarianceMipTreeDimensions;
				PassParameters->RadianceTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
				PassParameters->ViewSize = TexSize;
				FIntVector MipLevelDimensions = FIntVector(VarianceMipTreeDimensions.X >> MipLevel, VarianceMipTreeDimensions.Y >> MipLevel, 1);
				FIntVector NumGroups = FIntVector::DivideAndRoundUp(MipLevelDimensions, FComputeShaderUtils::kGolden2DGroupSize);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("RayTracingSkyLightVarianceTreeBuildCS %d", MipLevel),
					ComputeShader,
					PassParameters,
					NumGroups);

			}
		}

		if (CVarSkyLightVisualizeVarianceMap.GetValueOnRenderThread() && (Iteration > MinSample))
		{
			FVisualizeVarianceMipTreeCS::FParameters* VisualizeParameters = GraphBuilder.AllocParameters<FVisualizeVarianceMipTreeCS::FParameters>();

			VisualizeParameters->Dimensions = VarianceMipTreeDimensions;
			//VisualizeParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::EClear);
			Desc.Format = PF_R32_FLOAT;
			auto DebugVis = GraphBuilder.CreateTexture(Desc, TEXT("MipMapTree"));
			VisualizeParameters->Output = GraphBuilder.CreateUAV(DebugVis);
			VisualizeParameters->TexViewSize = TexSize;
			VisualizeParameters->MipTree = GraphBuilder.CreateSRV(VarianceMipTree, PF_R32_FLOAT);
			VisualizeParameters->MipLevel = CVarSkyLightVisualizeVarianceMapLevel.GetValueOnRenderThread();
			FScreenPassTextureViewport Viewport(SceneColorTexture, View.ViewRect);

			TShaderMapRef<FVisualizeVarianceMipTreeCS> ComputeShader(View.ShaderMap);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VisualizeVarianceTreeCS"),
				ComputeShader,
				VisualizeParameters,
				FComputeShaderUtils::GetGroupCount(TexSize, FComputeShaderUtils::kGolden2DGroupSize));
			//AddDrawScreenPass(
			//	GraphBuilder,
			//	RDG_EVENT_NAME("Path Tracer Display (%d x %d)", View.ViewRect.Size().X, View.ViewRect.Size().Y),
			//	View,
			//	Viewport,
			//	Viewport,
			//	PixelShader,
			//	VisualizeParameters
			//);

		}
		GraphBuilder.QueueTextureExtraction(OutSkyLightTexture, &View.ViewState->RayTracingSkyLightRadianceRT);
		GraphBuilder.QueueTextureExtraction(CumulativeSampleCount, &View.ViewState->RayTracingSkyLightSampleCountRT);
		GraphBuilder.QueueBufferExtraction(VarianceMipTree, &View.ViewState->RayTracingSkyLightVarianceMipTree);
		Iteration++;
	}

}

void FDeferredShadingSceneRenderer::RenderRayTracingSkyLight(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef& OutSkyLightTexture,
	FRDGTextureRef& OutHitDistanceTexture,
	const FHairStrandsRenderingData* HairDatas)
{
	if (CVarRayTracingSkyLightAdaptiveSampling.GetValueOnRenderThread())
	{
		return RenderRayTracingSkyLightProgressive(GraphBuilder, SceneColorTexture, OutSkyLightTexture, OutHitDistanceTexture, HairDatas);
	}

	FSkyLightSceneProxy* SkyLight = Scene->SkyLight;
	
	// Fill Sky Light parameters
	const bool bShouldRenderRayTracingSkyLight = ShouldRenderRayTracingSkyLight(SkyLight);
	FPathTracingSkylight SkylightParameters;
	FSkyLightData SkyLightData;
	if (!SetupSkyLightParameters(GraphBuilder, Scene, Views[0], bShouldRenderRayTracingSkyLight, &SkylightParameters, &SkyLightData))
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "RayTracingSkyLight");
	RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingSkyLight);

	check(SceneColorTexture);

	float ResolutionFraction = 1.0f;
	if (GRayTracingSkyLightDenoiser != 0)
	{
		ResolutionFraction = FMath::Clamp(CVarRayTracingSkyLightScreenPercentage.GetValueOnRenderThread() / 100.0f, 0.25f, 1.0f);
	}

	int32 UpscaleFactor = int32(1.0 / ResolutionFraction);
	ResolutionFraction = 1.0f / UpscaleFactor;
	{
		FRDGTextureDesc Desc = SceneColorTexture->Desc;
		Desc.Reset();
		Desc.Format = PF_FloatRGBA;
		Desc.Flags |= TexCreate_UAV;
		Desc.Extent /= UpscaleFactor;
		OutSkyLightTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingSkylight"));

		Desc.Format = PF_G16R16;
		OutHitDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingSkyLightHitDistance"));
	}


	FRDGBufferRef SkyLightVisibilityRaysBuffer;
	FIntVector SkyLightVisibilityRaysDimensions;
	if (CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() == 1)
	{
		GenerateSkyLightVisibilityRays(GraphBuilder, Views[0], SkylightParameters, SkyLightData, SkyLightVisibilityRaysBuffer, SkyLightVisibilityRaysDimensions);
	}
	else
	{
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FSkyLightVisibilityRays), 1);
		SkyLightVisibilityRaysBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("SkyLightVisibilityRays"));
		SkyLightVisibilityRaysDimensions = FIntVector(1);
	}

	const FRDGTextureDesc& SceneColorDesc = SceneColorTexture->Desc;
	FRDGTextureUAV* SkyLightkUAV = GraphBuilder.CreateUAV(OutSkyLightTexture);
	FRDGTextureUAV* RayDistanceUAV = GraphBuilder.CreateUAV(OutHitDistanceTexture);

	// Fill Scene Texture parameters
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	FRHITexture* SubsurfaceProfileTexture = GBlackTexture->TextureRHI;
	if (IPooledRenderTarget* SubsurfaceProfileRT = GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList))
	{
		SubsurfaceProfileTexture = SubsurfaceProfileRT->GetShaderResourceRHI();
	}

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	int32 ViewIndex = 0;

	for (FViewInfo& View : Views)
	{
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		FSceneViewState* SceneViewState = (FSceneViewState*)View.State;

		FRayTracingSkyLightRGS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRayTracingSkyLightRGS::FParameters>();
		PassParameters->RWSkyOcclusionMaskUAV = SkyLightkUAV;
		PassParameters->RWSkyOcclusionRayDistanceUAV = RayDistanceUAV;
		PassParameters->SkyLightParameters = SkylightParameters;
		PassParameters->SkyLightData = CreateUniformBufferImmediate(SkyLightData, EUniformBufferUsage::UniformBuffer_SingleDraw);
		PassParameters->SkyLightVisibilityRaysData.SkyLightVisibilityRaysDimensions = SkyLightVisibilityRaysDimensions;
		if (CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() == 1)
		{
			PassParameters->SkyLightVisibilityRaysData.SkyLightVisibilityRays = GraphBuilder.CreateSRV(SkyLightVisibilityRaysBuffer, EPixelFormat::PF_R32_UINT);
		}
		PassParameters->SSProfilesTexture = SubsurfaceProfileTexture;
		PassParameters->TransmissionProfilesLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SceneTextures = SceneTextures;
		PassParameters->UpscaleFactor = UpscaleFactor;

		PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

		if (VisualizeRayTracingTiming(View) && CVarRayTracingSkyLightTiming.GetValueOnRenderThread() != 0)
		{
			PassParameters->AccumulateTime = 1;
			PassParameters->CumulativeTime = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming));
		}
		else
		{
			PassParameters->AccumulateTime = 0;
			PassParameters->CumulativeTime = RayDistanceUAV; // bogus UAV to just keep validation happy as it is dynamically unused
		}

		const bool bUseHairLighting = 
			HairDatas && ViewIndex < HairDatas->MacroGroupsPerViews.Views.Num() && 
			HairDatas->MacroGroupsPerViews.Views[ViewIndex].VirtualVoxelResources.IsValid() && 
			CVarRayTracingSkyLightEnableHairVoxel.GetValueOnRenderThread() > 0;
		if (bUseHairLighting)
		{
			PassParameters->VirtualVoxel = HairDatas->MacroGroupsPerViews.Views[ViewIndex].VirtualVoxelResources.UniformBuffer;
		}

		FRayTracingSkyLightRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingSkyLightRGS::FEnableTwoSidedGeometryDim>(CVarRayTracingSkyLightEnableTwoSidedGeometry.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FRayTracingSkyLightRGS::FEnableMaterialsDim>(CVarRayTracingSkyLightEnableMaterials.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FRayTracingSkyLightRGS::FDecoupleSampleGeneration>(CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FRayTracingSkyLightRGS::FHairLighting>(bUseHairLighting ? 1 : 0);
		PermutationVector.Set<FRayTracingSkyLightRGS::FEnableMipMapTreeDim>(CVarRayTracingSkyLightMipMapTreeSampling.GetValueOnRenderThread() != 0);
		TShaderMapRef<FRayTracingSkyLightRGS> RayGenerationShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		FIntPoint RayTracingResolution = View.ViewRect.Size() / UpscaleFactor;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SkyLightRayTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, this, &View, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
		{
			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

			FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
			if (CVarRayTracingSkyLightEnableMaterials.GetValueOnRenderThread() == 0)
			{
				// Declare default pipeline
				FRayTracingPipelineStateInitializer Initializer;
				Initializer.MaxPayloadSizeInBytes = 64; // sizeof(FPackedMaterialClosestHitPayload)
				FRHIRayTracingShader* RayGenShaderTable[] = { RayGenerationShader.GetRayTracingShader() };
				Initializer.SetRayGenShaderTable(RayGenShaderTable);

				FRHIRayTracingShader* HitGroupTable[] = { View.ShaderMap->GetShader<FOpaqueShadowHitGroup>().GetRayTracingShader() };
				Initializer.SetHitGroupTable(HitGroupTable);
				Initializer.bAllowHitGroupIndexing = false; // Use the same hit shader for all geometry in the scene by disabling SBT indexing.

				Pipeline = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, Initializer);
			}

			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
		});

		// Denoising
		if (GRayTracingSkyLightDenoiser != 0)
		{
			const IScreenSpaceDenoiser* DefaultDenoiser = IScreenSpaceDenoiser::GetDefaultDenoiser();
			const IScreenSpaceDenoiser* DenoiserToUse = DefaultDenoiser;// GRayTracingGlobalIlluminationDenoiser == 1 ? DefaultDenoiser : GScreenSpaceDenoiser;

			IScreenSpaceDenoiser::FDiffuseIndirectInputs DenoiserInputs;
			DenoiserInputs.Color = OutSkyLightTexture;
			DenoiserInputs.RayHitDistance = OutHitDistanceTexture;

			{
				IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig RayTracingConfig;
				RayTracingConfig.ResolutionFraction = ResolutionFraction;
				RayTracingConfig.RayCountPerPixel = GetSkyLightSamplesPerPixel(SkyLight);

				RDG_EVENT_SCOPE(GraphBuilder, "%s%s(SkyLight) %dx%d",
					DenoiserToUse != DefaultDenoiser ? TEXT("ThirdParty ") : TEXT(""),
					DenoiserToUse->GetDebugName(),
					View.ViewRect.Width(), View.ViewRect.Height());

				IScreenSpaceDenoiser::FDiffuseIndirectOutputs DenoiserOutputs = DenoiserToUse->DenoiseSkyLight(
					GraphBuilder,
					View,
					&View.PrevViewInfo,
					SceneTextures,
					DenoiserInputs,
					RayTracingConfig);

				OutSkyLightTexture = DenoiserOutputs.Color;
			}
		}

		if (SceneViewState != nullptr)
		{
			if (CVarRayTracingSkyLightDecoupleSampleGeneration.GetValueOnRenderThread() == 1)
			{
				// Set the Sky Light Visibility Ray dimensions and its extracted pooled RDG buffer on the scene view state
				GraphBuilder.QueueBufferExtraction(SkyLightVisibilityRaysBuffer, &SceneViewState->SkyLightVisibilityRaysBuffer);
				SceneViewState->SkyLightVisibilityRaysDimensions = SkyLightVisibilityRaysDimensions;
			}
			else
			{
				// Set invalid Sky Light Visibility Ray dimensions and pooled RDG buffer
				SceneViewState->SkyLightVisibilityRaysBuffer = nullptr;
				SceneViewState->SkyLightVisibilityRaysDimensions = FIntVector(1);
			}
		}

		++ViewIndex;
	}
}

class FCompositeSkyLightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompositeSkyLightPS)
	SHADER_USE_PARAMETER_STRUCT(FCompositeSkyLightPS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RENDER_TARGET_BINDING_SLOTS()
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkyLightTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SkyLightTextureSampler)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FCompositeSkyLightPS, "/Engine/Private/RayTracing/CompositeSkyLightPS.usf", "CompositeSkyLightPS", SF_Pixel);


#endif // RHI_RAYTRACING

void FDeferredShadingSceneRenderer::CompositeRayTracingSkyLight(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SceneColorTexture,
	FIntPoint SceneTextureExtent,
	FRDGTextureRef SkyLightRT,
	FRDGTextureRef HitDistanceRT)
#if RHI_RAYTRACING
{
	check(SkyLightRT);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];

		FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

		FCompositeSkyLightPS::FParameters *PassParameters = GraphBuilder.AllocParameters<FCompositeSkyLightPS::FParameters>();
		PassParameters->SkyLightTexture = SkyLightRT;
		PassParameters->SkyLightTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->ViewUniformBuffer = Views[ViewIndex].ViewUniformBuffer;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
		PassParameters->SceneTextures = SceneTextures;

		// dxr_todo: Unify with RTGI compositing workflow
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GlobalIlluminationComposite"),
			PassParameters,
			ERDGPassFlags::Raster,
			[this, &View, PassParameters, SceneTextureExtent](FRHICommandListImmediate& RHICmdList)
		{
			TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
			TShaderMapRef<FCompositeSkyLightPS> PixelShader(View.ShaderMap);
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

			// Additive blending
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			RHICmdList.SetViewport((float)View.ViewRect.Min.X, (float)View.ViewRect.Min.Y, 0.0f, (float)View.ViewRect.Max.X, (float)View.ViewRect.Max.Y, 1.0f);

			DrawRectangle(
				RHICmdList,
				0, 0,
				View.ViewRect.Width(), View.ViewRect.Height(),
				View.ViewRect.Min.X, View.ViewRect.Min.Y,
				View.ViewRect.Width(), View.ViewRect.Height(),
				FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
				SceneTextureExtent,
				VertexShader
			);
		});
	}
}
#else
{
	unimplemented();
}
#endif

class FVisualizeSkyLightMipTreePS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FVisualizeSkyLightMipTreePS, Global);

public:
	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	FVisualizeSkyLightMipTreePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		DimensionsParameter.Bind(Initializer.ParameterMap, TEXT("Dimensions"));
		MipTreePosXParameter.Bind(Initializer.ParameterMap, TEXT("MipTreePosX"));
		MipTreeNegXParameter.Bind(Initializer.ParameterMap, TEXT("MipTreeNegX"));
		MipTreePosYParameter.Bind(Initializer.ParameterMap, TEXT("MipTreePosY"));
		MipTreeNegYParameter.Bind(Initializer.ParameterMap, TEXT("MipTreeNegY"));
		MipTreePosZParameter.Bind(Initializer.ParameterMap, TEXT("MipTreePosZ"));
		MipTreeNegZParameter.Bind(Initializer.ParameterMap, TEXT("MipTreeNegZ"));
	}

	FVisualizeSkyLightMipTreePS() {}

	template<typename TRHICommandList>
	void SetParameters(
		TRHICommandList& RHICmdList,
		const FViewInfo& View,
		const FIntVector Dimensions,
		const FRWBuffer& MipTreePosX,
		const FRWBuffer& MipTreeNegX,
		const FRWBuffer& MipTreePosY,
		const FRWBuffer& MipTreeNegY,
		const FRWBuffer& MipTreePosZ,
		const FRWBuffer& MipTreeNegZ)
	{
		FRHIPixelShader* ShaderRHI = RHICmdList.GetBoundPixelShader();
		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, ShaderRHI, View.ViewUniformBuffer);

		SetShaderValue(RHICmdList, ShaderRHI, DimensionsParameter, Dimensions);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreePosXParameter, MipTreePosX.SRV);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreeNegXParameter, MipTreeNegX.SRV);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreePosYParameter, MipTreePosY.SRV);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreeNegYParameter, MipTreeNegY.SRV);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreePosZParameter, MipTreePosZ.SRV);
		SetSRVParameter(RHICmdList, ShaderRHI, MipTreeNegZParameter, MipTreeNegZ.SRV);
	}

private:
	LAYOUT_FIELD(FShaderParameter, DimensionsParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreePosXParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreeNegXParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreePosYParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreeNegYParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreePosZParameter);
	LAYOUT_FIELD(FShaderResourceParameter, MipTreeNegZParameter);
};

#if RHI_RAYTRACING

IMPLEMENT_SHADER_TYPE(, FVisualizeSkyLightMipTreePS, TEXT("/Engine/Private/RayTracing/VisualizeSkyLightMipTreePS.usf"), TEXT("VisualizeSkyLightMipTreePS"), SF_Pixel)

void FDeferredShadingSceneRenderer::VisualizeSkyLightMipTree(
	FRHICommandListImmediate& RHICmdList,
	const FViewInfo& View,
	FRWBuffer& SkyLightMipTreePosX,
	FRWBuffer& SkyLightMipTreeNegX,
	FRWBuffer& SkyLightMipTreePosY,
	FRWBuffer& SkyLightMipTreeNegY,
	FRWBuffer& SkyLightMipTreePosZ,
	FRWBuffer& SkyLightMipTreeNegZ,
	const FIntVector& SkyLightMipDimensions)
{
	// Allocate render target
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
	TRefCountPtr<IPooledRenderTarget> SceneColor = SceneContext.GetSceneColor();
	FPooledRenderTargetDesc Desc = SceneColor->GetDesc();
	Desc.Flags &= ~(TexCreate_FastVRAM | TexCreate_Transient);
	TRefCountPtr<IPooledRenderTarget> SkyLightMipTreeRT;
	GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SkyLightMipTreeRT, TEXT("SkyLightMipTreeRT"));

	// Define shaders
	const auto ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FPostProcessVS> VertexShader(ShaderMap);
	TShaderMapRef<FVisualizeSkyLightMipTreePS> PixelShader(ShaderMap);
	FRHITexture* RenderTargets[2] =
	{
		SceneColor->GetRenderTargetItem().TargetableTexture,
		SkyLightMipTreeRT->GetRenderTargetItem().TargetableTexture
	};
	FRHIRenderPassInfo RenderPassInfo(2, RenderTargets, ERenderTargetActions::Load_Store);
	RHICmdList.BeginRenderPass(RenderPassInfo, TEXT("SkyLight Visualization"));

	// PSO definition
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	// Transition to graphics
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosX.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegX.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosY.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegY.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosZ.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegZ.UAV, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

	// Draw
	RHICmdList.SetViewport((float)View.ViewRect.Min.X, (float)View.ViewRect.Min.Y, 0.0f, (float)View.ViewRect.Max.X, (float)View.ViewRect.Max.Y, 1.0f);
	PixelShader->SetParameters(RHICmdList, View, SkyLightMipDimensions, SkyLightMipTreePosX, SkyLightMipTreeNegX, SkyLightMipTreePosY, SkyLightMipTreeNegY, SkyLightMipTreePosZ, SkyLightMipTreeNegZ);
	DrawRectangle(
		RHICmdList,
		0, 0,
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Min.X, View.ViewRect.Min.Y,
		View.ViewRect.Width(), View.ViewRect.Height(),
		FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
		SceneContext.GetBufferSizeXY(),
		VertexShader);
	RHICmdList.EndRenderPass();
	GVisualizeTexture.SetCheckPoint(RHICmdList, SkyLightMipTreeRT);

	RHICmdList.CopyToResolveTarget(SceneColor->GetRenderTargetItem().TargetableTexture, SceneColor->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());

	// Transition to compute
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosX.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegX.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosY.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegY.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreePosZ.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
	RHICmdList.Transition(FRHITransitionInfo(SkyLightMipTreeNegZ.UAV, ERHIAccess::Unknown, ERHIAccess::ERWBarrier));
}
#endif
