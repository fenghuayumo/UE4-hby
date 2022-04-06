#include "FusionDenoiser.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneRenderTargetParameters.h"
#include "ScenePrivate.h"
#include "ClearQuad.h"
#include "PipelineStateCache.h"
#include "SceneTextureParameters.h"
#include "BlueNoise.h"
#include "Halton.h"

static TAutoConsoleVariable<int32> CVarFusionSpatialUseSSAO(
	TEXT("r.Fusion.GIDenoiser.UseSSAO"), 0,
	TEXT("whether use ssao to strength detail default(0)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFusionSpatialEnabled(
	TEXT("r.Fusion.GIDenoiser.Spatial"), 1,
	TEXT("whether use spatial filter."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFusionTemporalEnabled(
	TEXT("r.Fusion.GIDenoiser.Temporal"), 1,
	TEXT("whether use Temporal filter."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarDenoiserTemporalNormalRejectionThreshold(
	TEXT("r.Fusion.GIDenoiser.Temporal.NormalRejectionThreshold"), 0.5f,
	TEXT("Rejection threshold for rejecting samples based on normal differences (default 0.5)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarDenoiserTemporalDepthRejectionThreshold(
	TEXT("r.Fusion.GIDenoiser.Temporal.DepthRejectionThreshold"), 0.1f,
	TEXT("Rejection threshold for rejecting samples based on depth differences (default 0.1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPlaneDistanceRejectionThreshold(
	TEXT("r.Fusion.Temporal.PlaneDistanceRejectionThreshold"), 50.0f,
	TEXT("Rejection threshold for rejecting samples based on plane distance differences (default 50.0)"),
	ECVF_RenderThreadSafe);

enum class ETemporalFilterStage
{
    ResetHistory = 0,
    ReprojectHistory = 1,
    TemporalAccum = 2,
	MAX
};

class FDiffuseInDirectTemporalFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDiffuseInDirectTemporalFilterCS)
	SHADER_USE_PARAMETER_STRUCT(FDiffuseInDirectTemporalFilterCS, FGlobalShader)

    // class FResetHistoryDim : SHADER_PERMUTATION_BOOL("RESET_HISTORY");
    // class FReprojectHistoryDim : SHADER_PERMUTATION_BOOL("REPROJECT_HISTORY");
	// using FPermutationDomain = TShaderPermutationDomain<FResetHistoryDim, FReprojectHistoryDim>;
	class FStageDim : SHADER_PERMUTATION_ENUM_CLASS("DIM_STAGE", ETemporalFilterStage);
    using FPermutationDomain = TShaderPermutationDomain<FStageDim>;

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
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTex)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistoryTex)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VarianceHistoryTex)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWOutputTex)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWHistoryTex)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWVarianceTex)

        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)
         SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ReprojectionTex)

        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalHistory)

		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(FVector4, BufferTexSize)
        SHADER_PARAMETER(float, TemporalNormalRejectionThreshold)
        SHADER_PARAMETER(float, TemporalDepthRejectionThreshold)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FDiffuseInDirectTemporalFilterCS, "/Engine/Private/RestirGI/TemporalFilter.usf", "TemporalFilter", SF_Compute);

enum class EStage
{
    PreConvolution = 0,
    PostFiltering = 1,
    MAX
};

class FDiffuseInDirectSpatialFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDiffuseInDirectSpatialFilterCS)
	SHADER_USE_PARAMETER_STRUCT(FDiffuseInDirectSpatialFilterCS, FGlobalShader)
	class FUseSSAODim : SHADER_PERMUTATION_BOOL("USE_SSAO_STEERING");
	class FStageDim : SHADER_PERMUTATION_ENUM_CLASS("DIM_STAGE", EStage);
	using FPermutationDomain = TShaderPermutationDomain<FUseSSAODim, FStageDim>;

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
	}

	static uint32 GetThreadBlockSize()
	{
		return 8;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSAOTex)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTex)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWFilteredTex)
        
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)

		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
		SHADER_PARAMETER(FVector4, BufferTexSize)
        SHADER_PARAMETER(int, UpscaleFactor)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)


		SHADER_PARAMETER_STRUCT_REF(FHaltonIteration, HaltonIteration)
		SHADER_PARAMETER_STRUCT_REF(FHaltonPrimes, HaltonPrimes)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FDiffuseInDirectSpatialFilterCS, "/Engine/Private/RestirGI/SpatialFilter.usf", "SpatialFilter", SF_Compute);


