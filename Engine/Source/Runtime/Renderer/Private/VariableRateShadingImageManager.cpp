// Copyright Epic Games, Inc. All Rights Reserved.

#include "VariableRateShadingImageManager.h"
#include "StereoRenderTargetManager.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHIDefinitions.h"
#include "RenderTargetPool.h"
#include "SystemTextures.h"
#include "SceneView.h"
#include "IEyeTracker.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"

const int32 kComputeGroupSize = FComputeShaderUtils::kGolden2DGroupSize;

enum class EVRSGenerationFlags : uint32
{
	None = 0x0,
	StereoRendering = 0x1,
	SideBySideStereo = 0x2,
	HMDFixedFoveation = 0x4,
	HMDEyeTrackedFoveation = 0x8,
};

ENUM_CLASS_FLAGS(EVRSGenerationFlags);

const int32 kMaxCombinedSources = 4;

static TAutoConsoleVariable<int> CVarHMDFixedFoveationLevel(
	TEXT("vr.VRS.HMDFixedFoveationLevel"),
	0,
	TEXT("Level of fixed-foveation VRS to apply (when Variable Rate Shading is available)\n")
	TEXT(" 0: Disabled (default);\n")
	TEXT(" 1: Low;\n")
	TEXT(" 2: Medium;\n")
	TEXT(" 3: High;\n"),
	// @todo: 4 - Dynamic (adjusts based on framerate)
	ECVF_RenderThreadSafe);

TGlobalResource<FVariableRateShadingImageManager> GVRSImageManager;

class FComputeVariableRateShadingImageGeneration : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeVariableRateShadingImageGeneration);
	SHADER_USE_PARAMETER_STRUCT(FComputeVariableRateShadingImageGeneration, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWOutputTexture)
		SHADER_PARAMETER_RDG_TEXTURE_ARRAY(Texture2D<uint>, CombineSourceIn, [kMaxCombinedSources])
		SHADER_PARAMETER(FVector2D, HMDFieldOfView)
		SHADER_PARAMETER(FVector2D, LeftEyeCenterPixelXY)
		SHADER_PARAMETER(FVector2D, RightEyeCenterPixelXY)
		SHADER_PARAMETER(float, ViewDiagonalSquaredInPixels)
		SHADER_PARAMETER(float, FixedFoveationFullRateCutoffSquared)
		SHADER_PARAMETER(float, FixedFoveationHalfRateCutoffSquared)
		SHADER_PARAMETER(uint32, CombineSourceCount)
		SHADER_PARAMETER(uint32, ShadingRateAttachmentGenerationFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVariableRateShading(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// Shading rates:
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_1x1"), VRSSR_1x1);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_1x2"), VRSSR_1x2);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_2x1"), VRSSR_2x1);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_2x2"), VRSSR_2x2);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_2x4"), VRSSR_2x4);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_4x2"), VRSSR_4x2);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_4x4"), VRSSR_4x4);

		OutEnvironment.SetDefine(TEXT("MAX_COMBINED_SOURCES_IN"), kMaxCombinedSources);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_TILE_WIDTH"), GRHIVariableRateShadingImageTileMinWidth);
		OutEnvironment.SetDefine(TEXT("SHADING_RATE_TILE_HEIGHT"), GRHIVariableRateShadingImageTileMinHeight);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), kComputeGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), kComputeGroupSize);

		OutEnvironment.SetDefine(TEXT("STEREO_RENDERING"), (uint32)EVRSGenerationFlags::StereoRendering);
		OutEnvironment.SetDefine(TEXT("SIDE_BY_SIDE_STEREO"), (uint32)EVRSGenerationFlags::SideBySideStereo);
		OutEnvironment.SetDefine(TEXT("HMD_FIXED_FOVEATION"), (uint32)EVRSGenerationFlags::HMDFixedFoveation);
		OutEnvironment.SetDefine(TEXT("HMD_EYETRACKED_FOVEATION"), (uint32)EVRSGenerationFlags::HMDEyeTrackedFoveation);
	}
};

IMPLEMENT_GLOBAL_SHADER(FComputeVariableRateShadingImageGeneration, "/Engine/Private/VariableRateShading.usf", "GenerateShadingRateTexture", SF_Compute);

/**
 * Struct containing parameters used to build a VRS image.
 */
struct FVRSImageGenerationParameters
{
	FIntPoint Size = FIntPoint(0, 0);

	FVector2D HMDFieldOfView = FVector2D(90.0f, 90.0f);
	FVector2D HMDEyeTrackedFoveationOrigin = FVector2D(0.0f, 0.0f);

	float HMDFixedFoveationFullRateCutoff = 1.0f;
	float HMDFixedFoveationHalfRateCutoff = 1.0f;
	float HMDEyeTrackedFoveationFullRateCutoff = 1.0f;
	float HMDEYeTrackedFoveationHalfRateCutoff = 1.0f;

