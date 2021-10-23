/*
* Copyright (c) 2021 NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA Corporation and its licensors retain all intellectual property and proprietary
* rights in and to this software, related documentation and any modifications thereto.
* Any use, reproduction, disclosure or distribution of this software and related
* documentation without an express license agreement from NVIDIA Corporation is strictly
* prohibited.
*
* TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS*
* AND NVIDIA AND ITS SUPPLIERS DISCLAIM ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
* PARTICULAR PURPOSE.  IN NO EVENT SHALL NVIDIA OR ITS SUPPLIERS BE LIABLE FOR ANY
* SPECIAL, INCIDENTAL, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER (INCLUDING, WITHOUT
* LIMITATION, DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF
* BUSINESS INFORMATION, OR ANY OTHER PECUNIARY LOSS) ARISING OUT OF THE USE OF OR
* INABILITY TO USE THIS SOFTWARE, EVEN IF NVIDIA HAS BEEN ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGES.
*/

#include "ReLAX.h"
#include "NRDCommon.h"
#include "NRDPrivate.h"
#include "NRDDenoiserHistory.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

#include "Shader.h"
#include "ScreenPass.h"
#include "ShaderCore.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

#include "RenderGraphBuilder.h"



namespace
{
	static const uint32 RELAX_MAX_HISTORY_FRAME_NUM = 63;

