// Copyright Epic Games, Inc. All Rights Reserved.

#include "RayTracingIESLightProfiles.h"
#include "SceneRendering.h"
#include "CopyTextureShaders.h"

#if RHI_RAYTRACING

void FIESLightProfileResource::BuildIESLightProfilesTexture(FRHICommandListImmediate& RHICmdList, const TArray<UTextureLightProfile*, SceneRenderingAllocator>& NewIESProfilesArray)
{
	// Rebuild 2D texture that contains one IES light profile per row

	check(IsInRenderingThread());

	bool NeedsRebuild = false;
	if (NewIESProfilesArray.Num() != IESTextureData.Num())
	{
		NeedsRebuild = true;
		IESTextureData.SetNum(NewIESProfilesArray.Num(), true);
	}
	else
	{
		for (int32 i = 0; i < IESTextureData.Num(); ++i)
		{
			if (IESTextureData[i] != NewIESProfilesArray[i])
			{
				NeedsRebuild = true;
				break;
			}
		}
	}

	uint32 NewArraySize = NewIESProfilesArray.Num();

	if (!NeedsRebuild || NewArraySize == 0)
	{
		return;
	}

	if (!DefaultTexture)
	{
		FRHIResourceCreateInfo CreateInfo;
		CreateInfo.DebugName = TEXT("RTDefaultIESProfile");
		DefaultTexture = RHICreateTexture2D(AllowedIESProfileWidth, 1, AllowedIESProfileFormat, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
		FUnorderedAccessViewRHIRef UAV = RHICreateUnorderedAccessView(DefaultTexture, 0);

		RHICmdList.Transition(FRHITransitionInfo(DefaultTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.ClearUAVFloat(UAV, FVector4(1.0f, 1.0f, 1.0f, 1.0f));
		RHICmdList.Transition(FRHITransitionInfo(DefaultTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
	}

	if (!AtlasTexture || AtlasTexture->GetSizeY() != NewArraySize)
	{
		FRHIResourceCreateInfo CreateInfo;
		CreateInfo.DebugName = TEXT("RTIESProfileAtlas");
		AtlasTexture = RHICreateTexture2D(AllowedIESProfileWidth, NewArraySize, AllowedIESProfileFormat, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
		AtlasUAV = RHICreateUnorderedAccessView(AtlasTexture, 0);
	}

	CopyTextureCS::DispatchContext DispatchContext;
	TShaderRef<FCopyTextureCS> Shader = FCopyTextureCS::SelectShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), 
		ECopyTextureResourceType::Texture2D, // SrcType
		ECopyTextureResourceType::Texture2D, // DstType
		ECopyTextureValueType::Float,
		DispatchContext); // out DispatchContext
	FRHIComputeShader* ShaderRHI = Shader.GetComputeShader();

	RHICmdList.Transition(FRHITransitionInfo(AtlasUAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.SetComputeShader(ShaderRHI);
	RHICmdList.SetUAVParameter(ShaderRHI, Shader->GetDstResourceParam().GetBaseIndex(), AtlasUAV);
	RHICmdList.BeginUAVOverlap(AtlasUAV);
	for (uint32 ProfileIndex = 0; ProfileIndex < NewArraySize; ++ProfileIndex)
	{
		IESTextureData[ProfileIndex] = NewIESProfilesArray[ProfileIndex];
		const UTextureLightProfile* LightProfileTexture = IESTextureData[ProfileIndex];

		FTextureRHIRef ProfileTexture; 
		if (IsIESTextureFormatValid(LightProfileTexture))
		{
			ProfileTexture = LightProfileTexture->Resource->TextureRHI;
		}
		else
		{
			ProfileTexture = DefaultTexture;
		}
		RHICmdList.SetShaderTexture(ShaderRHI, Shader->GetSrcResourceParam().GetBaseIndex(), ProfileTexture);
		Shader->Dispatch(RHICmdList, DispatchContext,
			FIntVector(0, 0, 0), // SrcOffset
			FIntVector(0, ProfileIndex, 0), // DstOffset
			FIntVector(AllowedIESProfileWidth, 1, 1));
	}
	RHICmdList.EndUAVOverlap(AtlasUAV);
	RHICmdList.Transition(FRHITransitionInfo(AtlasUAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
}

bool FIESLightProfileResource::IsIESTextureFormatValid(const UTextureLightProfile* Texture) const
{
	if (Texture
		&& Texture->Resource
		&& Texture->Resource->TextureRHI
		&& Texture->PlatformData
		&& Texture->PlatformData->PixelFormat == AllowedIESProfileFormat
		&& Texture->PlatformData->Mips.Num() == 1
		&& Texture->PlatformData->Mips[0].SizeX == AllowedIESProfileWidth
		//#dxr_todo: UE-70840 anisotropy in IES files is ignored so far (to support that, we should not store one IES profile per row but use more than one row per profile in that case)
		&& Texture->PlatformData->Mips[0].SizeY == 1
		)
	{
		return true;
	}
	else
	{
		return false;
	}
}


void FIESLightProfile2DResource::BuildIESLightProfilesTexture(FRHICommandListImmediate& RHICmdList, const TArray<UTextureLightProfile*, SceneRenderingAllocator>& NewIESProfilesArray)
{
	// Rebuild 2D texture that contains one IES light profile per row

	check(IsInRenderingThread());

	bool NeedsRebuild = false;
	if (NewIESProfilesArray.Num() != IESTextureData.Num())
	{
		NeedsRebuild = true;
		IESTextureData.SetNum(NewIESProfilesArray.Num(), true);
		IESIndexData.SetNum(NewIESProfilesArray.Num(), true);
	}
	else
	{
		for (int32 i = 0; i < IESTextureData.Num(); ++i)
		{
			if (IESTextureData[i] != NewIESProfilesArray[i])
			{
				NeedsRebuild = true;
				break;
			}
		}
	}

	if (!NeedsRebuild || NewIESProfilesArray.Num() == 0)
	{
		return;
	}

	uint32 NumProfiles = NewIESProfilesArray.Num();

	// pack 1D profiles and determine how many total pages we need
	uint32 NewArraySize = 0;
	uint32 OneDProfileCount = 0;

	for (uint32 ProfileIndex = 0; ProfileIndex < NumProfiles; ++ProfileIndex)
	{
		IESTextureData[ProfileIndex] = NewIESProfilesArray[ProfileIndex];
		const UTextureLightProfile* LightProfileTexture = IESTextureData[ProfileIndex];

		if (IsIESTextureFormatValid(LightProfileTexture))
		{
			if (LightProfileTexture->PlatformData->Mips[0].SizeY == 1)
			{
				OneDProfileCount++;
			}
			else
			{
				NewArraySize++;
			}
		}
		else
		{
			// invalid textures get the default in a 1D slot
			OneDProfileCount++;
		}
	}

	const uint32 OneDPageCount = FMath::DivideAndRoundUp(OneDProfileCount, AllowedIESProfileDim);
	NewArraySize += OneDPageCount;

	if (!DefaultTexture)
	{
		FRHIResourceCreateInfo CreateInfo;
		CreateInfo.DebugName = TEXT("RTDefaultIESProfile2D");
		DefaultTexture = RHICreateTexture2D(AllowedIESProfileDim, 1, AllowedIESProfileFormat, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
		FUnorderedAccessViewRHIRef UAV = RHICreateUnorderedAccessView(DefaultTexture, 0);

		RHICmdList.Transition(FRHITransitionInfo(DefaultTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.ClearUAVFloat(UAV, FVector4(1.0f, 1.0f, 1.0f, 1.0f));
		RHICmdList.Transition(FRHITransitionInfo(DefaultTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
	}

	if (!AtlasTexture || AtlasTexture->GetSizeZ() != NewArraySize)
	{
		FRHIResourceCreateInfo CreateInfo;
		CreateInfo.DebugName = TEXT("RTIESProfile2DAtlas");
		AtlasTexture = RHICreateTexture2DArray(AllowedIESProfileDim, AllowedIESProfileDim, NewArraySize, AllowedIESProfileFormat, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
		AtlasUAV = RHICreateUnorderedAccessView(AtlasTexture, 0);
	}

	CopyTextureCS::DispatchContext DispatchContext;
	TShaderRef<FCopyTextureCS> Shader = FCopyTextureCS::SelectShader(GetGlobalShaderMap(GMaxRHIFeatureLevel),
		ECopyTextureResourceType::Texture2D, // SrcType
		ECopyTextureResourceType::Texture2DArray, // DstType
		ECopyTextureValueType::Float,
		DispatchContext); // out DispatchContext
	FRHIComputeShader* ShaderRHI = Shader.GetComputeShader();

	RHICmdList.Transition(FRHITransitionInfo(AtlasUAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.SetComputeShader(ShaderRHI);
	RHICmdList.SetUAVParameter(ShaderRHI, Shader->GetDstResourceParam().GetBaseIndex(), AtlasUAV);
	RHICmdList.BeginUAVOverlap(AtlasUAV);

	uint32 Current1DSlot = 0;
	uint32 Current2DSlot = OneDPageCount;

	for (uint32 ProfileIndex = 0; ProfileIndex < NumProfiles; ++ProfileIndex)
	{
		const UTextureLightProfile* LightProfileTexture = IESTextureData[ProfileIndex];

		FTextureRHIRef ProfileTexture;
		bool OneD = false;
		if (IsIESTextureFormatValid(LightProfileTexture))
		{
			ProfileTexture = LightProfileTexture->Resource->TextureRHI;
			OneD = LightProfileTexture->PlatformData->Mips[0].SizeY == 1;
		}
		else
		{
			ProfileTexture = DefaultTexture;
			OneD = true;
		}

		if (OneD)
		{
			uint32 Y = Current1DSlot % AllowedIESProfileDim;
			uint32 Z = Current1DSlot / AllowedIESProfileDim;;
			RHICmdList.SetShaderTexture(ShaderRHI, Shader->GetSrcResourceParam().GetBaseIndex(), ProfileTexture);
			Shader->Dispatch(RHICmdList, DispatchContext,
				FIntVector(0, 0, 0), // SrcOffset
				FIntVector(0, Y, Z), // DstOffset
				FIntVector(AllowedIESProfileDim, 1, 1));

			IESIndexData[ProfileIndex].Page = Z;
			IESIndexData[ProfileIndex].Start = Y + 1;
			Current1DSlot++;
		}
		else
		{
			RHICmdList.SetShaderTexture(ShaderRHI, Shader->GetSrcResourceParam().GetBaseIndex(), ProfileTexture);
			Shader->Dispatch(RHICmdList, DispatchContext,
				FIntVector(0, 0, 0), // SrcOffset
				FIntVector(0, 0, Current2DSlot), // DstOffset
				FIntVector(AllowedIESProfileDim, AllowedIESProfileDim, 1));

			IESIndexData[ProfileIndex].Page = Current2DSlot;
			IESIndexData[ProfileIndex].Start = 0;

			Current2DSlot++;
		}
	}
	RHICmdList.EndUAVOverlap(AtlasUAV);
	RHICmdList.Transition(FRHITransitionInfo(AtlasUAV, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
}

bool FIESLightProfile2DResource::IsIESTextureFormatValid(const UTextureLightProfile* Texture) const
{
	if (Texture
		&& Texture->Resource
		&& Texture->Resource->TextureRHI
		&& Texture->PlatformData
		&& Texture->PlatformData->PixelFormat == AllowedIESProfileFormat
		&& Texture->PlatformData->Mips.Num() == 1
		&& Texture->PlatformData->Mips[0].SizeX == AllowedIESProfileDim
		&&(Texture->PlatformData->Mips[0].SizeY == AllowedIESProfileDim || Texture->PlatformData->Mips[0].SizeY == 1)
		)
	{
		return true;
	}
	else
	{
		return false;
	}
}

#endif