	bool bGenerateFixedFoveation = false;
	bool bGenerateEyeTrackedFoveation = false;
	bool bInstancedStereo = false;
};

uint64 FVariableRateShadingImageManager::CalculateVRSImageHash(const FVRSImageGenerationParameters& GenerationParamsIn, EVRSGenerationFlags GenFlags) const
{
	uint64 Hash = GetTypeHash(GenerationParamsIn.HMDFieldOfView);
	Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.Size));
	Hash = HashCombine(Hash, GetTypeHash(GenFlags));

	// Currently the only view flag that differentiates is stereo, but down the road there might be others.
	if (EnumHasAllFlags(GenFlags, EVRSGenerationFlags::StereoRendering))
	{
		// Only hash these values for stereo rendering.
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.HMDFixedFoveationFullRateCutoff));
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.HMDFixedFoveationHalfRateCutoff));
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.HMDEyeTrackedFoveationOrigin));
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.HMDEyeTrackedFoveationFullRateCutoff));
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.HMDEYeTrackedFoveationHalfRateCutoff));
		Hash = HashCombine(Hash, GetTypeHash(GenerationParamsIn.bInstancedStereo));
	}

	return Hash;
}

FVariableRateShadingImageManager::FVariableRateShadingImageManager()
	: FRenderResource()
	, LastFrameTick(0)
{
}

FVariableRateShadingImageManager::~FVariableRateShadingImageManager()
{}

void FVariableRateShadingImageManager::ReleaseDynamicRHI()
{
	for (auto& Image : ActiveVRSImages)
	{
		Image.Value.Target.SafeRelease();
	}

	ActiveVRSImages.Empty();

	GRenderTargetPool.FreeUnusedResources();
}

FRDGTextureRef FVariableRateShadingImageManager::GetVariableRateShadingImage(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily, const TArray<TRefCountPtr<IPooledRenderTarget>>* ExternalVRSSources, EVRSType VRSTypesToExclude)
{
	// If the RHI doesn't support VRS, we should always bail immediately.
	if (!GRHISupportsAttachmentVariableRateShading || !GRHIVariableRateShadingEnabled || !GRHIAttachmentVariableRateShadingEnabled)
	{
		return nullptr;
	}

	// Always want to make sure we tick every frame, even if we're not going to be generating any VRS images.
	Tick();

	if (EnumHasAllFlags(VRSTypesToExclude, EVRSType::All))
	{
		return nullptr;
	}

	FVRSImageGenerationParameters VRSImageParams;

	const bool bIsStereo = IStereoRendering::IsStereoEyeView(*ViewFamily.Views[0]) && GEngine->XRSystem.IsValid();
	
	VRSImageParams.bInstancedStereo |= ViewFamily.Views[0]->IsInstancedStereoPass();
	VRSImageParams.Size = FIntPoint(ViewFamily.RenderTarget->GetSizeXY());

	UpdateFixedFoveationParameters(VRSImageParams);
	UpdateEyeTrackedFoveationParameters(VRSImageParams, ViewFamily);

	EVRSGenerationFlags GenFlags = EVRSGenerationFlags::None;

	// Setup generation flags for XR foveation VRS generation.
	if (bIsStereo && !EnumHasAnyFlags(VRSTypesToExclude, EVRSType::XRFoveation) && !EnumHasAnyFlags(VRSTypesToExclude, EVRSType::EyeTrackedFoveation))
	{
		EnumAddFlags(GenFlags, EVRSGenerationFlags::StereoRendering);

		if (!EnumHasAnyFlags(VRSTypesToExclude, EVRSType::FixedFoveation) && VRSImageParams.bGenerateFixedFoveation)
		{
			EnumAddFlags(GenFlags, EVRSGenerationFlags::HMDFixedFoveation);
		}

		if (!EnumHasAllFlags(VRSTypesToExclude, EVRSType::EyeTrackedFoveation) && VRSImageParams.bGenerateEyeTrackedFoveation)
		{
			EnumAddFlags(GenFlags, EVRSGenerationFlags::HMDEyeTrackedFoveation);
		}

		if (VRSImageParams.bInstancedStereo)
		{
			EnumAddFlags(GenFlags, EVRSGenerationFlags::SideBySideStereo);
		}
	}

	// @todo: Other VRS generation flags here.

	if (GenFlags == EVRSGenerationFlags::None)
	{
		if (ExternalVRSSources == nullptr || ExternalVRSSources->Num() == 0)
		{
			// Nothing to generate.
			return nullptr;
		}
		else
		{
			// If there's one external VRS image, just return that since we're not building anything here.
			if (ExternalVRSSources->Num() == 1)
			{
				const FIntVector& ExtSize = (*ExternalVRSSources)[0]->GetDesc().GetSize();
				check(ExtSize.X == VRSImageParams.Size.X / GRHIVariableRateShadingImageTileMinWidth && ExtSize.Y == VRSImageParams.Size.Y / GRHIVariableRateShadingImageTileMinHeight);
				return GraphBuilder.RegisterExternalTexture((*ExternalVRSSources)[0]);
			}

			// If there is more than one external image, we'll generate a final one by combining, so fall through.
		}
	}

	IHeadMountedDisplay* HMDDevice = (GEngine->XRSystem == nullptr) ? nullptr : GEngine->XRSystem->GetHMDDevice();
	if (HMDDevice != nullptr)
	{
		HMDDevice->GetFieldOfView(VRSImageParams.HMDFieldOfView.X, VRSImageParams.HMDFieldOfView.Y);
	}

	const uint64 Key = CalculateVRSImageHash(VRSImageParams, GenFlags);
	FActiveTarget* ActiveTarget = ActiveVRSImages.Find(Key);
	if (ActiveTarget == nullptr)
	{
		// Render it.
		return GraphBuilder.RegisterExternalTexture(RenderShadingRateImage(GraphBuilder, Key, VRSImageParams, GenFlags));
	}

	ActiveTarget->LastUsedFrame = GFrameNumber;

	return GraphBuilder.RegisterExternalTexture(ActiveTarget->Target);
}

