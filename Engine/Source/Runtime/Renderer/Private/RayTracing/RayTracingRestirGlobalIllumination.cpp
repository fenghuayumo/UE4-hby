//#include "LightRendering.h"
//#include "RendererModule.h"
//#include "DeferredShadingRenderer.h"
//#include "LightPropagationVolume.h"
//#include "ScenePrivate.h"
//#include "PostProcess/SceneFilterRendering.h"
//#include "PipelineStateCache.h"
//#include "ClearQuad.h"
//#include "Engine/SubsurfaceProfile.h"
//#include "ShowFlags.h"
//#include "VisualizeTexture.h"
//#include "RayTracing/RaytracingOptions.h"
//#include "SceneTextureParameters.h"
//#include "HairStrands/HairStrandsRendering.h"
//#include "ScreenPass.h"
//#include "SkyAtmosphereRendering.h"
//#include "RayTracingDefinitions.h"
//#include "RayTracingDeferredMaterials.h"
//#include "RayTracingTypes.h"
//#include "PathTracingDefinitions.h"
//#include "PathTracing.h"
//
//#include "Modules/ModuleManager.h"
//#include "Misc/MessageDialog.h"
//
//#if RHI_RAYTRACING
//#include "RayTracing/RayTracingLighting.h"
//
//static TAutoConsoleVariable<int32> CVarRestirGIDenoiser(
//	TEXT("r.RayTracing.RestirGI.Denoiser"),
//	2,
//	TEXT("Choose the denoising algorithm.\n")
//	TEXT(" 0: Disabled ;\n")
//	TEXT(" 1: Forces the default denoiser of the renderer;\n")
//	TEXT(" 2: GScreenSpaceDenoiser whitch may be overriden by a third party plugin. This needs the NRD denoiser plugin to work correctly (default)\n"),
//	ECVF_RenderThreadSafe);
//
//
//static TAutoConsoleVariable<int32> CVarRestirGICompositeDiffuse(
//	TEXT("r.RayTracing.RestirGI.CompositeDiffuse"), 1,
//	TEXT("Whether to composite the diffuse signal"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGICompositeMode(
//	TEXT("r.RayTracing.RestirGI.CompositeMode"), 0,
//	TEXT("How to composite the signal (add = 0, replace = 1)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGISpatial(
//	TEXT("r.RayTracing.RestirGI.Spatial"), 1,
//	TEXT("Whether to apply spatial resmapling"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIInitialCandidates(
//	TEXT("r.RayTracing.RestirGI.InitialSamples"), 4,
//	TEXT("How many lights to test sample during the initial candidate search"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIInitialCandidatesBoost(
//	TEXT("r.RayTracing.RestirGI.InitialSamplesBoost"), 32,
//	TEXT("How many lights to test sample during the initial candidate search when history is invalidated"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGITemporal(
//	TEXT("r.RayTracing.RestirGI.Temporal"), 1,
//	TEXT("Whether to use temporal resampling for the reserviors"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIApplyBoilingFilter(
//	TEXT("r.RayTracing.RestirGI.ApplyBoilingFilter"), 1,
//	TEXT("Whether to apply boiling filter when temporally resampling"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGIBoilingFilterStrength(
//	TEXT("r.RayTracing.RestirGI.BoilingFilterStrength"), 0.20f,
//	TEXT("Strength of Boiling filter"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGISpatialSamplingRadius(
//	TEXT("r.RayTracing.RestirGI.Spatial.SamplingRadius"), 32.0f,
//	TEXT("Spatial radius for sampling in pixels (Default 32.0)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGISpatialSamples(
//	TEXT("r.RayTracing.RestirGI.Spatial.Samples"), 1,
//	TEXT("Spatial samples per pixel"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGISpatialSamplesBoost(
//	TEXT("r.RayTracing.RestirGI.Spatial.SamplesBoost"), 8,
//	TEXT("Spatial samples per pixel when invalid history is detected"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGISpatialNormalRejectionThreshold(
//	TEXT("r.RayTracing.RestirGI.Spatial.NormalRejectionThreshold"), 0.5f,
//	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGISpatialDepthRejectionThreshold(
//	TEXT("r.RayTracing.RestirGI.Spatial.DepthRejectionThreshold"), 0.1f,
//	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGISpatialApplyApproxVisibility(
//	TEXT("r.RayTracing.RestirGI.Spatial.ApplyApproxVisibility"), 0,
//	TEXT("Apply an approximate visibility test on sample selected during spatial sampling"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGITemporalMaxHistory(
//	TEXT("r.RayTracing.RestirGI.Temporal.MaxHistory"), 10,
//	TEXT("Maximum temporal history for samples (default 10)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGITemporalNormalRejectionThreshold(
//	TEXT("r.RayTracing.RestirGI.Temporal.NormalRejectionThreshold"), 0.5f,
//	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<float> CVarRestirGITemporalDepthRejectionThreshold(
//	TEXT("r.RayTracing.RestirGI.Temporal.DepthRejectionThreshold"), 0.1f,
//	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGITemporalApplyApproxVisibility(
//	TEXT("r.RayTracing.RestirGI.Temporal.ApplyApproxVisibility"), 0,
//	TEXT("Apply an approximate visibility test on sample selected during reprojection"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIDemodulateMaterials(
//	TEXT("r.RayTracing.RestirGI.DemodulateMaterials"), 1,
//	TEXT("Whether to demodulate the material contributiuon from the signal for denoising"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIFaceCull(
//	TEXT("r.RayTracing.RestirGI.FaceCull"), 0,
//	TEXT("Face culling to use for visibility tests\n")
//	TEXT("  0 - none (Default)\n")
//	TEXT("  1 - front faces (equivalent to backface culling in shadow maps)\n")
//	TEXT("  2 - back faces"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIApproximateVisibilityMode(
//	TEXT("r.RayTracing.RestirGI.ApproximateVisibilityMode"), 0,
//	TEXT("Visibility mode for approximate visibility tests (default 0/accurate)\n")
//	TEXT("  0 - Accurate, any hit shaders process alpha coverage\n")
//	TEXT("  1 - Force opaque, anyhit shaders ignored, alpha coverage considered 100%\n")
//	TEXT("  2 - Force transparent, anyhit shaders ignored, alpha coverage considered 0%"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGINumReservoirs(
//	TEXT("r.RayTracing.RestirGI.NumReservoirs"), -1,
//	TEXT("Number of independent light reservoirs per pixel\n")
//	TEXT("  1-N - Explicit number of reservoirs\n")
//	TEXT("  -1 - Auto-select based on subsampling (default)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIMinReservoirs(
//	TEXT("r.RayTracing.RestirGI.MinReservoirs"), 1,
//	TEXT("Minimum number of light reservoirs when auto-seleting(default 1)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIMaxReservoirs(
//	TEXT("r.RayTracing.RestirGI.MaxReservoirs"), 2,
//	TEXT("Maximum number of light reservoirs when auto-seleting (default 2)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRestirGIFusedSampling(
//	TEXT("r.RayTracing.RestirGI.FusedSampling"), 1,
//	TEXT("Whether to fuse initial candidate and temporal sampling (default 0)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRayTracingRestirGIDebugMode(
//	TEXT("r.RayTracing.RestirGI.DebugMode"),
//	0,
//	TEXT("Debug visualization mode (default = 0)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRayTracingRestirGIFeedbackVisibility(
//	TEXT("r.RayTracing.RestirGI.FeedbackVisibility"),
//	1,
//	TEXT("Whether to feedback the final visibility result to the history (default = 1)"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRayTracingRestirGITestInitialVisibility(
//	TEXT("r.RayTracing.RestirGI.TestInitialVisibility"),
//	1,
//	TEXT("Test initial samples for visibility (default = 1)\n")
//	TEXT("  0 - Do not test visibility during inital sampling\n")
//	TEXT("  1 - Test visibility on final merged reservoir  (default)\n")
//	TEXT("  2 - Test visibility on reservoirs prior to merging\n"),
//	ECVF_RenderThreadSafe);
//
//static TAutoConsoleVariable<int32> CVarRayTracingRestirGIEnableHairVoxel(
//	TEXT("r.RayTracing.RestirGI.EnableHairVoxel"),
//	1,
//	TEXT("Whether to test hair voxels for visibility when evaluating (default = 1)\n"),
//	ECVF_RenderThreadSafe);
//
////
//// CVars controlling shader permutations
////
//static TAutoConsoleVariable<int32> CVarRayTracingRestirGIEvaluationMode(
//	TEXT("r.RayTracing.RestirGI.Permute.EvaluationMode"),
//	1,
//	TEXT("Method for computing the light estimate used for driving sampling\n")
//	TEXT("  0 - Use standard integrated lighting via the GetDynamicLightingSplit function, similar to raster\n")
//	TEXT("  1 - Use sampled lighting like the path tracer (default)"),
//	ECVF_RenderThreadSafe);
//
//
//struct FRestirGIPresets
//{
//	int32 CorrectionMode;
//	int32 SpatialSamples;
//	int32 InitialSamples;
//	int32 DisocclusionSamples;
//};
//
//static const FRestirGIPresets RestirGIPresets[] =
//{
//	{ 0, 1, 4, 8},
//	{ 1, 1, 4, 16},
//	{ 1, 4, 8, 16}
//};
//
////static FAutoConsoleCommand GSendRemoteTalkersToEndpointCommand(
////	TEXT("r.RayTracing.RestirGI.Preset"),
////	TEXT("Command applies preset quality levels for restir GI\n")
////	TEXT("  Available levels: medium, high, ultra"),
////	FConsoleCommandWithArgsDelegate::CreateStatic(
////		[](const TArray< FString >& Args)
////{
////	int32 QualityLevel = -1;
////	if (Args.Num() == 1)
////	{
////		if (Args[0] == TEXT("medium"))
////		{
////			QualityLevel = 0;
////		}
////		if (Args[0] == TEXT("high"))
////		{
////			QualityLevel = 1;
////		}
////		if (Args[0] == TEXT("ultra"))
////		{
////			QualityLevel = 2;
////		}
////	}
////
////	if (QualityLevel == -1)
////	{
////		UE_LOG(LogRenderer, Display, TEXT("Invalid arguments for setting sampled lighting presets (options: medium, high, ultra)"));
////	}
////	else
////	{
////		check(QualityLevel >= 0 && QualityLevel < (sizeof(RestirGIPresets) / sizeof(RestirGIPresets[0])));
////
////		const FRestirGIPresets& Presets = RestirGIPresets[QualityLevel];
////
////		// Correction mode / approximate visibility shared for temporal/spatial
////		CVarRestirGITemporalApplyApproxVisibility.AsVariable()->Set(Presets.CorrectionMode, ECVF_SetByConsole);
////		CVarRestirGISpatialApplyApproxVisibility.AsVariable()->Set(Presets.CorrectionMode, ECVF_SetByConsole);
////
////		// spatial sample count
////		CVarRestirGISpatialSamples.AsVariable()->Set(Presets.SpatialSamples, ECVF_SetByConsole);
////
////		// boosted spatial count
////		CVarRestirGISpatialSamplesBoost.AsVariable()->Set(Presets.DisocclusionSamples, ECVF_SetByConsole);
////
////		// initial sample count
////		CVarRestirGIInitialCandidates.AsVariable()->Set(Presets.InitialSamples, ECVF_SetByConsole);
////	}
////})
////);
//
//struct RTXGI_PackedReservoir
//{
//	// Internal compressed GI sample data
//	FIntVector4 VisPos;
//	FIntVector4 SamPos;
//
//	FIntVector4 	data1;
//	FIntVector4   data2;
//};
//
//BEGIN_SHADER_PARAMETER_STRUCT(FRestirGICommonParameters, )
//	SHADER_PARAMETER(float, MaxNormalBias)
//	SHADER_PARAMETER(int32, VisibilityApproximateTestMode)
//	SHADER_PARAMETER(int32, VisibilityFaceCull)
//	SHADER_PARAMETER(int32, SupportTranslucency)
//	SHADER_PARAMETER(int32, InexactShadows)
//	SHADER_PARAMETER(float, MaxBiasForInexactGeometry)
//	SHADER_PARAMETER(int32, MaxTemporalHistory)
//	SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
//	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<RTXGI_PackedReservoir>, RWLightReservoirUAV)
//	SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
//END_SHADER_PARAMETER_STRUCT()
//
//static void ApplyRestirGIGlobalSettings(FShaderCompilerEnvironment& OutEnvironment)
//{
//	OutEnvironment.SetDefine(TEXT("RTXGI_INTEGRATION_VERSION"), 4270);
//
//	OutEnvironment.SetDefine(TEXT("LIGHT_ESTIMATION_MODE"), 1);
//	OutEnvironment.SetDefine(TEXT("USE_ALTERNATE_RNG"), 0);
//	OutEnvironment.SetDefine(TEXT("USE_LDS_FOR_SPATIAL_RESAMPLE"), 1);
//}
//
//class FRestirGIInitialSamplesRGS : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FRestirGIInitialSamplesRGS)
//	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGIInitialSamplesRGS, FGlobalShader)
//
//	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");
//
//	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim>;
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
//	{
//		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
//		ApplyRestirGIGlobalSettings(OutEnvironment);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//
//		SHADER_PARAMETER(int32, OutputSlice)
//		SHADER_PARAMETER(int32, HistoryReservoir)
//		SHADER_PARAMETER(int32, InitialCandidates)
//		SHADER_PARAMETER(int32, InitialSampleVisibility)
//		
//		SHADER_PARAMETER(float, DiffuseThreshold)
//		SHADER_PARAMETER(uint32, EvalSkyLight)
//		SHADER_PARAMETER(uint32, UseRussianRoulette)
//		SHADER_PARAMETER(float, MaxRayDistanceForGI)
//		SHADER_PARAMETER(float, MaxShadowDistance)
//		SHADER_PARAMETER(float, NextEventEstimationSamples)
//
//		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
//
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
//
//		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)
//
//
//	END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FRestirGIInitialSamplesRGS, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "GenerateInitialSamplesRGS", SF_RayGen);
//
//class FEvaluateRestirGIRGS : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FEvaluateRestirGIRGS)
//	SHADER_USE_ROOT_PARAMETER_STRUCT(FEvaluateRestirGIRGS, FGlobalShader)
//
//	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");
//	class FHairLightingDim : SHADER_PERMUTATION_BOOL("USE_HAIR_LIGHTING");
//
//	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim, FHairLightingDim>;
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
//	{
//		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
//		ApplyRestirGIGlobalSettings(OutEnvironment);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//
//		SHADER_PARAMETER(int32, InputSlice)
//		SHADER_PARAMETER(int32, NumReservoirs)
//		SHADER_PARAMETER(int32, DemodulateMaterials)
//		SHADER_PARAMETER(int32, DebugOutput)
//		SHADER_PARAMETER(int32, FeedbackVisibility)
//		SHADER_PARAMETER(uint32, bUseHairVoxel)
//		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
//
//		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseUAV)
//		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWRayDistanceUAV)
//		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
//		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<RTXGI_PackedReservoir>, RWGIReservoirHistoryUAV)
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCategorizationTexture)
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairLightChannelMaskTexture)
//
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
//		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)
//
//		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)
//
//		END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FEvaluateRestirGIRGS, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "EvaluateRestirGILightingRGS", SF_RayGen);
//
//class FRestirGISpatialResampling : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FRestirGISpatialResampling)
//	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGISpatialResampling, FGlobalShader)
//
//	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");
//
//	using FPermutationDomain = TShaderPermutationDomain<FEvaluateLightingDim>;
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
//	{
//		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
//		ApplyRestirGIGlobalSettings(OutEnvironment);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//
//		SHADER_PARAMETER(int32, InputSlice)
//		SHADER_PARAMETER(int32, OutputSlice)
//		SHADER_PARAMETER(int32, HistoryReservoir)
//		SHADER_PARAMETER(float, SpatialSamplingRadius)
//		SHADER_PARAMETER(int32, SpatialSamples)
//		SHADER_PARAMETER(int32, SpatialSamplesBoost)
//		SHADER_PARAMETER(float, SpatialDepthRejectionThreshold)
//		SHADER_PARAMETER(float, SpatialNormalRejectionThreshold)
//		SHADER_PARAMETER(int32, ApplyApproximateVisibilityTest)
//		SHADER_PARAMETER(uint32, NeighborOffsetMask)
//		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
//
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
//		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)
//
//		SHADER_PARAMETER_SRV(Buffer<float2>, NeighborOffsets)
//
//		END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FRestirGISpatialResampling, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "ApplySpatialResamplingRGS", SF_RayGen);
//
//
//class FRestirGITemporalResampling : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FRestirGITemporalResampling)
//	SHADER_USE_ROOT_PARAMETER_STRUCT(FRestirGITemporalResampling, FGlobalShader)
//
//	class FFuseInitialSamplingDim : SHADER_PERMUTATION_BOOL("FUSE_TEMPORAL_AND_INITIAL_SAMPLING");
//
//	class FEvaluateLightingDim : SHADER_PERMUTATION_BOOL("EVALUATE_LIGHTING_SAMPLED");
//
//	using FPermutationDomain = TShaderPermutationDomain<FFuseInitialSamplingDim,FEvaluateLightingDim>;
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
//	{
//		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
//		ApplyRestirGIGlobalSettings(OutEnvironment);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//
//		SHADER_PARAMETER(int32, InputSlice)
//		SHADER_PARAMETER(int32, OutputSlice)
//		SHADER_PARAMETER(int32, HistoryReservoir)
//		SHADER_PARAMETER(float, TemporalDepthRejectionThreshold)
//		SHADER_PARAMETER(float, TemporalNormalRejectionThreshold)
//		SHADER_PARAMETER(int32, ApplyApproximateVisibilityTest)
//		SHADER_PARAMETER(int32, InitialCandidates)
//		SHADER_PARAMETER(int32, InitialSampleVisibility)
//
//		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
//
//		SHADER_PARAMETER(FIntVector, ReservoirHistoryBufferDim)
//		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<RTXGI_PackedReservoir>, GIReservoirHistory)
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthHistory)
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, NormalHistory)
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
//		SHADER_PARAMETER_STRUCT_INCLUDE(FRestirGICommonParameters, RestirGICommonParameters)
//
//		END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FRestirGITemporalResampling, "/Engine/Private/RestirGI/RayTracingRestirGILighting.usf", "ApplyTemporalResamplingRGS", SF_RayGen);
//
//class FRestirGIBoilingFilterCS : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FRestirGIBoilingFilterCS)
//	SHADER_USE_PARAMETER_STRUCT(FRestirGIBoilingFilterCS, FGlobalShader)
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
//	{
//		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
//		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
//		ApplyRestirGIGlobalSettings(OutEnvironment);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//
//		SHADER_PARAMETER(int32, InputSlice)
//		SHADER_PARAMETER(int32, OutputSlice)
//		SHADER_PARAMETER(float, BoilingFilterStrength)
//
//		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWGIReservoirUAV)
//		SHADER_PARAMETER(FIntVector, ReservoirBufferDim)
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//
//	END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FRestirGIBoilingFilterCS, "/Engine/Private/RestirGI/BoilingFilter.usf", "BoilingFilterCS", SF_Compute);
//
//class FCompositeRestirGIPS : public FGlobalShader
//{
//	DECLARE_GLOBAL_SHADER(FCompositeRestirGIPS);
//	SHADER_USE_PARAMETER_STRUCT(FCompositeRestirGIPS, FGlobalShader);
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
//	}
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Diffuse)
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Specular)
//		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
//		SHADER_PARAMETER(int, ApplyDiffuse)
//		SHADER_PARAMETER(int, ApplySpecular)
//		SHADER_PARAMETER(int32, ModulateMaterials)
//		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
//		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//		RENDER_TARGET_BINDING_SLOTS()
//	END_SHADER_PARAMETER_STRUCT()
//};
//
//IMPLEMENT_GLOBAL_SHADER(FCompositeRestirGIPS, "/Engine/Private/RestirGI/CompositeRestirGIPS.usf", "CompositeRestirGIPS", SF_Pixel);
//
///**
// * This buffer provides a table with a low discrepency sequence
// */
//class FRestirGIDiscSampleBuffer : public FRenderResource
//{
//public:
//
//	/** The vertex buffer used for storage. */
//	FVertexBufferRHIRef DiscSampleBufferRHI;
//	/** Shader resource view in to the vertex buffer. */
//	FShaderResourceViewRHIRef DiscSampleBufferSRV;
//
//	const uint32 NumSamples = 8192;
//
//	/**
//	 * Initialize RHI resources.
//	 */
//	virtual void InitRHI() override
//	{
//		if (RHISupportsRayTracingShaders(GShaderPlatformForFeatureLevel[GetFeatureLevel()]))
//		{
//			// Create a sequence of low-discrepancy samples within a unit radius around the origin
//			// for "randomly" sampling neighbors during spatial resampling
//			TResourceArray<uint8> Buffer;
//
//			Buffer.AddZeroed(NumSamples * 2);
//
//			const int32 R = 250;
//			const float phi2 = 1.0f / 1.3247179572447f;
//			uint32 num = 0;
//			float U = 0.5f;
//			float V = 0.5f;
//			while (num < NumSamples * 2) {
//				U += phi2;
//				V += phi2 * phi2;
//				if (U >= 1.0f) U -= 1.0f;
//				if (V >= 1.0f) V -= 1.0f;
//
//				float rSq = (U - 0.5f) * (U - 0.5f) + (V - 0.5f) * (V - 0.5f);
//				if (rSq > 0.25f)
//					continue;
//
//				Buffer[num++] = uint8((U - 0.5f) * R + 127.5f);
//				Buffer[num++] = uint8((V - 0.5f) * R + 127.5f);
//				
//			}
//
//			FRHIResourceCreateInfo CreateInfo(&Buffer);
//			DiscSampleBufferRHI = RHICreateVertexBuffer(
//				/*Size=*/ sizeof(uint8) * 2 * NumSamples,
//				/*Usage=*/ BUF_Volatile | BUF_ShaderResource,
//				CreateInfo);
//			DiscSampleBufferSRV = RHICreateShaderResourceView(
//				DiscSampleBufferRHI, /*Stride=*/ sizeof(uint8) * 2, PF_R8G8
//			);
//		}
//	}
//
//	/**
//	 * Release RHI resources.
//	 */
//	virtual void ReleaseRHI() override
//	{
//		DiscSampleBufferSRV.SafeRelease();
//		DiscSampleBufferRHI.SafeRelease();
//	}
//};
//
///** The global resource for the disc sample buffer. */
//TGlobalResource<FRestirGIDiscSampleBuffer> GRestiGIDiscSampleBuffer;
//
///*
// * Code for handling top-level shader permutations
// */
//struct RestirGIPermutation
//{
//	bool EvaluationMode;
//};
//
//
//void FDeferredShadingSceneRenderer::PrepareRayTracingRestirGI(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
//{
//	// Declare all RayGen shaders that require material closest hit shaders to be bound
//	RestirGIPermutation Options;
//	Options.EvaluationMode = CVarRayTracingRestirGIEvaluationMode.GetValueOnRenderThread() != 0;
//
//
//	{
//		FRestirGIInitialSamplesRGS::FPermutationDomain PermutationVector;
//		PermutationVector.Set<FRestirGIInitialSamplesRGS::FEvaluateLightingDim>(Options.EvaluationMode);
//		auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesRGS>(PermutationVector);
//		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
//	}
//	{
//		FRestirGISpatialResampling::FPermutationDomain PermutationVector;
//		PermutationVector.Set<FRestirGISpatialResampling::FEvaluateLightingDim>(Options.EvaluationMode);
//		auto RayGenShader = View.ShaderMap->GetShader<FRestirGISpatialResampling>(PermutationVector);
//		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
//	}
//
//
//	for (int32 Permutation = 0; Permutation < 2; Permutation++)
//	{
//		FRestirGITemporalResampling::FPermutationDomain PermutationVector;
//
//		PermutationVector.Set<FRestirGITemporalResampling::FFuseInitialSamplingDim>(Permutation != 0);
//
//		PermutationVector.Set<FRestirGITemporalResampling::FEvaluateLightingDim>(Options.EvaluationMode);
//		auto RayGenShader = View.ShaderMap->GetShader<FRestirGITemporalResampling>(PermutationVector);
//		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
//	}
//
//	for (int32 Permutation = 0; Permutation < 2; Permutation++)
//	{
//		FEvaluateRestirGIRGS::FPermutationDomain PermutationVector;
//
//		PermutationVector.Set<FEvaluateRestirGIRGS::FHairLightingDim>(Permutation != 0);
//
//		PermutationVector.Set<FEvaluateRestirGIRGS::FEvaluateLightingDim>(Options.EvaluationMode);
//		auto RayGenShader = View.ShaderMap->GetShader<FEvaluateRestirGIRGS>(PermutationVector);
//
//		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
//	}
//}
//
//
//void FDeferredShadingSceneRenderer::RenderRestirGI(
//	FRDGBuilder& GraphBuilder,
//	FSceneTextureParameters& SceneTextures,
//	FViewInfo& View,
//	const IScreenSpaceDenoiser::FAmbientOcclusionRayTracingConfig& RayTracingConfig,
//	int32 UpscaleFactor,
//	// Output
//	IScreenSpaceDenoiser::FDiffuseIndirectInputs* OutDenoiserInputs)
//{
//	RDG_EVENT_SCOPE(GraphBuilder, "RestirGI");
//
//	// Intermediate lighting targets
//	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
//		SceneTextures.SceneDepthTexture->Desc.Extent,
//		PF_FloatRGBA,
//		FClearValueBinding::None,
//		TexCreate_ShaderResource | TexCreate_UAV);
//
//	FRDGTextureRef Diffuse = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIDiffuse"));
//
//	Desc.Format = PF_G16R16F;
//	FRDGTextureRef RayHitDistance = GraphBuilder.CreateTexture(Desc, TEXT("SampledGIHitDistance"));
//
//	const int32 RequestedReservoirs = CVarRestirGINumReservoirs.GetValueOnAnyThread();
//	const int32 MinReservoirs = FMath::Max(CVarRestirGIMinReservoirs.GetValueOnAnyThread(), 1);
//	const int32 MaxReservoirs = FMath::Max(CVarRestirGIMaxReservoirs.GetValueOnAnyThread(), 1);
//	const bool SubsampledView = View.GetSecondaryViewRectSize() != View.ViewRect.Size();
//	const int32 AutoReservoirs = SubsampledView ? MaxReservoirs : MinReservoirs;
//	const int32 NumReservoirs = RequestedReservoirs < 0 ? AutoReservoirs : FMath::Max(RequestedReservoirs, 1);
//	FIntPoint PaddedSize = FMath::DivideAndRoundUp<FIntPoint>(SceneTextures.SceneDepthTexture->Desc.Extent, 4) * 4;
//
//	FIntVector ReservoirBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs + 1);
//	FRDGBufferDesc ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(16, ReservoirBufferDim.X * ReservoirBufferDim.Y * ReservoirBufferDim.Z);
//
//	FRDGBufferRef GIReservoirs = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("GIReservoirs"));
//
//	
//	FIntVector ReservoirHistoryBufferDim = FIntVector(PaddedSize.X, PaddedSize.Y, NumReservoirs);
//	FRDGBufferDesc ReservoirHistoryDesc = FRDGBufferDesc::CreateStructuredDesc(16, ReservoirHistoryBufferDim.X * ReservoirHistoryBufferDim.Y * ReservoirHistoryBufferDim.Z);
//	FRDGBufferRef GIReservoirsHistory = GraphBuilder.CreateBuffer(ReservoirHistoryDesc, TEXT("GIReservoirsHistory"));
//
//	{
//		FIntPoint LightingResolution = View.ViewRect.Size();
//
//		{
//			// detect camera cuts/history invalidation and boost initial/spatial samples to compensate
//			const bool bCameraCut = !View.PrevViewInfo.SampledGIHistory.GIReservoirs.IsValid() || View.bCameraCut;
//			const int32 PrevHistoryCount = View.PrevViewInfo.SampledGIHistory.ReservoirDimensions.Z;
//
//			// Global permutation options
//			//const RestirGIPermutation Options = GetPermutationOptions();
//
//			const int32 InitialCandidates = bCameraCut ? CVarRestirGIInitialCandidatesBoost.GetValueOnRenderThread() : CVarRestirGIInitialCandidates.GetValueOnRenderThread();
//
//			int32 InitialSlice = 0;
//			const bool bEnableFusedSampling = CVarRestirGIFusedSampling.GetValueOnRenderThread() != 0;
//
//			static auto CVarSupportTranslucency = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadows.SupportTranslucency"));
//			static auto CVarMaxInexactBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadow.MaxBiasForInexactGeometry"));
//			static auto CVarEnableInexactBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Shadow.UseBiasForSkipWPOEval"));
//
//			// Parameters shared by ray tracing passes
//			FRestirGICommonParameters CommonParameters;
//			CommonParameters.MaxNormalBias = GetRaytracingMaxNormalBias();
//			CommonParameters.TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
//			CommonParameters.RWLightReservoirUAV = GraphBuilder.CreateUAV(GIReservoirs);
//			CommonParameters.ReservoirBufferDim = ReservoirBufferDim;
//			CommonParameters.VisibilityApproximateTestMode = CVarRestirGIApproximateVisibilityMode.GetValueOnRenderThread();
//			CommonParameters.VisibilityFaceCull = CVarRestirGIFaceCull.GetValueOnRenderThread();
//			CommonParameters.SupportTranslucency = CVarSupportTranslucency ? CVarSupportTranslucency->GetInt() : 0;
//			CommonParameters.InexactShadows = CVarEnableInexactBias ? CVarEnableInexactBias->GetInt() : 0;
//			CommonParameters.MaxBiasForInexactGeometry = CVarMaxInexactBias ? CVarMaxInexactBias->GetFloat() : 0.0f;
//			CommonParameters.MaxTemporalHistory = FMath::Max(1, CVarRestirGITemporalMaxHistory.GetValueOnRenderThread());
//
//			for (int32 Reservoir = 0; Reservoir < NumReservoirs; Reservoir++)
//			{
//				const bool bUseFusedSampling = CVarRestirGITemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount && bEnableFusedSampling;
//
//				// Initial sampling pass to select a light candidate
//				//if (!bUseFusedSampling)
//				{
//					FRestirGIInitialSamplesRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIInitialSamplesRGS::FParameters>();
//
//					PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//					PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
//					PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
//
//					PassParameters->MaxRayDistanceForGI = 1e27;
//					PassParameters->MaxShadowDistance = -1;
//					PassParameters->EvalSkyLight = 0;
//					PassParameters->UseRussianRoulette = 0;
//					PassParameters->DiffuseThreshold = 0.01;
//					PassParameters->NextEventEstimationSamples = 2;
//					PassParameters->OutputSlice = Reservoir;
//					PassParameters->HistoryReservoir = Reservoir;
//					PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
//					PassParameters->InitialSampleVisibility = CVarRayTracingRestirGITestInitialVisibility.GetValueOnRenderThread();
//
//					PassParameters->RestirGICommonParameters = CommonParameters;
//
//					auto RayGenShader = View.ShaderMap->GetShader<FRestirGIInitialSamplesRGS>();
//
//					ClearUnusedGraphResources(RayGenShader, PassParameters);
//
//					GraphBuilder.AddPass(
//						RDG_EVENT_NAME("CreateInitialSamples"),
//						PassParameters,
//						ERDGPassFlags::Compute,
//						[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
//					{
//						FRayTracingShaderBindingsWriter GlobalResources;
//						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
//
//						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
//						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
//					});
//				}
//
//				// Temporal candidate merge pass, optionally merged with initial candidate pass
//				if (CVarRestirGITemporal.GetValueOnRenderThread() != 0 && !bCameraCut && Reservoir < PrevHistoryCount)
//				{
//					{
//						FRestirGITemporalResampling::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGITemporalResampling::FParameters>();
//
//						PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//						PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
//						PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
//
//						PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
//						PassParameters->InputSlice = Reservoir;
//						PassParameters->OutputSlice = Reservoir;
//						PassParameters->HistoryReservoir = Reservoir;
//						PassParameters->TemporalDepthRejectionThreshold = FMath::Clamp(CVarRestirGITemporalDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
//						PassParameters->TemporalNormalRejectionThreshold = FMath::Clamp(CVarRestirGITemporalNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
//						PassParameters->ApplyApproximateVisibilityTest = CVarRestirGITemporalApplyApproxVisibility.GetValueOnAnyThread();
//						PassParameters->InitialCandidates = FMath::Max(1, InitialCandidates);
//						PassParameters->InitialSampleVisibility = CVarRayTracingRestirGITestInitialVisibility.GetValueOnRenderThread();
//
//						PassParameters->GIReservoirHistory = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(View.PrevViewInfo.SampledGIHistory.GIReservoirs));
//						PassParameters->NormalHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.GBufferA, GSystemTextures.BlackDummy);
//						PassParameters->DepthHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.DepthBuffer, GSystemTextures.BlackDummy);
//
//						PassParameters->RestirGICommonParameters = CommonParameters;
//
//						FRestirGITemporalResampling::FPermutationDomain PermutationVector;
//						PermutationVector.Set<FRestirGITemporalResampling::FFuseInitialSamplingDim>(bEnableFusedSampling);
//
//						auto RayGenShader = View.ShaderMap->GetShader<FRestirGITemporalResampling>(PermutationVector);
//						//auto RayGenShader = GetShaderPermutation<FRestirGITemporalResampling>(PermutationVector,Options, View);
//
//						ClearUnusedGraphResources(RayGenShader, PassParameters);
//
//						GraphBuilder.AddPass(
//							RDG_EVENT_NAME("%sTemporalResample", bEnableFusedSampling ? TEXT("FusedInitialCandidateAnd") : TEXT("")),
//							PassParameters,
//							ERDGPassFlags::Compute,
//							[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
//						{
//							FRayTracingShaderBindingsWriter GlobalResources;
//							SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
//
//							FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
//							RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
//
//						});
//					}
//
//					// Boiling filter pass to prevent runaway samples
//					if (CVarRestirGIApplyBoilingFilter.GetValueOnRenderThread() != 0)
//					{
//						FRestirGIBoilingFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGIBoilingFilterCS::FParameters>();
//
//						PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//
//						PassParameters->RWGIReservoirUAV = GraphBuilder.CreateUAV(GIReservoirs);
//						PassParameters->ReservoirBufferDim = ReservoirBufferDim;
//						PassParameters->InputSlice = Reservoir;
//						PassParameters->OutputSlice = Reservoir;
//						PassParameters->BoilingFilterStrength = FMath::Clamp(CVarRestirGIBoilingFilterStrength.GetValueOnRenderThread(), 0.00001f, 1.0f);
//
//						auto ComputeShader = View.ShaderMap->GetShader<FRestirGIBoilingFilterCS>();
//
//						ClearUnusedGraphResources(ComputeShader, PassParameters);
//						FIntPoint GridSize = FMath::DivideAndRoundUp<FIntPoint>(View.ViewRect.Size(), 16);
//
//						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoilingFilter"), ComputeShader, PassParameters, FIntVector(GridSize.X, GridSize.Y, 1));
//					}
//				}
//			}
//
//			// Spatial resampling passes, one per reservoir
//			for (int32 Reservoir = NumReservoirs; Reservoir > 0; Reservoir--)
//			{
//				if (CVarRestirGISpatial.GetValueOnRenderThread() != 0)
//				{
//					FRestirGISpatialResampling::FParameters* PassParameters = GraphBuilder.AllocParameters<FRestirGISpatialResampling::FParameters>();
//
//					PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//					PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
//					PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
//
//					PassParameters->InputSlice = Reservoir - 1;
//					PassParameters->OutputSlice = Reservoir;
//					PassParameters->HistoryReservoir = Reservoir - 1;
//					PassParameters->SpatialSamples = FMath::Max(CVarRestirGISpatialSamples.GetValueOnRenderThread(), 1);
//					PassParameters->SpatialSamplesBoost = FMath::Max(CVarRestirGISpatialSamplesBoost.GetValueOnRenderThread(), 1);
//					PassParameters->SpatialSamplingRadius = FMath::Max(1.0f, CVarRestirGISpatialSamplingRadius.GetValueOnRenderThread());
//					PassParameters->SpatialDepthRejectionThreshold = FMath::Clamp(CVarRestirGISpatialDepthRejectionThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
//					PassParameters->SpatialNormalRejectionThreshold = FMath::Clamp(CVarRestirGISpatialNormalRejectionThreshold.GetValueOnRenderThread(), -1.0f, 1.0f);
//					PassParameters->ApplyApproximateVisibilityTest = CVarRestirGISpatialApplyApproxVisibility.GetValueOnRenderThread();
//
//					PassParameters->NeighborOffsetMask = GRestiGIDiscSampleBuffer.NumSamples - 1;
//					PassParameters->NeighborOffsets = GRestiGIDiscSampleBuffer.DiscSampleBufferSRV;
//
//					PassParameters->RestirGICommonParameters = CommonParameters;
//
//					auto RayGenShader = View.ShaderMap->GetShader<FRestirGISpatialResampling>();
//					//auto RayGenShader = GetShaderPermutation<FRestirGISpatialResampling>(Options, View);
//
//					ClearUnusedGraphResources(RayGenShader, PassParameters);
//
//					GraphBuilder.AddPass(
//						RDG_EVENT_NAME("SpatialResample"),
//						PassParameters,
//						ERDGPassFlags::Compute,
//						[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
//					{
//						FRayTracingShaderBindingsWriter GlobalResources;
//						SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
//
//						FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
//						RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
//
//					});
//					InitialSlice = Reservoir;
//				}
//			}
//
//			// Shading evaluation pass
//			{
//				const bool bUseHairLighting = false;
//				FEvaluateRestirGIRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FEvaluateRestirGIRGS::FParameters>();
//
//				PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
//				PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder); //SceneTextures;
//				PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
//
//				PassParameters->RWDiffuseUAV = GraphBuilder.CreateUAV(Diffuse);
//				PassParameters->RWRayDistanceUAV = GraphBuilder.CreateUAV(RayHitDistance);
//				PassParameters->ReservoirHistoryBufferDim = ReservoirHistoryBufferDim;
//				PassParameters->RWGIReservoirHistoryUAV = GraphBuilder.CreateUAV(GIReservoirsHistory);
//				PassParameters->InputSlice = InitialSlice;
//				PassParameters->NumReservoirs = NumReservoirs;
//				PassParameters->DemodulateMaterials = CVarRestirGIDemodulateMaterials.GetValueOnRenderThread();
//				PassParameters->DebugOutput = CVarRayTracingRestirGIDebugMode.GetValueOnRenderThread();
//				PassParameters->FeedbackVisibility = CVarRayTracingRestirGIFeedbackVisibility.GetValueOnRenderThread();
//
//				PassParameters->RestirGICommonParameters = CommonParameters;
//
//				FEvaluateRestirGIRGS::FPermutationDomain PermutationVector;
//				PermutationVector.Set<FEvaluateRestirGIRGS::FHairLightingDim>(bUseHairLighting);
//				//auto RayGenShader = GetShaderPermutation<FEvaluateRestirGIRGS>(PermutationVector, Options, View);
//				auto RayGenShader = View.ShaderMap->GetShader<FEvaluateRestirGIRGS>();
//				ClearUnusedGraphResources(RayGenShader, PassParameters);
//
//				GraphBuilder.AddPass(
//					RDG_EVENT_NAME("ShadeSamples"),
//					PassParameters,
//					ERDGPassFlags::Compute,
//					[PassParameters, this, &View, RayGenShader, LightingResolution](FRHICommandList& RHICmdList)
//				{
//					FRayTracingShaderBindingsWriter GlobalResources;
//					SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
//
//					FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
//					RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, LightingResolution.X, LightingResolution.Y);
//				});
//			}
//		}
//
//		// composite
//		/*{
//			const FScreenPassRenderTarget Output(SceneColorTexture, View.ViewRect, ERenderTargetLoadAction::ELoad);
//
//			const FScreenPassTexture SceneColor(Diffuse, View.ViewRect);
//
//			TShaderMapRef<FScreenPassVS> VertexShader(View.ShaderMap);
//			TShaderMapRef<FCompositeRestirGIPS> PixelShader(View.ShaderMap);
//			const bool bCompositeReplace = CVarRestirGICompositeMode.GetValueOnRenderThread() != 0;
//			FRHIBlendState* BlendState = bCompositeReplace ?
//				TStaticBlendState<CW_RGBA>::GetRHI()  :
//				TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
//			const FScreenPassTextureViewport InputViewport(SceneColorTexture->Desc.Extent, View.ViewRect);
//			const FScreenPassTextureViewport OutputViewport(SceneColorTexture->Desc.Extent, View.ViewRect);
//
//			auto Parameters = GraphBuilder.AllocParameters<FCompositeRestirGIPS::FParameters>();
//
//			Parameters->ApplyDiffuse = CVarRestirGICompositeDiffuse.GetValueOnRenderThread();
//			Parameters->ApplySpecular = CVarRestirGICompositeSpecular.GetValueOnRenderThread();
//			Parameters->ModulateMaterials = CVarRestirGIDemodulateMaterials.GetValueOnRenderThread();
//
//			Parameters->Diffuse = Diffuse;
//			Parameters->Specular = Specular;
//			Parameters->InputSampler = TStaticSamplerState<>::GetRHI();
//
//			Parameters->SceneTextures = SceneTexturesUniformBuffer;
//			Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
//			Parameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
//
//			ClearUnusedGraphResources(PixelShader, Parameters);
//
//			AddDrawScreenPass(
//				GraphBuilder,
//				RDG_EVENT_NAME("CompositeRestirGI"),
//				View,
//				OutputViewport,
//				InputViewport,
//				VertexShader,
//				PixelShader,
//				BlendState,
//				Parameters);
//		}*/
//	}
//
//	if (!View.bStatePrevViewInfoIsReadOnly)
//	{
//		//Extract history feedback here
//		GraphBuilder.QueueBufferExtraction(GIReservoirsHistory, &View.ViewState->PrevFrameViewInfo.SampledLightHistory.LightReservoirs);
//
//		View.ViewState->PrevFrameViewInfo.SampledLightHistory.ReservoirDimensions = ReservoirHistoryBufferDim;
//	}
//
//	//ToDo - revist light buffer lifetimes. Maybe they should be made as explict allocations from the RDG
//}
//
//#endif