	// History
	NRDCVar<int32> RelaxHistorySpecularMaxAccumulatedFrameNum(TEXT("r.NRD.Relax.History.SpecularMaxAccumulatedFrameNum"), 63, TEXT("Amount of frames in history for specular signal temporal accumulation "), 0 , RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistorySpecularMaxFastAccumulatedFrameNum(TEXT("r.NRD.Relax.History.SpecularFastMaxAccumulatedFrameNum"), 4, TEXT("Amount of frames in history for responsive specular signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistoryDiffuseMaxAccumulatedFrameNum(TEXT("r.NRD.Relax.History.DiffuseMaxAccumulatedFrameNum"), 63, TEXT("Amount of frames in history for diffuse signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistoryDiffuseMaxFastAccumulatedFrameNum(TEXT("r.NRD.Relax.History.DiffuseFastMaxAccumulatedFrameNum"), 0, TEXT("Amount of frames in history for fast responsive signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);

	// REPROJECTION
	NRDCVar<float> RelaxReprojectionSpecularVarianceBoost(TEXT("r.NRD.Relax.Reprojection.SpecularVarianceBoost"), 1.0f, TEXT("How much variance we inject to specular if reprojection confidence is low"), 0.0f, 8.0f);
	NRDCVar<float> RelaxReprojectionHistoryClampingColorBoxSigmaScale(TEXT("r.NRD.Relax.Reprojection.HistoryClampingColorBoxSigmaScale"), 1.0f, TEXT("Scale for standard deviation of color box for clamping normal history color to responsive history color"), 1.0f, 10.0f);
	NRDCVar<bool>  RelaxReprojectionBicubicFilter(TEXT("r.NRD.Relax.Reprojection.BicubicFilter"), true, TEXT("Slower but sharper filtering of the history during reprojection"), false, true);
	NRDCVar<float> RelaxReprojectionDisocclusionThreshold(TEXT("r.NRD.Relax.Reprojection.DisocclusionThreshold"), 0.01f, TEXT("Percentage of the depth value for disocclusion detection / geometry tests"), 0.001f, 1.0f);

	// DISOCCLUSION FIX
	NRDCVar<float> RelaxDisocclusionFixEdgeStoppingNormalPower(TEXT("r.NRD.Relax.DisocclusionFix.EdgeStoppingNormalPower"), 8.0f, TEXT("Normal edge stopper for cross-bilateral sparse filter"), 0.0f, 128.0f);
	NRDCVar<float> RelaxDisocclusionFixMaxRadius(TEXT("r.NRD.Relax.DisocclusionFix.MaxRadius"), 8.0f, TEXT("Maximum radius for sparse bilateral filter, expressed in pixels"), 0.0f, 100.0f);
	NRDCVar<int32> RelaxDisocclusionFixNumFramesToFix(TEXT("r.NRD.Relax.DisocclusionFix.NumFramesToFix"), 2, TEXT("Cross-bilateral sparse filter will be applied to frames with history length shorter than this value"), 0, 10);

	// ANTI - LAG
	NRDCVar<float> RelaxAntiLagSpecularColorBoxSigmaScale(TEXT("r.NRD.Relax.AntiLag.SpecularColorBoxSigmaScale"), 2.0f, TEXT("Scale for standard deviation of specular color box for lag detection"), 1.0f, 10.0f);
	NRDCVar<float> RelaxAntiLagSpecularPower(TEXT("r.NRD.Relax.AntiLag.SpecularPower"), 0.0f, TEXT("Amount of history shortening when specular lag is detected"), 0.0f, 100.0f);
	NRDCVar<float> RelaxAntiLagDiffuseColorBoxSigmaScale(TEXT("r.NRD.Relax.AntiLag.DiffuseColorBoxSigmaScale"), 2.0f, TEXT("Scale for standard deviation of diffuse color box for lag detection"), 1.0f, 10.0f);
	NRDCVar<float> RelaxAntiLagDiffusePower(TEXT("r.NRD.Relax.AntiLag.DiffusePower"), 0.0f, TEXT("Amount of history shortening when diffuse lag is detected"), 0.0f, 100.0f);

	// SPATIAL VARIANCE ESTIMATION
	NRDCVar<int32> RelaxSpatialVarianceEstimationHistoryThreshold(TEXT("r.NRD.Relax.SpatialVarianceEstimation.HistoryThreshold"), 3, TEXT("History length threshold below which spatial variance estimation will be applied"), 0, 10);
	
	// A-TROUS
	NRDCVar<int32> RelaxAtrousIterations(	TEXT("r.NRD.Relax.Atrous.Iterations"), 5, TEXT("Number of iterations of the A-trous filter."), 2, 8);
	NRDCVar<float> RelaxAtrousDiffusePhiLuminance(TEXT("r.NRD.Relax.Atrous.DiffusePhiLuminance"), 1.5f, TEXT("A-trous edge stopping diffuse Luminance sensitivity"), 0.0f, 10.0f);
	NRDCVar<float> RelaxAtrousSpecularPhiLuminance(TEXT("r.NRD.Relax.Atrous.SpecularPhiLuminance"), 1.5f, TEXT("A-trous edge stopping specular Luminance sensitivity."), 0.0f, 10.0f);


	NRDCVar<float> RelaxAtrousPhiNormal(TEXT("r.NRD.Relax.Atrous.PhiNormal"), 64.0f, TEXT("A-trous edge stopping Normal sensitivity for diffuse"), 0.1f, 256.0f);
	NRDCVar<float> RelaxAtrousPhiDepth(TEXT("r.NRD.Relax.Atrous.PhiDepth"), 0.05f, TEXT(" A-trous edge stopping Depth sensitivity."), 0.0f, 1.0f);
	
	NRDCVar<float> RelaxAtrousRoughnessEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.RoughnessEdgeStoppingRelaxation"), 0.3f, TEXT("How much we relax roughness based rejection in areas where specular reprojection is low"), 0.0f, 1.0f);
	NRDCVar<float> RelaxAtrousNormalEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.NormalEdgeStoppingRelaxation"), 0.3f, TEXT("How much we relax normal based rejection in areas where specular reprojection is low."), 0.0f, 1.0f);
	NRDCVar<float> RelaxAtrousLuminanceEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.LuminanceEdgeStoppingRelaxation"), 1.0f, TEXT("How much we relax luminance based rejection in areas where specular reprojection is low"), 0.0f, 1.0f);

	// MISC
	NRDCVar<bool>  RelaxFireflySupression(TEXT("r.NRD.Relax.FireflySupression"), false, TEXT("Whether to suppress fireflies or not"), false, true);
	NRDCVar<int32> RelaxSplitScreenPercentage(TEXT("r.NRD.Relax.SplitScreen.Percentage"), 0, TEXT("Where to split the screen between inputs and denoised outputs. In Percent"), 0, 100);
	NRDCVar<float> NRDDenoisingRange(TEXT("r.NRD.DenoisingRange"), 100000.0f, TEXT("World space range of geometry"), 0.0f, 10000000.0f);

	NRDCVar<bool>  RelaxDiffuseRadianceCompression(TEXT("r.NRD.Relax.DiffuseRadianceCompression"), false, TEXT("Whether to compress diffuse radiance at input/output. This can help with loss of details in highlight"), false, true);
	NRDCVar<bool>  RelaxSpecularRadianceCompression(TEXT("r.NRD.Relax.SpecularRadianceCompression"), false, TEXT("Whether to compress specular radiance at input/output. This can help with loss of details in highlight"), false, true);
}

class RelaxDiffuseSpecularSettings
{
private:
	RelaxDiffuseSpecularSettings()
	{
	}

public:
	static RelaxDiffuseSpecularSettings FromConsoleVariables()
	{
		RelaxDiffuseSpecularSettings Result;
		// History
		Result.specularMaxAccumulatedFrameNum = RelaxHistorySpecularMaxAccumulatedFrameNum;
		Result.specularMaxFastAccumulatedFrameNum = RelaxHistorySpecularMaxFastAccumulatedFrameNum;
		Result.diffuseMaxAccumulatedFrameNum = RelaxHistoryDiffuseMaxAccumulatedFrameNum;
		Result.diffuseMaxFastAccumulatedFrameNum = RelaxHistoryDiffuseMaxFastAccumulatedFrameNum;

		// REPROJECTION
		Result.specularVarianceBoost = RelaxReprojectionSpecularVarianceBoost;
		Result.historyClampingColorBoxSigmaScale = RelaxReprojectionHistoryClampingColorBoxSigmaScale;
		Result.bicubicFilterForReprojectionEnabled = RelaxReprojectionBicubicFilter;
		Result.disocclusionThreshold = RelaxReprojectionDisocclusionThreshold;

		// DISOCCLUSION FIX
		Result.disocclusionFixEdgeStoppingNormalPower = RelaxDisocclusionFixEdgeStoppingNormalPower;
		Result.disocclusionFixMaxRadius = RelaxDisocclusionFixMaxRadius;
		Result.disocclusionFixNumFramesToFix = RelaxDisocclusionFixNumFramesToFix;

		// ANTI - LAG
		Result.specularAntiLagColorBoxSigmaScale = RelaxAntiLagSpecularColorBoxSigmaScale;
		Result.specularAntiLagPower = RelaxAntiLagSpecularPower;
		Result.diffuseAntiLagColorBoxSigmaScale = RelaxAntiLagDiffuseColorBoxSigmaScale;
		Result.diffuseAntiLagPower = RelaxAntiLagDiffusePower;

		// SPATIAL VARIANCE ESTIMATION
		Result.spatialVarianceEstimationHistoryThreshold = RelaxSpatialVarianceEstimationHistoryThreshold;

		// A-TROUS
		Result.atrousIterationNum = RelaxAtrousIterations;
		Result.specularPhiLuminance = RelaxAtrousSpecularPhiLuminance;
		Result.diffusePhiLuminance = RelaxAtrousDiffusePhiLuminance;
		Result.phiNormal = RelaxAtrousPhiNormal;
		Result.phiDepth = RelaxAtrousPhiDepth;
		Result.roughnessEdgeStoppingRelaxation = RelaxAtrousRoughnessEdgeStoppingRelaxation;
		Result.normalEdgeStoppingRelaxation = RelaxAtrousNormalEdgeStoppingRelaxation;
		Result.luminanceEdgeStoppingRelaxation = RelaxAtrousLuminanceEdgeStoppingRelaxation;

		// MISC
		Result.antifirefly = RelaxFireflySupression;
		Result.splitScreen = RelaxSplitScreenPercentage;

		Result.denoisingRange = NRDDenoisingRange;

		Result.diffuseRadianceCompression = RelaxDiffuseRadianceCompression;
		Result.specularRadianceCompression = RelaxSpecularRadianceCompression;

		
		return Result;
	}

	// History
	uint32 specularMaxAccumulatedFrameNum;
	uint32 specularMaxFastAccumulatedFrameNum;
	uint32 diffuseMaxAccumulatedFrameNum;
	uint32 diffuseMaxFastAccumulatedFrameNum;
	
	// REPROJECTION
	float specularVarianceBoost;
	float historyClampingColorBoxSigmaScale;
	bool bicubicFilterForReprojectionEnabled;

	// DISOCCLUSION FIX
	float disocclusionFixEdgeStoppingNormalPower;
	float disocclusionFixMaxRadius;
	uint32 disocclusionFixNumFramesToFix;

	// ANTI - LAG
	float specularAntiLagColorBoxSigmaScale;
	float specularAntiLagPower;
	float diffuseAntiLagColorBoxSigmaScale;
	float diffuseAntiLagPower;

	// SPATIAL VARIANCE ESTIMATION
	uint32 spatialVarianceEstimationHistoryThreshold;

	// A-TROUS
	uint32 atrousIterationNum;
	float specularPhiLuminance;
	float diffusePhiLuminance;
	float phiNormal;
	float phiDepth;
	float roughnessEdgeStoppingRelaxation;
	float normalEdgeStoppingRelaxation;
	float luminanceEdgeStoppingRelaxation;

	// MISC
	bool antifirefly;
	uint32 splitScreen;
	float disocclusionThreshold;
	float denoisingRange;

	bool diffuseRadianceCompression;
	bool specularRadianceCompression;
};

FNRDPackInputsArguments RelaxPackInputArguments()
{
	FNRDPackInputsArguments Result;
	Result.bPackDiffuseHitDistance = false;
	Result.bPackSpecularHitDistance = true;

	Result.bCompressDiffuseRadiance = RelaxDiffuseRadianceCompression;
	Result.bCompressSpecularRadiance = RelaxSpecularRadianceCompression;
	return Result;
}



// PACK INPUT DATA
class FNRDRELAXPackInputDataCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXPackInputDataCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXPackInputDataCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, gIn_Normal_Roughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, gIn_ViewZ)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>, gOutNormalRoughnessDepth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXPackInputDataCS, "/Plugin/NRD/Private/RELAX_PackInputData.cs.usf", "main", SF_Compute);

// REPROJECT
class FNRDRELAXReprojectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXReprojectCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXReprojectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FMatrix,   gPrevWorldToClip)
		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FVector4,  gPrevFrustumRight)
		SHADER_PARAMETER(FVector4,  gPrevFrustumUp)
		SHADER_PARAMETER(FVector4,  gPrevFrustumForward)
		SHADER_PARAMETER(FVector,   gPrevCameraPosition)
		SHADER_PARAMETER(float,     gJitterDelta)
		SHADER_PARAMETER(FVector2D, gMotionVectorScale)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize)
		SHADER_PARAMETER(float,     gUseBicubic)
		SHADER_PARAMETER(float,     gSpecularAlpha)
		SHADER_PARAMETER(float,     gSpecularResponsiveAlpha)
		SHADER_PARAMETER(float,     gSpecularVarianceBoost)
		SHADER_PARAMETER(float,     gDiffuseAlpha)
		SHADER_PARAMETER(float,     gDiffuseResponsiveAlpha)
		SHADER_PARAMETER(float,     gWorldSpaceMotion)
		SHADER_PARAMETER(float,     gIsOrtho)
		SHADER_PARAMETER(float,     gUnproject)
		SHADER_PARAMETER(float,     gNeedHistoryReset)
		SHADER_PARAMETER(float,     gDenoisingRange)
		SHADER_PARAMETER(float,     gDisocclusionThreshold)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gMotion)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gNormalRoughnessDepth)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gPrevSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gPrevSpecularAndDiffuseIlluminationResponsiveLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevSpecularIlluminationUnpacked)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevDiffuseIlluminationUnpacked)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gPrevSpecularAndDiffuse2ndMoments)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gPrevNormalRoughnessDepth)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gPrevReflectionHitT)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gPrevSpecularAndDiffuseHistoryLength)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationResponsiveLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, gOutSpecularAndDiffuse2ndMoments)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>,  gOutReflectionHitT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, gOutSpecularAndDiffuseHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>,  gOutSpecularReprojectionConfidence)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXReprojectCS, "/Plugin/NRD/Private/RELAX_Reproject.cs.usf", "main", SF_Compute);