DECLARE_GPU_STAT_NAMED(FusionDiffuseDenoiser, TEXT("FusionGI Denoiser"));

FFusionDenoiser::FFusionDenoiser(const IScreenSpaceDenoiser* InWrappedDenoiser)
	: WrappedDenoiser(InWrappedDenoiser)
{
	check(WrappedDenoiser);
}

IScreenSpaceDenoiser::EShadowRequirements FFusionDenoiser::GetShadowRequirements(const FViewInfo& View, const FLightSceneInfo& LightSceneInfo, const FShadowRayTracingConfig& RayTracingConfig) const
{
	return WrappedDenoiser->GetShadowRequirements(View, LightSceneInfo, RayTracingConfig);
}

void FFusionDenoiser::DenoiseShadowVisibilityMasks(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const TStaticArray<FShadowVisibilityParameters, IScreenSpaceDenoiser::kMaxBatchSize>& InputParameters, const int32 InputParameterCount, TStaticArray<FShadowVisibilityOutputs, IScreenSpaceDenoiser::kMaxBatchSize>& Outputs) const
{
	WrappedDenoiser->DenoiseShadowVisibilityMasks(GraphBuilder, View, PreviousViewInfos, SceneTextures, InputParameters, InputParameterCount, Outputs);
}

IScreenSpaceDenoiser::FPolychromaticPenumbraOutputs FFusionDenoiser::DenoisePolychromaticPenumbraHarmonics(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FPolychromaticPenumbraHarmonics& Inputs) const
{
	return WrappedDenoiser->DenoisePolychromaticPenumbraHarmonics(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs);
}

