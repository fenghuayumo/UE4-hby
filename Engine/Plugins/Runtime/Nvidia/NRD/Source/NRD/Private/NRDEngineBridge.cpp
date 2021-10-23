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

#include "NRDEngineBridge.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

#include "Shader.h"
#include "ScreenPass.h"
#include "ShaderCore.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"




const int32 kVelocityCombineComputeTileSizeX = FComputeShaderUtils::kGolden2DGroupSize;
const int32 kVelocityCombineComputeTileSizeY = FComputeShaderUtils::kGolden2DGroupSize;

class FPackDiffuseRayDistanceDim : SHADER_PERMUTATION_BOOL("PACK_DIFFUSE_RAY_DISTANCE");
class FCompressDiffuseRadianceDim : SHADER_PERMUTATION_BOOL("COMPRESS_DIFFUSE_RADIANCE");

class FPackSpecularRayDistanceDim : SHADER_PERMUTATION_BOOL("PACK_SPECULAR_RAY_DISTANCE");
class FCompressSpecularRadianceDim : SHADER_PERMUTATION_BOOL("COMPRESS_SPECULAR_RADIANCE");



class FNRDPackInputsCS : public FGlobalShader
{
public: 
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), FComputeShaderUtils::kGolden2DGroupSize);
	}
	using FPermutationDomain = TShaderPermutationDomain<
		FPackDiffuseRayDistanceDim,
		FCompressDiffuseRadianceDim,
		FPackSpecularRayDistanceDim,
		FCompressSpecularRadianceDim
	>;

	DECLARE_GLOBAL_SHADER(FNRDPackInputsCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDPackInputsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		/// Input images
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, Diffuse)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DiffuseRayHitDistance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, Specular)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SpecularRayHitDistance)
		
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InBuffer)

		// Output images

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutViewDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutMotionVectors)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutNormalAndRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDiffuseHit)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutSpecularHit)

		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, OutBuffer)

		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FNRDPackInputsCS, "/Plugin/NRD/Private/NRDEngineBridge.usf", "NRDPackInputsMainCS", SF_Compute);


NRD_API FNRDResources NRDPackInputs( FRDGBuilder& GraphBuilder, const FViewInfo& View, 
	const FEngineResources& InEngineResources,
	const FNRDPackInputsArguments& InArguments)
{
	// pack and copy the data from the engine images to how NRD expects them
	// resizes the NRD input images to the view rect size
	// shifts the viewrect in the output to the origin 
	const FIntRect InputViewRect = View.ViewRect; 
	const FIntRect OutputViewRect = FIntRect(FIntPoint::ZeroValue, InputViewRect.Size());
	const FIntPoint OutputBufferSize = OutputViewRect.Size();

	auto CreateOutputTexture = [&GraphBuilder, OutputBufferSize](EPixelFormat Format, const TCHAR* DebugName)
	{ 
		return GraphBuilder.CreateTexture( 
			FRDGTextureDesc::Create2D(
				OutputBufferSize, Format, FClearValueBinding::Black,	
				TexCreate_ShaderResource | TexCreate_UAV),
			DebugName
		);
	};

	// Create the NRD resources
	FNRDResources Resources;
	Resources.ViewDepth = CreateOutputTexture(PF_R32_FLOAT, TEXT("NRD.Input.ViewDepth"));
	Resources.MotionVectors = CreateOutputTexture(PF_FloatRGBA, TEXT("NRD.Input.MotionVectors"));
	Resources.NormalAndRoughness = CreateOutputTexture(PF_R8G8B8A8, TEXT("NRD.Input.NormalAndRoughness"));
	
	// TODO change texture format based on whether we pack or not? Probably not since that might require shader changes
	Resources.DiffuseHit = CreateOutputTexture(PF_FloatRGBA, InArguments.bPackDiffuseHitDistance ? TEXT("NRD.Input.DiffuseHit"): TEXT("NRD.Input.Diffuse"));
	Resources.SpecularHit = CreateOutputTexture(PF_FloatRGBA, InArguments.bPackSpecularHitDistance ? TEXT("NRD.Input.SpecularHit") : TEXT("NRD.Input.Specular"));

	// Pick the shader permutation
	FNRDPackInputsCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FPackDiffuseRayDistanceDim>(InArguments.bPackDiffuseHitDistance);
	PermutationVector.Set<FCompressDiffuseRadianceDim>(InArguments.bCompressDiffuseRadiance);
	
	PermutationVector.Set<FPackSpecularRayDistanceDim>(InArguments.bPackSpecularHitDistance);
	PermutationVector.Set<FCompressSpecularRadianceDim>(InArguments.bCompressSpecularRadiance);

	TShaderMapRef<FNRDPackInputsCS> ComputeShader(View.ShaderMap, PermutationVector);
	FNRDPackInputsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDPackInputsCS::FParameters>();

	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = InEngineResources.SceneTextures;
	PassParameters->Diffuse = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InEngineResources.Diffuse));
	PassParameters->DiffuseRayHitDistance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InEngineResources.DiffuseRayHitDistance));
	PassParameters->Specular = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InEngineResources.Specular));
	PassParameters->SpecularRayHitDistance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InEngineResources.SpecularRayHitDistance));

	// buffer coordinate transforms to read from the input buffers
	// assumes that the gbuffer as well as specular and diffuse have the same size & location in the viewrect
	{
		check(InEngineResources.Diffuse && InEngineResources.Diffuse->Desc.Extent == PassParameters->SceneTextures.GBufferATexture->Desc.Extent);
		check(InEngineResources.Specular && InEngineResources.Specular->Desc.Extent == PassParameters->SceneTextures.GBufferATexture->Desc.Extent);

		FScreenPassTextureViewport InBufferViewport(PassParameters->SceneTextures.GBufferATexture, InputViewRect);
		FScreenPassTextureViewportParameters InBufferViewportParameters = GetScreenPassTextureViewportParameters(InBufferViewport);
		PassParameters->InBuffer = InBufferViewportParameters;
	}

	PassParameters->OutViewDepth = GraphBuilder.CreateUAV(Resources.ViewDepth);
	PassParameters->OutMotionVectors = GraphBuilder.CreateUAV(Resources.MotionVectors);
	PassParameters->OutNormalAndRoughness = GraphBuilder.CreateUAV(Resources.NormalAndRoughness);
	PassParameters->OutDiffuseHit = GraphBuilder.CreateUAV(Resources.DiffuseHit);
	PassParameters->OutSpecularHit = GraphBuilder.CreateUAV(Resources.SpecularHit);

	// buffer coordinate transforms to write to the output buffers
	{
		FScreenPassTextureViewport OutBufferViewport(Resources.ViewDepth, OutputViewRect);
		FScreenPassTextureViewportParameters OutBufferViewportParameters = GetScreenPassTextureViewportParameters(OutBufferViewport);
		PassParameters->OutBuffer = OutBufferViewportParameters;
	}


	ClearUnusedGraphResources(ComputeShader, PassParameters);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("NRD Pack Inputs"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(OutputViewRect.Size(), FComputeShaderUtils::kGolden2DGroupSize)
	);
	return Resources;
}


NRD_API FRDGTextureRef NRDUnpackOutput(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef InNRDResource,
	const FRDGTextureDesc& OutputDesc,
	const TCHAR* Name
)
{
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, Name);

		AddDrawTexturePass(GraphBuilder, View, InNRDResource, OutputTexture,
			FIntPoint::ZeroValue, View.ViewRect.Min, View.ViewRect.Size());
		return OutputTexture;
}