// DISOCCLUSION FIX
class FNRDRELAXDisocclusionFixCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXDisocclusionFixCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXDisocclusionFixCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize)
		SHADER_PARAMETER(float,     gDisocclusionThreshold)
		SHADER_PARAMETER(float,     gDisocclusionFixEdgeStoppingNormalPower)
		SHADER_PARAMETER(float,     gMaxRadius)
		SHADER_PARAMETER(int,       gFramesToFix)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseIlluminationResponsiveLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gSpecularAndDiffuse2ndMoments)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gSpecularAndDiffuseHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gNormalRoughnessDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationResponsiveLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, gOutSpecularAndDiffuse2ndMoments)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXDisocclusionFixCS, "/Plugin/NRD/Private/RELAX_DisocclusionFix.cs.usf", "main", SF_Compute);

// HISTORY CLAMPING
class FNRDRELAXHistoryClampingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXHistoryClampingCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXHistoryClampingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(float,     gColorBoxSigmaScale)
		SHADER_PARAMETER(float,     gSpecularAntiLagSigmaScale)
		SHADER_PARAMETER(float,     gSpecularAntiLagPower)
		SHADER_PARAMETER(float,     gDiffuseAntiLagSigmaScale)
		SHADER_PARAMETER(float,     gDiffuseAntiLagPower)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseResponsiveIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gSpecularAndDiffuseHistoryLength)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseHistoryLength)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXHistoryClampingCS, "/Plugin/NRD/Private/RELAX_HistoryClamping.cs.usf", "main", SF_Compute);