IScreenSpaceDenoiser::FReflectionsOutputs FFusionDenoiser::DenoiseReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const
{ 
	return WrappedDenoiser->DenoiseReflections(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FReflectionsOutputs FFusionDenoiser::DenoiseWaterReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseWaterReflections(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FAmbientOcclusionOutputs FFusionDenoiser::DenoiseAmbientOcclusion(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FAmbientOcclusionInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseAmbientOcclusion(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FFusionDenoiser::DenoiseDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
    RDG_GPU_STAT_SCOPE(GraphBuilder, FusionDiffuseDenoiser);
	RDG_EVENT_SCOPE(GraphBuilder, "FusionDiffuseDenoiser");
    FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
   
    FRDGTextureRef GBufferATexture = SceneTextures.GBufferATexture;
    FRDGTextureRef GBufferBTexture = SceneTextures.GBufferBTexture;
    FRDGTextureRef GBufferCTexture = SceneTextures.GBufferCTexture;
    FRDGTextureRef SceneDepthTexture = SceneTextures.SceneDepthTexture;
    FRDGTextureRef SceneVelocityTexture = SceneTextures.GBufferVelocityTexture;

	FIntPoint TexSize = SceneTextures.SceneDepthTexture->Desc.Extent;
	//FIntPoint TexSize = FIntPoint(SceneTextures.SceneDepthTexture->Desc.Extent.X* Config.ResolutionFraction, SceneTextures.SceneDepthTexture->Desc.Extent.Y * Config.ResolutionFraction);
    FVector4 BufferTexSize = FVector4(TexSize.X, TexSize.Y, 1.0 / TexSize.X, 1.0 / TexSize.Y);

    FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
        SceneTextures.SceneDepthTexture->Desc.Extent,
        PF_FloatRGBA,
        FClearValueBinding::None,
        TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
    
    auto PreOutputTex =  GraphBuilder.CreateTexture(Desc, TEXT("DiffuseIndirectPreConvolution0"));
    auto OutputTex =  GraphBuilder.CreateTexture(Desc, TEXT("DenoisedDiffuse"));
    auto TemporalOutTex =  GraphBuilder.CreateTexture(Desc, TEXT("DiffuseIndirectTemporalAccumulation0"));
    auto ReprojectedHistoryTex =  GraphBuilder.CreateTexture(Desc, TEXT("DiffuseIndirectReprojected"));
    Desc.Format = PF_G32R32F;
    auto VarianceTex = GraphBuilder.CreateTexture(Desc, TEXT("DiffuseVariance"));
    FRDGTextureRef TemporalHistTex = nullptr, VarianceHistTex = nullptr;
    bool ResetHistory = !PreviousViewInfos->FusionDiffuseIndirectHistory.RT[0];
    FRDGTextureRef OutputSignal = Inputs.Color;
    // else
    // {
    //     Desc.Format = PF_FloatRGBA;
    //     TemporalHistTex = GraphBuilder.CreateTexture(Desc, TEXT("DiffuseTemporalHist"));
    //     Desc.Format = PF_G32R32F;
    //     VarianceHistTex = GraphBuilder.CreateTexture(Desc, TEXT("DiffuseHistVariance"));
    // }
    uint32 IterationCount = 1;
    uint32 SequenceCount = 1;
    uint32 DimensionCount = 24;

    FScene* Scene = static_cast<FScene*>(View.Family->Scene);
    FHaltonSequenceIteration HaltonSequenceIteration(Scene->HaltonSequence, IterationCount, SequenceCount, DimensionCount, View.ViewState ? (View.ViewState->FrameIndex % 1024) : 0);
    FHaltonIteration HaltonIteration;
    InitializeHaltonSequenceIteration(HaltonSequenceIteration, HaltonIteration);

    FHaltonPrimes HaltonPrimes;
    InitializeHaltonPrimes(Scene->HaltonPrimesResource, HaltonPrimes);

    FBlueNoise BlueNoise;
    InitializeBlueNoise(BlueNoise);

	FDiffuseInDirectSpatialFilterCS::FParameters CommonParameters;

    CommonParameters.HaltonIteration = CreateUniformBufferImmediate(HaltonIteration, EUniformBufferUsage::UniformBuffer_SingleFrame);
    CommonParameters.HaltonPrimes = CreateUniformBufferImmediate(HaltonPrimes, EUniformBufferUsage::UniformBuffer_SingleFrame);
    CommonParameters.BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleFrame);
    {
        FDiffuseInDirectSpatialFilterCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FDiffuseInDirectSpatialFilterCS::FUseSSAODim>(CVarFusionSpatialUseSSAO.GetValueOnRenderThread() > 0);
        PermutationVector.Set<FDiffuseInDirectSpatialFilterCS::FStageDim>(EStage::PreConvolution);
        TShaderMapRef<FDiffuseInDirectSpatialFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
        FDiffuseInDirectSpatialFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDiffuseInDirectSpatialFilterCS::FParameters>();
        *PassParameters = CommonParameters;
        PassParameters->InputTex = Inputs.Color;
        PassParameters->RWFilteredTex = GraphBuilder.CreateUAV(PreOutputTex);
        PassParameters->SSAOTex = GraphBuilder.RegisterExternalTexture(SceneContext.ScreenSpaceAO);
        PassParameters->NormalTexture = GBufferATexture;
        PassParameters->DepthTexture = SceneDepthTexture;

        PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

        FIntPoint HalfTexSize = FIntPoint(TexSize.X* Config.ResolutionFraction, TexSize.Y * Config.ResolutionFraction);
        PassParameters->BufferTexSize = FVector4( HalfTexSize.X, HalfTexSize.Y, 1.0 / HalfTexSize.X, 1.0 / HalfTexSize.Y) ;
        PassParameters->UpscaleFactor = int32(1.0 /Config.ResolutionFraction); 
        ClearUnusedGraphResources(ComputeShader, PassParameters);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("DiffuseIndirect Pre SpatioalFilter"),
            ComputeShader,
            PassParameters,
            FComputeShaderUtils::GetGroupCount(TexSize, FDiffuseInDirectSpatialFilterCS::GetThreadBlockSize()));
        OutputSignal = PreOutputTex;
    }

    if( CVarFusionTemporalEnabled.GetValueOnRenderThread() > 0)
    {
        if( ResetHistory )
        {
            FDiffuseInDirectTemporalFilterCS::FPermutationDomain PermutationVector;
            PermutationVector.Set<FDiffuseInDirectTemporalFilterCS::FStageDim>(ETemporalFilterStage::ResetHistory);
            TShaderMapRef<FDiffuseInDirectTemporalFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
            FDiffuseInDirectTemporalFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDiffuseInDirectTemporalFilterCS::FParameters>();
            PassParameters->InputTex = PreOutputTex;
            PassParameters->RWHistoryTex = GraphBuilder.CreateUAV(TemporalOutTex);
            PassParameters->RWVarianceTex =  GraphBuilder.CreateUAV(VarianceTex);
            ClearUnusedGraphResources(ComputeShader, PassParameters);
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("FDiffuseIndirectTemporalFilter"),
                ComputeShader,
                PassParameters,
                FComputeShaderUtils::GetGroupCount(TexSize, FDiffuseInDirectTemporalFilterCS::GetThreadBlockSize()));
            OutputSignal = OutputTex;

        }
        else
        {
            {
                FDiffuseInDirectTemporalFilterCS::FPermutationDomain PermutationVector;
                PermutationVector.Set<FDiffuseInDirectTemporalFilterCS::FStageDim>(ETemporalFilterStage::ReprojectHistory);
                TShaderMapRef<FDiffuseInDirectTemporalFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
                FDiffuseInDirectTemporalFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDiffuseInDirectTemporalFilterCS::FParameters>();
                PassParameters->HistoryTex = GraphBuilder.RegisterExternalTexture(PreviousViewInfos->FusionDiffuseIndirectHistory.RT[0]);
                PassParameters->VarianceHistoryTex = GraphBuilder.RegisterExternalTexture(PreviousViewInfos->FusionDiffuseIndirectHistory.RT[1]);

                PassParameters->InputTex = PreOutputTex;
                PassParameters->RWHistoryTex = GraphBuilder.CreateUAV(ReprojectedHistoryTex);

                PassParameters->ReprojectionTex = View.ProjectionMapTexture;

                PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
           
                PassParameters->BufferTexSize = BufferTexSize;
                ClearUnusedGraphResources(ComputeShader, PassParameters);
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("FDiffuseIndirectTemporalReproject"),
                    ComputeShader,
                    PassParameters,
                    FComputeShaderUtils::GetGroupCount(TexSize, FDiffuseInDirectTemporalFilterCS::GetThreadBlockSize()));
                OutputSignal = OutputTex;
            }

            {
                FDiffuseInDirectTemporalFilterCS::FPermutationDomain PermutationVector;
                PermutationVector.Set<FDiffuseInDirectTemporalFilterCS::FStageDim>(ETemporalFilterStage::TemporalAccum);
                TShaderMapRef<FDiffuseInDirectTemporalFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
                FDiffuseInDirectTemporalFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDiffuseInDirectTemporalFilterCS::FParameters>();
                PassParameters->HistoryTex = ReprojectedHistoryTex;
                //PassParameters->HistoryTex = GraphBuilder.RegisterExternalTexture(PreviousViewInfos->FusionDiffuseIndirectHistory.RT[0]);
                PassParameters->VarianceHistoryTex = GraphBuilder.RegisterExternalTexture(PreviousViewInfos->FusionDiffuseIndirectHistory.RT[1]);
            
                PassParameters->InputTex = PreOutputTex;
                PassParameters->RWHistoryTex = GraphBuilder.CreateUAV(TemporalOutTex);
                PassParameters->RWOutputTex = GraphBuilder.CreateUAV(OutputTex);
                PassParameters->RWVarianceTex =  GraphBuilder.CreateUAV(VarianceTex);
            
                PassParameters->NormalHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.GBufferA, GSystemTextures.BlackDummy);
                PassParameters->DepthHistory = RegisterExternalTextureWithFallback(GraphBuilder, View.PrevViewInfo.DepthBuffer, GSystemTextures.BlackDummy);
                PassParameters->ReprojectionTex = View.ProjectionMapTexture;

                PassParameters->NormalTexture = GBufferATexture;
                PassParameters->DepthTexture = SceneDepthTexture;
                PassParameters->VelocityTexture = SceneVelocityTexture;
                PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
                PassParameters->TemporalNormalRejectionThreshold = CVarDenoiserTemporalNormalRejectionThreshold.GetValueOnRenderThread();
                PassParameters->TemporalDepthRejectionThreshold = CVarDenoiserTemporalDepthRejectionThreshold.GetValueOnRenderThread();

                PassParameters->BufferTexSize = BufferTexSize;
                ClearUnusedGraphResources(ComputeShader, PassParameters);
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("FDiffuseIndirectAccum"),
                    ComputeShader,
                    PassParameters,
                    FComputeShaderUtils::GetGroupCount(TexSize, FDiffuseInDirectTemporalFilterCS::GetThreadBlockSize()));
                OutputSignal = OutputTex;
            }
        }
    }

    if (!View.bStatePrevViewInfoIsReadOnly && CVarFusionTemporalEnabled.GetValueOnRenderThread() > 0)
	{
		//Extract history feedback here
		GraphBuilder.QueueTextureExtraction(TemporalOutTex, &View.ViewState->PrevFrameViewInfo.FusionDiffuseIndirectHistory.RT[0]);
        GraphBuilder.QueueTextureExtraction(VarianceTex, &View.ViewState->PrevFrameViewInfo.FusionDiffuseIndirectHistory.RT[1]);
	}

    if( CVarFusionSpatialEnabled.GetValueOnRenderThread() > 0)
    {
		FDiffuseInDirectSpatialFilterCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FDiffuseInDirectSpatialFilterCS::FUseSSAODim>(CVarFusionSpatialUseSSAO.GetValueOnRenderThread() > 0);
        PermutationVector.Set<FDiffuseInDirectSpatialFilterCS::FStageDim>(EStage::PostFiltering);
        TShaderMapRef<FDiffuseInDirectSpatialFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), PermutationVector);
        FDiffuseInDirectSpatialFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDiffuseInDirectSpatialFilterCS::FParameters>();
        //PassParameters->InputTex = OutputTex;
        PassParameters->SSAOTex = GraphBuilder.RegisterExternalTexture(SceneContext.ScreenSpaceAO);
        PassParameters->NormalTexture = GBufferATexture;
        PassParameters->DepthTexture = SceneDepthTexture;
        PassParameters->RWFilteredTex =  GraphBuilder.CreateUAV(OutputSignal);
        PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

        PassParameters->BufferTexSize = BufferTexSize;
        PassParameters->UpscaleFactor = int32(1.0 /Config.ResolutionFraction); 
        ClearUnusedGraphResources(ComputeShader, PassParameters);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("DiffuseIndirect Post SpatioalFilter"),
            ComputeShader,
            PassParameters,
            FComputeShaderUtils::GetGroupCount(TexSize, FDiffuseInDirectSpatialFilterCS::GetThreadBlockSize()));
         OutputSignal = OutputTex;
    }


    FDiffuseIndirectOutputs GlobalIlluminationOutputs;
    GlobalIlluminationOutputs.Color = OutputSignal;
    return GlobalIlluminationOutputs;
    //TODO: add restir gi denoiser, now use default denoiser
	//return WrappedDenoiser->DenoiseDiffuseIndirect(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FFusionDenoiser::DenoiseSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseSkyLight(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FFusionDenoiser::DenoiseReflectedSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseReflectedSkyLight(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectHarmonic FFusionDenoiser::DenoiseDiffuseIndirectHarmonic(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectHarmonic& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseDiffuseIndirectHarmonic(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

bool FFusionDenoiser::SupportsScreenSpaceDiffuseIndirectDenoiser(EShaderPlatform Platform) const
{
	return WrappedDenoiser->SupportsScreenSpaceDiffuseIndirectDenoiser(Platform);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FFusionDenoiser::DenoiseScreenSpaceDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseScreenSpaceDiffuseIndirect(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

const IScreenSpaceDenoiser* FFusionDenoiser::GetWrappedDenoiser() const
{
	return WrappedDenoiser;
}

IScreenSpaceDenoiser* FFusionDenoiser::GetDenoiser()
{
    const IScreenSpaceDenoiser* DenoiserToWrap = GScreenSpaceDenoiser ? GScreenSpaceDenoiser : IScreenSpaceDenoiser::GetDefaultDenoiser();

   static FFusionDenoiser denoiser(DenoiserToWrap);
   return &denoiser;
}