TRefCountPtr<IPooledRenderTarget> FVariableRateShadingImageManager::RenderShadingRateImage(FRDGBuilder& GraphBuilder, uint64 Key, const FVRSImageGenerationParameters& VRSImageGenParamsIn, EVRSGenerationFlags GenFlags)
{
	// Sanity check VRS tile size.
	check(GRHIVariableRateShadingImageTileMinWidth >= 8 && GRHIVariableRateShadingImageTileMinWidth <= 64 && GRHIVariableRateShadingImageTileMinHeight >= 8 && GRHIVariableRateShadingImageTileMaxHeight <= 64);

	FIntPoint AttachmentSize(VRSImageGenParamsIn.Size.X / GRHIVariableRateShadingImageTileMinWidth, VRSImageGenParamsIn.Size.Y / GRHIVariableRateShadingImageTileMinHeight);

	FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
		AttachmentSize,
		GRHIVariableRateShadingImageFormat,
		FClearValueBinding::None,
		TexCreate_Foveation,
		TexCreate_UAV,
		false);

	TRefCountPtr<IPooledRenderTarget> Attachment;

	GRenderTargetPool.FindFreeElement(GraphBuilder.RHICmdList, Desc, Attachment, TEXT("VariableRateShadingAttachment"));

	if (!Attachment.IsValid())
	{
		return Attachment;
	}

	FRDGTextureRef RDGAttachment = GraphBuilder.RegisterExternalTexture(Attachment, ERenderTargetTexture::Targetable);

	FComputeVariableRateShadingImageGeneration::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeVariableRateShadingImageGeneration::FParameters>();
	PassParameters->RWOutputTexture = GraphBuilder.CreateUAV(RDGAttachment);

	PassParameters->HMDFieldOfView = VRSImageGenParamsIn.HMDFieldOfView;

	PassParameters->FixedFoveationFullRateCutoffSquared = VRSImageGenParamsIn.HMDFixedFoveationFullRateCutoff * VRSImageGenParamsIn.HMDFixedFoveationFullRateCutoff;
	PassParameters->FixedFoveationHalfRateCutoffSquared = VRSImageGenParamsIn.HMDFixedFoveationHalfRateCutoff * VRSImageGenParamsIn.HMDFixedFoveationHalfRateCutoff;

	PassParameters->LeftEyeCenterPixelXY = FVector2D(AttachmentSize.X / 2, AttachmentSize.Y / 2);
	PassParameters->RightEyeCenterPixelXY = PassParameters->LeftEyeCenterPixelXY;

	// If instanced (side-by-side) stereo, there's two "center" points, so adjust both eyes
	if (VRSImageGenParamsIn.bInstancedStereo)
	{
		PassParameters->LeftEyeCenterPixelXY.X /= 2;
		PassParameters->RightEyeCenterPixelXY.X = PassParameters->LeftEyeCenterPixelXY.X + AttachmentSize.X / 2;
	}

	PassParameters->ViewDiagonalSquaredInPixels = FVector2D::DotProduct(PassParameters->LeftEyeCenterPixelXY, PassParameters->LeftEyeCenterPixelXY);
	PassParameters->CombineSourceCount = 0;
	PassParameters->ShadingRateAttachmentGenerationFlags = (uint32)GenFlags;

	// @todo: When we have other VRS sources to combine in.
	for (uint32 i = 0; i < kMaxCombinedSources; ++i)
	{
		PassParameters->CombineSourceIn[i] = GraphBuilder.RegisterExternalTexture(GSystemTextures.WhiteDummy, TEXT("CombineSourceDummy"));
	}

	TShaderMapRef<FComputeVariableRateShadingImageGeneration> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(AttachmentSize, FComputeShaderUtils::kGolden2DGroupSize);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GenerateVariableRateShadingImage"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
	{
		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
	});

	ActiveVRSImages.Add(Key, FActiveTarget(Attachment));

	return Attachment;
}