// FIREFLY
class FNRDRELAXFireflyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXFireflyCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXFireflyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(uint32,    gFireflyEnabled)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gNormalRoughnessDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>,  gOutSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIllumination)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXFireflyCS, "/Plugin/NRD/Private/RELAX_Firefly.cs.usf", "main", SF_Compute);


// SPATIAL VARIANCE ESTIMATION
class FNRDRELAXSpatialVarianceEstimationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXSpatialVarianceEstimationCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXSpatialVarianceEstimationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32,    gHistoryThreshold)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gSpecularAndDiffuseIlluminationLogLuv)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gSpecularAndDiffuse2ndMoments)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gNormalRoughnessDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXSpatialVarianceEstimationCS, "/Plugin/NRD/Private/RELAX_SpatialVarianceEstimation.cs.usf", "main", SF_Compute);

// A-TROUS (SMEM)
class FNRDRELAXAtrousShmemCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXAtrousShmemCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXAtrousShmemCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize)
		SHADER_PARAMETER(float,     gSpecularPhiLuminance)
		SHADER_PARAMETER(float,     gDiffusePhiLuminance)
		SHADER_PARAMETER(float,     gPhiDepth)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32_t,  gStepSize)
		SHADER_PARAMETER(float,     gRoughnessEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gNormalEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gLuminanceEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gSpecularReprojectionConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>,    gNormalRoughnessDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXAtrousShmemCS, "/Plugin/NRD/Private/RELAX_AtrousShmem.cs.usf", "main", SF_Compute);

// A-TROUS
class FNRDRELAXAtrousStandardCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXAtrousStandardCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXAtrousStandardCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize)
		SHADER_PARAMETER(float,     gSpecularPhiLuminance)
		SHADER_PARAMETER(float,     gDiffusePhiLuminance)
		SHADER_PARAMETER(float,     gPhiDepth)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32_t,  gStepSize)
		SHADER_PARAMETER(float,     gRoughnessEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gNormalEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gLuminanceEdgeStoppingRelaxation)
		SHADER_PARAMETER(uint32_t,  gIsLastPass)
		SHADER_PARAMETER(float,     gDenoisingRange)
		SHADER_PARAMETER(float,     gUncompressDiffuse)
		SHADER_PARAMETER(float,     gUncompressSpecular)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gDiffuseIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, gSpecularReprojectionConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, gNormalRoughnessDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXAtrousStandardCS, "/Plugin/NRD/Private/RELAX_AtrousStandard.cs.usf", "main", SF_Compute);

// SPLIT_SCREEN
class FNRDRELAXSplitScreenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXSplitScreenCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXSplitScreenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FVector2D, gInvScreenSize)
		SHADER_PARAMETER(float, gSplitScreen)
		SHADER_PARAMETER(float, gUncompressDiffuse)
		SHADER_PARAMETER(float, gUncompressSpecular)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, gIn_Normal_Roughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, gIn_Spec)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, gIn_Diff)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, gOut_Spec)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, gOut_Diff)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPILER_UNREAL_ENGINE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXSplitScreenCS, "/Plugin/NRD/Private/RELAX_SplitScreen.cs.usf", "main", SF_Compute);


