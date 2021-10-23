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

#include "NRDDenoiser.h"
#include "NRDDenoiserHistory.h"
#include "NRDPrivate.h"
#include "ReLAX.h"
#include "NRDEngineBridge.h"

#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

DECLARE_GPU_STAT(NRDRelaxDiffuseSpecularDenoiser);

FNRDDenoiser::FNRDDenoiser(const IScreenSpaceDenoiser* InWrappedDenoiser)
	: WrappedDenoiser(InWrappedDenoiser)
{
	check(WrappedDenoiser);
}

const TCHAR* FNRDDenoiser::GetDebugName() const
{
	return TEXT("FNRDDenoiser");
}

IScreenSpaceDenoiser::EShadowRequirements FNRDDenoiser::GetShadowRequirements(const FViewInfo& View, const FLightSceneInfo& LightSceneInfo, const FShadowRayTracingConfig& RayTracingConfig) const
{
	return WrappedDenoiser->GetShadowRequirements(View, LightSceneInfo, RayTracingConfig);
}

void FNRDDenoiser::DenoiseShadowVisibilityMasks(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const TStaticArray<FShadowVisibilityParameters, IScreenSpaceDenoiser::kMaxBatchSize>& InputParameters, const int32 InputParameterCount, TStaticArray<FShadowVisibilityOutputs, IScreenSpaceDenoiser::kMaxBatchSize>& Outputs) const
{
	WrappedDenoiser->DenoiseShadowVisibilityMasks(GraphBuilder, View, PreviousViewInfos, SceneTextures, InputParameters, InputParameterCount, Outputs);
}

IScreenSpaceDenoiser::FPolychromaticPenumbraOutputs FNRDDenoiser::DenoisePolychromaticPenumbraHarmonics(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FPolychromaticPenumbraHarmonics& Inputs) const
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, NRDRelaxDiffuseSpecularDenoiser);
	RDG_EVENT_SCOPE(GraphBuilder, "NRD");
	
	
	checkf(Inputs.Diffuse.Harmonics[0], TEXT("diffuse channel input not set correctly from the calling code"));
	checkf(Inputs.Diffuse.Harmonics[1], TEXT("diffuse ray hit distance input not set correctly from the calling code"));
	checkf(Inputs.Specular.Harmonics[0], TEXT("specular channel input not set correctly from the calling code"));
	checkf(Inputs.Specular.Harmonics[1], TEXT("specular ray hit distance input not set correctly from the calling code"));

	FEngineResources EngineResources(SceneTextures);
	EngineResources.Diffuse = Inputs.Diffuse.Harmonics[0];
	EngineResources.DiffuseRayHitDistance = Inputs.Diffuse.Harmonics[1];
	EngineResources.Specular = Inputs.Specular.Harmonics[0];
	EngineResources.SpecularRayHitDistance = Inputs.Specular.Harmonics[1];

	const FNRDPackInputsArguments PackArguments = RelaxPackInputArguments();
	
	FNRDResources NRDInputResources = NRDPackInputs(
		GraphBuilder, View, 
		EngineResources,
		PackArguments
	);

	FRelaxPassParameters  RelaxInputs;
	RelaxInputs.MotionVectors = NRDInputResources.MotionVectors;
	RelaxInputs.ViewZ = NRDInputResources.ViewDepth;
	RelaxInputs.NormalAndRoughness = NRDInputResources.NormalAndRoughness;

	RelaxInputs.Diffuse = NRDInputResources.DiffuseHit;
	RelaxInputs.Specular = NRDInputResources.SpecularHit;

	// unpack the history from the UE4 data structures
	const TRefCountPtr<ICustomDenoisePolychromaticPenumbraHarmonicsHistory> InputCustomHistoryInterface = View.PrevViewInfo.CustomDenoisePolychromaticPenumbraHarmonicsHistory;
	TRefCountPtr < ICustomDenoisePolychromaticPenumbraHarmonicsHistory >* OutputCustomHistoryInterface = View.ViewState ? &(View.ViewState->PrevFrameViewInfo.CustomDenoisePolychromaticPenumbraHarmonicsHistory) : nullptr;
	const FNRDDenoisePolychromaticPenumbraHarmonicsHistory* InputRelaxCustomHistory = static_cast<const FNRDDenoisePolychromaticPenumbraHarmonicsHistory*>(InputCustomHistoryInterface.GetReference());

	const bool bCameraCut = !InputCustomHistoryInterface.IsValid() || View.bCameraCut || !OutputCustomHistoryInterface;
	const FIntPoint BufferSize = NRDInputResources.DiffuseHit->Desc.Extent;
	FNRDRelaxHistoryRef RelaxHistory = (bCameraCut || !InputRelaxCustomHistory) ? MakeShared<FNRDRelaxHistory, ESPMode::ThreadSafe>(BufferSize) : InputRelaxCustomHistory->RelaxHistory;
	
	// We ignore the ViewRect position since NRDPackInputs moves the ViewRect for NRD to 0,0
	if (RelaxHistory->HistorySize != View.ViewRect.Size())
	{
		UE_LOG(LogNRD, Log, TEXT("Resizing FNRDRelaxHistory = %p HasValidResources=%u HistorySize=%s FrameIndex = %u"), 
								RelaxHistory.Get(), RelaxHistory->HasValidResources(), *RelaxHistory->HistorySize.ToString(), RelaxHistory->FrameIndex );
		RelaxHistory = MakeShared<FNRDRelaxHistory, ESPMode::ThreadSafe>( BufferSize);
	}

	FRelaxOutputs RelaxOutputs = AddRelaxPasses(GraphBuilder, View, RelaxInputs, RelaxHistory);

	IScreenSpaceDenoiser::FPolychromaticPenumbraOutputs DenoiserOutputs;
	
	const bool bCopyOutputs = RelaxOutputs.Diffuse->Desc.Extent != EngineResources.Diffuse->Desc.Extent;

	{
		RDG_EVENT_SCOPE(GraphBuilder, "NRD Unpack Outputs");
		DenoiserOutputs.Diffuse  = !bCopyOutputs ? RelaxOutputs.Diffuse  : NRDUnpackOutput(GraphBuilder, View, RelaxOutputs.Diffuse,  EngineResources.Diffuse->Desc,  TEXT("SampledLightDiffuseDenoised"));
		DenoiserOutputs.Specular = !bCopyOutputs ? RelaxOutputs.Specular : NRDUnpackOutput(GraphBuilder, View, RelaxOutputs.Specular, EngineResources.Specular->Desc, TEXT("SampledLightSpecularDenoised"));
	}

	// pack Relax history back into the UE4 data structures;
	if (!View.bStatePrevViewInfoIsReadOnly && OutputCustomHistoryInterface)
	{
		if (!OutputCustomHistoryInterface->GetReference())
		{
			(*OutputCustomHistoryInterface) = new FNRDDenoisePolychromaticPenumbraHarmonicsHistory(RelaxHistory);
		}
	}
	return DenoiserOutputs;
}