TRefCountPtr<IPooledRenderTarget> FVariableRateShadingImageManager::GetMobileVariableRateShadingImage(const FSceneViewFamily& ViewFamily)
{
	if (!(IStereoRendering::IsStereoEyeView(*ViewFamily.Views[0]) && GEngine->XRSystem.IsValid()))
	{
		return TRefCountPtr<IPooledRenderTarget>();
	}

	FIntPoint Size(ViewFamily.RenderTarget->GetSizeXY());

	const bool bStereo = GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled();
	IStereoRenderTargetManager* const StereoRenderTargetManager = bStereo ? GEngine->StereoRenderingDevice->GetRenderTargetManager() : nullptr;

	FTexture2DRHIRef Texture;
	FIntPoint TextureSize(0, 0);

	// Allocate variable resolution texture for VR foveation if supported
	if (StereoRenderTargetManager && StereoRenderTargetManager->NeedReAllocateShadingRateTexture(MobileHMDFixedFoveationOverrideImage))
	{
		bool bAllocatedShadingRateTexture = StereoRenderTargetManager->AllocateShadingRateTexture(0, Size.X, Size.Y, GRHIVariableRateShadingImageFormat, 0, TexCreate_None, TexCreate_None, Texture, TextureSize);
		if (bAllocatedShadingRateTexture)
		{
			MobileHMDFixedFoveationOverrideImage = CreateRenderTarget(Texture, TEXT("ShadingRate"));
		}
	}

	return MobileHMDFixedFoveationOverrideImage;
}

void FVariableRateShadingImageManager::Tick()
{
	static const uint64 RecycleTargetAfterNFrames = 12;

	uint64 FrameNumber = GFrameNumber;

	if (LastFrameTick == GFrameNumber)
	{
		return;
	}

	TArray<uint64> RemoveKeys;

	for (auto& ActiveTarget : ActiveVRSImages)
	{
		if ((GFrameNumber - ActiveTarget.Value.LastUsedFrame) > RecycleTargetAfterNFrames)
		{
			ActiveTarget.Value.Target.SafeRelease();
			RemoveKeys.Add(ActiveTarget.Key);
		}
	}

	for (uint64 Key : RemoveKeys)
	{
		ActiveVRSImages.Remove(Key);
	}

	LastFrameTick = FrameNumber;
}

void FVariableRateShadingImageManager::UpdateFixedFoveationParameters(FVRSImageGenerationParameters& VRSImageGenParamsInOut)
{
	static const float FIXED_FOVEATION_FULL_RATE_CUTOFFS[] = { 1.0f, 0.7f, 0.50f, 0.35f };
	static const float FIXED_FOVEATION_HALF_RATE_CUTOFFS[] = { 1.0f, 0.9f, 0.75f, 0.45f };

	int Level = FMath::Clamp(CVarHMDFixedFoveationLevel.GetValueOnAnyThread(), 0, 3);

	VRSImageGenParamsInOut.bGenerateFixedFoveation = Level ? true : false;
	VRSImageGenParamsInOut.HMDFixedFoveationFullRateCutoff = FIXED_FOVEATION_FULL_RATE_CUTOFFS[Level];
	VRSImageGenParamsInOut.HMDFixedFoveationHalfRateCutoff = FIXED_FOVEATION_HALF_RATE_CUTOFFS[Level];
}

void FVariableRateShadingImageManager::UpdateEyeTrackedFoveationParameters(FVRSImageGenerationParameters& VRSImageGenParamsInOut, const FSceneViewFamily& ViewFamily)
{
	VRSImageGenParamsInOut.bGenerateEyeTrackedFoveation = false;
	VRSImageGenParamsInOut.HMDEyeTrackedFoveationFullRateCutoff = 1.0f;
	VRSImageGenParamsInOut.HMDEYeTrackedFoveationHalfRateCutoff = 1.0f;

	auto EyeTracker = GEngine->EyeTrackingDevice;
	if (!EyeTracker.IsValid())
	{
		return;
	}

	FEyeTrackerGazeData GazeData;
	if (!EyeTracker->GetEyeTrackerGazeData(GazeData))
	{
		return;
	}

	// @todo:
}