// moral equivalent of DenoiserImpl::UpdateMethod_RelaxDiffuseSpecular(const MethodData& methodData)
FRelaxOutputs AddRelaxPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FRelaxPassParameters& Inputs,
	FNRDRelaxHistoryRef History
	)
{
	Inputs.Validate();
	check(History.IsValid());

	RDG_EVENT_SCOPE(GraphBuilder, "RELAX::DiffuseSpecular");

	RelaxDiffuseSpecularSettings RelaxSettings = RelaxDiffuseSpecularSettings::FromConsoleVariables();

	const FIntPoint DenoiseBufferSize = Inputs.Diffuse->Desc.Extent;
	check(Inputs.Diffuse->Desc.Extent == View.ViewRect.Size());
	const bool bNeedHistoryReset = View.bCameraCut || !History->HasValidResources() || History->FrameIndex == 0;

	// Extract camera parameters from View.

	FMatrix WorldToViewMatrix = View.ViewMatrices.GetTranslatedViewMatrix();
	FMatrix WorldToViewMatrixPrev = View.PrevViewInfo.ViewMatrices.GetTranslatedViewMatrix();
	FMatrix WorldToClipMatrixPrev = View.PrevViewInfo.ViewMatrices.GetTranslatedViewProjectionMatrix().GetTransposed();
	FMatrix ViewToClipMatrix = View.ViewMatrices.GetProjectionMatrix();
	FMatrix ViewToClipMatrixPrev = View.PrevViewInfo.ViewMatrices.GetProjectionMatrix();
	FVector2D JitterDelta2D = View.ViewMatrices.GetTemporalAAJitter() - View.PrevViewInfo.ViewMatrices.GetTemporalAAJitter();
	float JitterDelta = FMath::Max(abs(JitterDelta2D.X), abs(JitterDelta2D.Y));

	// Calculate camera right and up vectors in worldspace scaled according to frustum extents,
	// and unit forward vector, for fast worldspace position reconstruction in shaders
	float TanHalfFov = 1.0f / ViewToClipMatrix.M[0][0];
	float Aspect = ViewToClipMatrix.M[0][0] / ViewToClipMatrix.M[1][1];
	FVector FrustumRight = WorldToViewMatrix.GetColumn(0) * TanHalfFov;
	FVector FrustumUp = WorldToViewMatrix.GetColumn(1) * TanHalfFov * Aspect;
	FVector FrustumForward = WorldToViewMatrix.GetColumn(2);

	float PrevTanHalfFov = 1.0f / ViewToClipMatrixPrev.M[0][0];
	float PrevAspect = ViewToClipMatrixPrev.M[0][0] / ViewToClipMatrixPrev.M[1][1];
	FVector PrevFrustumRight = WorldToViewMatrixPrev.GetColumn(0) * TanHalfFov;
	FVector PrevFrustumUp = WorldToViewMatrixPrev.GetColumn(1) * PrevTanHalfFov * PrevAspect;
	FVector PrevFrustumForward = WorldToViewMatrixPrev.GetColumn(2);

	// Helper funcs
	auto CreateIntermediateTexture = [&GraphBuilder, DenoiseBufferSize](EPixelFormat Format, const TCHAR* DebugName)
	{
		return GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(
				DenoiseBufferSize, Format, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV),
			DebugName
		);
	};

	auto ImportFromHistory = [&GraphBuilder, &History](TRefCountPtr <IPooledRenderTarget>& Buffer, TRefCountPtr <IPooledRenderTarget>& Fallback)
	{
		check((History->FrameIndex != 0) == History->HasValidResources());
		// RegisterExternalTextureWithFallback checks whether Buffer IsValid()
		return GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(
			RegisterExternalTextureWithFallback(GraphBuilder, Buffer , Fallback)
		));
	};

	// Pack input data
	FRDGTextureRef NormalRoughnessDepth = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.NormalRoughnessDepth"));
	{
		
		TShaderMapRef<FNRDRELAXPackInputDataCS> ComputeShader(View.ShaderMap);

		FNRDRELAXPackInputDataCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXPackInputDataCS::FParameters>();

		// Set SRVs
		PassParameters->gIn_Normal_Roughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
		PassParameters->gIn_ViewZ = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.ViewZ));

		// Set UAVs
		PassParameters->gOutNormalRoughnessDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NormalRoughnessDepth));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Pack input data"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);

	}

	// Reproject
	FRDGTextureRef ReprojectSpecularAndDiffuseIlluminationLogLuv = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.Reproject.SpecularAndDiffuseIlluminationLogLuv"));
	FRDGTextureRef ReprojectSpecularAndDiffuseIlluminationResponsiveLogLuv = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.Reproject.SpecularAndDiffuseIlluminationResponsiveLogLuv"));
	FRDGTextureRef ReprojectSpecularAndDiffuse2ndMoments = CreateIntermediateTexture(PF_G16R16F, TEXT("NRD.Relax.Reproject.SpecularAndDiffuse2ndMoments"));
	FRDGTextureRef ReprojectReflectionHitT = CreateIntermediateTexture(PF_R16F, TEXT("NRD.Relax.Reproject.ReflectionHitT"));
	FRDGTextureRef ReprojectSpecularAndDiffuseHistoryLength = CreateIntermediateTexture(PF_R8G8, TEXT("NRD.Relax.Reproject.SpecularAndDiffuseHistoryLength"));
	FRDGTextureRef ReprojectSpecularReprojectionConfidence = CreateIntermediateTexture(PF_R16F, TEXT("NRD.Relax.Reproject.SpecularReprojectionConfidence"));
	{
		TShaderMapRef<FNRDRELAXReprojectCS> ComputeShader(View.ShaderMap);

		FNRDRELAXReprojectCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXReprojectCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gPrevWorldToClip = WorldToClipMatrixPrev;
		PassParameters->gFrustumRight = FrustumRight;
		PassParameters->gFrustumUp = FrustumUp;
		PassParameters->gFrustumForward = FrustumForward;
		PassParameters->gPrevFrustumRight = PrevFrustumRight;
		PassParameters->gPrevFrustumUp = PrevFrustumUp;
		PassParameters->gPrevFrustumForward = PrevFrustumForward;
		PassParameters->gPrevCameraPosition = -View.ViewMatrices.GetViewOrigin() + View.PrevViewInfo.ViewMatrices.GetViewOrigin();
		PassParameters->gJitterDelta = JitterDelta;
		PassParameters->gMotionVectorScale = FVector2D(1.0f, 1.0f);
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gInvViewSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
		PassParameters->gUseBicubic = RelaxSettings.bicubicFilterForReprojectionEnabled;
		PassParameters->gSpecularAlpha = 1.0f / (1.0f + RelaxSettings.specularMaxAccumulatedFrameNum);
		PassParameters->gSpecularResponsiveAlpha = 1.0f / (1.0f + RelaxSettings.specularMaxFastAccumulatedFrameNum);
		PassParameters->gSpecularVarianceBoost = RelaxSettings.specularVarianceBoost;
		PassParameters->gDiffuseAlpha = 1.0f / (1.0f + RelaxSettings.diffuseMaxAccumulatedFrameNum);
		PassParameters->gDiffuseResponsiveAlpha = 1.0f / (1.0f + RelaxSettings.diffuseMaxFastAccumulatedFrameNum);
		PassParameters->gWorldSpaceMotion = 0;
		PassParameters->gIsOrtho = !View.IsPerspectiveProjection();
		PassParameters->gUnproject = 0.0f; // TODO: apply NRD math to proj matrix to ease geometry disocclusion test in case of jitter present (if needed)
		PassParameters->gNeedHistoryReset = bNeedHistoryReset;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
		PassParameters->gDisocclusionThreshold = RelaxSettings.disocclusionThreshold;

		// Set SRVs for input & intermediate textures
		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Specular));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Diffuse));
		PassParameters->gMotion  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.MotionVectors));
		PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

		// Set SRVs for history buffers
		PassParameters->gPrevSpecularAndDiffuseIlluminationLogLuv = ImportFromHistory(History->SpecularAndDiffuseIlluminationLogLuv, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularAndDiffuseIlluminationResponsiveLogLuv = ImportFromHistory(History->SpecularAndDiffuseIlluminationResponsiveLogLuv, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularIlluminationUnpacked = ImportFromHistory(History->SpecularIlluminationUnpacked, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevDiffuseIlluminationUnpacked = ImportFromHistory(History->DiffuseIlluminationUnpacked, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularAndDiffuse2ndMoments = ImportFromHistory(History->SpecularAndDiffuse2ndMoments, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevNormalRoughnessDepth = ImportFromHistory(History->NormalRoughnessDepth, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevReflectionHitT = ImportFromHistory(History->ReflectionHitT, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularAndDiffuseHistoryLength = ImportFromHistory(History->SpecularAndDiffuseHistoryLength, GSystemTextures.ZeroUIntDummy);

		// Set UAVs
		PassParameters->gOutSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gOutSpecularAndDiffuseIlluminationResponsiveLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularAndDiffuseIlluminationResponsiveLogLuv));
		PassParameters->gOutSpecularAndDiffuse2ndMoments = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularAndDiffuse2ndMoments));
		PassParameters->gOutReflectionHitT = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectReflectionHitT));
		PassParameters->gOutSpecularAndDiffuseHistoryLength = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularAndDiffuseHistoryLength));
		PassParameters->gOutSpecularReprojectionConfidence = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularReprojectionConfidence));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Reproject"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}

	// Disocclusion fix
	FRDGTextureRef DisocclusionFixSpecularAndDiffuseIlluminationLogLuv = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.DisocclusionFix.SpecularAndDiffuseIlluminationLogLuv"));
	FRDGTextureRef DisocclusionFixSpecularAndDiffuseIlluminationResponsiveLogLuv = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.DisocclusionFix.SpecularAndDiffuseIlluminationResponsiveLogLuv"));
	FRDGTextureRef DisocclusionFixSpecularAndDiffuse2ndMoments = CreateIntermediateTexture(PF_G16R16F, TEXT("NRD.Relax.DisocclusionFix.SpecularAndDiffuse2ndMoments"));
	{
		TShaderMapRef<FNRDRELAXDisocclusionFixCS> ComputeShader(View.ShaderMap);

		FNRDRELAXDisocclusionFixCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXDisocclusionFixCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gFrustumRight = FrustumRight;
		PassParameters->gFrustumUp = FrustumUp;
		PassParameters->gFrustumForward = FrustumForward;
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gInvViewSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
		PassParameters->gDisocclusionFixEdgeStoppingNormalPower = RelaxSettings.disocclusionFixEdgeStoppingNormalPower;
		PassParameters->gMaxRadius = RelaxSettings.disocclusionFixMaxRadius;
		PassParameters->gFramesToFix = RelaxSettings.disocclusionFixNumFramesToFix;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
		PassParameters->gDisocclusionThreshold = RelaxSettings.disocclusionThreshold;

		// Set SRVs
		PassParameters->gSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gSpecularAndDiffuseIlluminationResponsiveLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseIlluminationResponsiveLogLuv));
		PassParameters->gSpecularAndDiffuse2ndMoments = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuse2ndMoments));
		PassParameters->gSpecularAndDiffuseHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseHistoryLength));
		PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

		// Set UAVs
		PassParameters->gOutSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gOutSpecularAndDiffuseIlluminationResponsiveLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixSpecularAndDiffuseIlluminationResponsiveLogLuv));
		PassParameters->gOutSpecularAndDiffuse2ndMoments = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixSpecularAndDiffuse2ndMoments));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Disocclusion fix"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}

	// History clamping
	FRDGTextureRef HistoryClampingSpecularAndDiffuseIlluminationLogLuv = CreateIntermediateTexture(PF_R32G32_UINT, TEXT("NRD.Relax.HistoryClamping.SpecularAndDiffuseIlluminationLogLuv"));
	FRDGTextureRef HistoryClampingSpecularAndDiffuseHistoryLength = CreateIntermediateTexture(PF_R8G8, TEXT("NRD.Relax.HistoryClamping.SpecularAndDiffuse2ndMoments"));
	{
		TShaderMapRef<FNRDRELAXHistoryClampingCS> ComputeShader(View.ShaderMap);

		FNRDRELAXHistoryClampingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXHistoryClampingCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gColorBoxSigmaScale = RelaxSettings.historyClampingColorBoxSigmaScale;
		PassParameters->gSpecularAntiLagSigmaScale = RelaxSettings.specularAntiLagColorBoxSigmaScale;
		PassParameters->gSpecularAntiLagPower = RelaxSettings.specularAntiLagPower;
		PassParameters->gDiffuseAntiLagSigmaScale = RelaxSettings.diffuseAntiLagColorBoxSigmaScale;
		PassParameters->gDiffuseAntiLagPower = RelaxSettings.diffuseAntiLagPower;

		// Set SRVs
		PassParameters->gSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gSpecularAndDiffuseResponsiveIlluminationLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixSpecularAndDiffuseIlluminationResponsiveLogLuv));
		PassParameters->gSpecularAndDiffuseHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseHistoryLength));

		// Set UAVs
		PassParameters->gOutSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HistoryClampingSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gOutSpecularAndDiffuseHistoryLength = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HistoryClampingSpecularAndDiffuseHistoryLength));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("History Clamping"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);
	}


	// Firefly suppression
	FRDGTextureRef FireflySpecularAndDiffuseIlluminationLogLuv = CreateIntermediateTexture(PF_G32R32F, TEXT("NRD.Relax.FireFly.SpecularAndDiffuseIlluminationLogLuv"));
	FRDGTextureRef FireflySpecularIlluminationUnpacked = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.FireFly.SpecularIlluminationUnpacked"));
	FRDGTextureRef FireflyDiffuseIlluminationUnpacked = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.FireFly.DiffuseIlluminationUnpacked"));
	{
		TShaderMapRef<FNRDRELAXFireflyCS> ComputeShader(View.ShaderMap);

		FNRDRELAXFireflyCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXFireflyCS::FParameters>();

		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gFireflyEnabled = RelaxSettings.antifirefly;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;

		PassParameters->gSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

		PassParameters->gOutSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FireflySpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FireflySpecularIlluminationUnpacked));
		PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FireflyDiffuseIlluminationUnpacked));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Firefly suppression"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);
	}

	// Spatial variance estimation
	FRDGTextureRef SpatialVarianceEstimationSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.SpatialVarianceEstimation.SpecularIlluminationAndVariance"));
	FRDGTextureRef SpatialVarianceEstimationDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.SpatialVarianceEstimation.DiffuseIlluminationAndVariance"));
	{
		TShaderMapRef<FNRDRELAXSpatialVarianceEstimationCS> ComputeShader(View.ShaderMap);

		FNRDRELAXSpatialVarianceEstimationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXSpatialVarianceEstimationCS::FParameters>();

		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gPhiNormal = RelaxSettings.phiNormal;
		PassParameters->gHistoryThreshold = RelaxSettings.spatialVarianceEstimationHistoryThreshold;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;

		PassParameters->gSpecularAndDiffuseIlluminationLogLuv = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(FireflySpecularAndDiffuseIlluminationLogLuv));
		PassParameters->gSpecularAndDiffuse2ndMoments = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixSpecularAndDiffuse2ndMoments));
		PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
		PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

		PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpatialVarianceEstimationSpecularIlluminationAndVariance));
		PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpatialVarianceEstimationDiffuseIlluminationAndVariance));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Spatial Variance Estimation"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);
	}

	FRDGTextureRef FinalAtrousOutputDiffuse = nullptr;
	FRDGTextureRef FinalAtrousOutputSpecular = nullptr;
	{
		RDG_EVENT_SCOPE(GraphBuilder, "A-trous");

		// A-trous (first) SMEM
		FRDGTextureRef AtrousPingSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPing.SpecularIlluminationAndVariance"));
		FRDGTextureRef AtrousPingDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPing.DiffuseIlluminationAndVariance"));
		FRDGTextureRef AtrousPongSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPong.SpecularIlluminationAndVariance"));
		FRDGTextureRef AtrousPongDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPong.DiffuseIlluminationAndVariance"));
		{
			TShaderMapRef<FNRDRELAXAtrousShmemCS> ComputeShader(View.ShaderMap);

			FNRDRELAXAtrousShmemCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXAtrousShmemCS::FParameters>();

			PassParameters->gFrustumRight = FrustumRight;
			PassParameters->gFrustumUp = FrustumUp;
			PassParameters->gFrustumForward = FrustumForward;
			PassParameters->gResolution = DenoiseBufferSize;
			PassParameters->gInvViewSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
			PassParameters->gSpecularPhiLuminance = RelaxSettings.specularPhiLuminance;
			PassParameters->gDiffusePhiLuminance = RelaxSettings.diffusePhiLuminance;
			PassParameters->gPhiDepth = RelaxSettings.phiDepth;
			PassParameters->gPhiNormal = RelaxSettings.phiNormal;
			PassParameters->gStepSize = 1;
			PassParameters->gRoughnessEdgeStoppingRelaxation = RelaxSettings.roughnessEdgeStoppingRelaxation;
			PassParameters->gNormalEdgeStoppingRelaxation = RelaxSettings.normalEdgeStoppingRelaxation;
			PassParameters->gLuminanceEdgeStoppingRelaxation = RelaxSettings.luminanceEdgeStoppingRelaxation;
			PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;

			PassParameters->gSpecularIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialVarianceEstimationSpecularIlluminationAndVariance));
			PassParameters->gDiffuseIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialVarianceEstimationDiffuseIlluminationAndVariance));
			PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
			PassParameters->gSpecularReprojectionConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularReprojectionConfidence));
			PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

			PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousPingSpecularIlluminationAndVariance));
			PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousPingDiffuseIlluminationAndVariance));

			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("A-Trous SHMEM"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
			);
		}
	
		check(2 <= RelaxSettings.atrousIterationNum && RelaxSettings.atrousIterationNum <= 7);
		// Running variable number of Atrous passes
		for (uint32_t i = 1; i < RelaxSettings.atrousIterationNum; i++)
		{
			FRDGTextureRef AtrousInputDiffuse;
			FRDGTextureRef AtrousInputSpecular;
			FRDGTextureRef AtrousOutputDiffuse;
			FRDGTextureRef AtrousOutputSpecular;

			if ((i % 2) == 1)
			{
				AtrousInputDiffuse = AtrousPingDiffuseIlluminationAndVariance;
				AtrousInputSpecular = AtrousPingSpecularIlluminationAndVariance;
				AtrousOutputDiffuse = AtrousPongDiffuseIlluminationAndVariance;
				AtrousOutputSpecular = AtrousPongSpecularIlluminationAndVariance;
			}
			else
			{
				AtrousInputDiffuse = AtrousPongDiffuseIlluminationAndVariance;
				AtrousInputSpecular = AtrousPongSpecularIlluminationAndVariance;
				AtrousOutputDiffuse = AtrousPingDiffuseIlluminationAndVariance;
				AtrousOutputSpecular = AtrousPingSpecularIlluminationAndVariance;
			}

			TShaderMapRef<FNRDRELAXAtrousStandardCS> ComputeShader(View.ShaderMap);

			FNRDRELAXAtrousStandardCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXAtrousStandardCS::FParameters>();

			PassParameters->gFrustumRight = FrustumRight;
			PassParameters->gFrustumUp = FrustumUp;
			PassParameters->gFrustumForward = FrustumForward;
			PassParameters->gResolution = DenoiseBufferSize;
			PassParameters->gInvViewSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
			PassParameters->gSpecularPhiLuminance = RelaxSettings.specularPhiLuminance;
			PassParameters->gDiffusePhiLuminance = RelaxSettings.diffusePhiLuminance;
			PassParameters->gPhiDepth = RelaxSettings.phiDepth;
			PassParameters->gPhiNormal = RelaxSettings.phiNormal;
			PassParameters->gStepSize = 1 << i;
			PassParameters->gIsLastPass = (i == RelaxSettings.atrousIterationNum - 1) ? 1 : 0;
			PassParameters->gRoughnessEdgeStoppingRelaxation = RelaxSettings.roughnessEdgeStoppingRelaxation;
			PassParameters->gNormalEdgeStoppingRelaxation = RelaxSettings.normalEdgeStoppingRelaxation;
			PassParameters->gLuminanceEdgeStoppingRelaxation = RelaxSettings.luminanceEdgeStoppingRelaxation;
			PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
			PassParameters->gUncompressDiffuse = RelaxDiffuseRadianceCompression;
			PassParameters->gUncompressSpecular = RelaxSpecularRadianceCompression;

			PassParameters->gSpecularIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtrousInputSpecular));
			PassParameters->gDiffuseIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtrousInputDiffuse));
			PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
			PassParameters->gSpecularReprojectionConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularReprojectionConfidence));
			PassParameters->gNormalRoughnessDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));

			PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousOutputSpecular));
			PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousOutputDiffuse));

			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("A-Trous standard"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
			);
		}
		// Selecting final output of the set of Atrous passes
		FinalAtrousOutputDiffuse = ((RelaxSettings.atrousIterationNum % 2) == 1) ? AtrousPongDiffuseIlluminationAndVariance : AtrousPingDiffuseIlluminationAndVariance;
		FinalAtrousOutputSpecular = ((RelaxSettings.atrousIterationNum % 2) == 1) ? AtrousPongSpecularIlluminationAndVariance : AtrousPingSpecularIlluminationAndVariance;
	}

	// split screen
	const int SplitScreenPercentage = RelaxSettings.splitScreen;
	if(0 != SplitScreenPercentage)
	{
		TShaderMapRef<FNRDRELAXSplitScreenCS> ComputeShader(View.ShaderMap);

		FNRDRELAXSplitScreenCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXSplitScreenCS::FParameters>();

		PassParameters->gInvScreenSize = FVector2D( 1.0f / DenoiseBufferSize.X, 1.0f / DenoiseBufferSize.Y) ;
		PassParameters->gSplitScreen = float(FMath::Clamp(SplitScreenPercentage, 0, 100)) / 100.0f;
		PassParameters->gUncompressDiffuse = RelaxDiffuseRadianceCompression;
		PassParameters->gUncompressSpecular = RelaxSpecularRadianceCompression;


		PassParameters->gIn_Normal_Roughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalRoughnessDepth));
		PassParameters->gIn_Diff = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Diffuse));
		PassParameters->gIn_Spec = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Specular));

		PassParameters->gOut_Diff = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FinalAtrousOutputDiffuse));
		PassParameters->gOut_Spec = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FinalAtrousOutputSpecular));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("split screen"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}
	



	// now queue up the history extraction
	if (!View.bStatePrevViewInfoIsReadOnly && History)
	{
		//UE_LOG(LogNRD, Log, TEXT("%s QueueTextureExtraction Enter"), ANSI_TO_TCHAR(__FUNCTION__));
		GraphBuilder.QueueTextureExtraction(FireflySpecularAndDiffuseIlluminationLogLuv, &History->SpecularAndDiffuseIlluminationLogLuv);
		GraphBuilder.QueueTextureExtraction(DisocclusionFixSpecularAndDiffuseIlluminationResponsiveLogLuv, &History->SpecularAndDiffuseIlluminationResponsiveLogLuv);
		GraphBuilder.QueueTextureExtraction(FireflySpecularIlluminationUnpacked, &History->SpecularIlluminationUnpacked);
		GraphBuilder.QueueTextureExtraction(FireflyDiffuseIlluminationUnpacked, &History->DiffuseIlluminationUnpacked);
		GraphBuilder.QueueTextureExtraction(DisocclusionFixSpecularAndDiffuse2ndMoments, &History->SpecularAndDiffuse2ndMoments);
		GraphBuilder.QueueTextureExtraction(NormalRoughnessDepth, &History->NormalRoughnessDepth);
		GraphBuilder.QueueTextureExtraction(ReprojectReflectionHitT, &History->ReflectionHitT);
		GraphBuilder.QueueTextureExtraction(HistoryClampingSpecularAndDiffuseHistoryLength, &History->SpecularAndDiffuseHistoryLength);
		++History->FrameIndex;
		//UE_LOG(LogNRD, Log, TEXT("%s QueueTextureExtraction Leave"), ANSI_TO_TCHAR(__FUNCTION__));
	}

	FRelaxOutputs Outputs;

	Outputs.Diffuse = FinalAtrousOutputDiffuse;
	Outputs.Specular = FinalAtrousOutputSpecular;

	//UE_LOG(LogNRD, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
	return Outputs;
}