IScreenSpaceDenoiser::FReflectionsOutputs FNRDDenoiser::DenoiseReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const
{ 
	return WrappedDenoiser->DenoiseReflections(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FReflectionsOutputs FNRDDenoiser::DenoiseWaterReflections(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FReflectionsInputs& Inputs, const FReflectionsRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseWaterReflections(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FAmbientOcclusionOutputs FNRDDenoiser::DenoiseAmbientOcclusion(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FAmbientOcclusionInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseAmbientOcclusion(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FNRDDenoiser::DenoiseDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseDiffuseIndirect(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FNRDDenoiser::DenoiseSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseSkyLight(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FNRDDenoiser::DenoiseReflectedSkyLight(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseReflectedSkyLight(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

IScreenSpaceDenoiser::FDiffuseIndirectHarmonic FNRDDenoiser::DenoiseDiffuseIndirectHarmonic(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectHarmonic& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseDiffuseIndirectHarmonic(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

bool FNRDDenoiser::SupportsScreenSpaceDiffuseIndirectDenoiser(EShaderPlatform Platform) const
{
	return WrappedDenoiser->SupportsScreenSpaceDiffuseIndirectDenoiser(Platform);
}

IScreenSpaceDenoiser::FDiffuseIndirectOutputs FNRDDenoiser::DenoiseScreenSpaceDiffuseIndirect(FRDGBuilder& GraphBuilder, const FViewInfo& View, FPreviousViewInfo* PreviousViewInfos, const FSceneTextureParameters& SceneTextures, const FDiffuseIndirectInputs& Inputs, const FAmbientOcclusionRayTracingConfig Config) const
{
	return WrappedDenoiser->DenoiseScreenSpaceDiffuseIndirect(GraphBuilder, View, PreviousViewInfos, SceneTextures, Inputs, Config);
}

const IScreenSpaceDenoiser* FNRDDenoiser::GetWrappedDenoiser() const
{
	return WrappedDenoiser;
}
