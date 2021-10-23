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

#pragma once

#include "CoreMinimal.h"
#include "SceneRendering.h"

struct IPooledRenderTarget;

class NRD_API FNRDRelaxHistory
{
public:
	uint64 FrameIndex = 0;

	FIntPoint HistorySize;

	TRefCountPtr <IPooledRenderTarget> SpecularAndDiffuseIlluminationLogLuv;
	TRefCountPtr <IPooledRenderTarget> SpecularAndDiffuseIlluminationResponsiveLogLuv;
	TRefCountPtr <IPooledRenderTarget> SpecularIlluminationUnpacked;
	TRefCountPtr <IPooledRenderTarget> DiffuseIlluminationUnpacked;
	TRefCountPtr <IPooledRenderTarget> SpecularAndDiffuse2ndMoments;
	TRefCountPtr <IPooledRenderTarget> NormalRoughnessDepth;
	TRefCountPtr <IPooledRenderTarget> ReflectionHitT;
	TRefCountPtr <IPooledRenderTarget> SpecularAndDiffuseHistoryLength;

	bool HasValidResources() const
	{
		return SpecularAndDiffuseIlluminationLogLuv.IsValid()
			&& SpecularAndDiffuseIlluminationResponsiveLogLuv.IsValid()
			&& SpecularIlluminationUnpacked.IsValid()
			&& SpecularIlluminationUnpacked.IsValid()
			&& DiffuseIlluminationUnpacked.IsValid()
			&& SpecularAndDiffuse2ndMoments.IsValid()
			&& NormalRoughnessDepth.IsValid()
			&& ReflectionHitT.IsValid()
			&& SpecularAndDiffuseHistoryLength.IsValid();
	}

	FNRDRelaxHistory(FIntPoint InHistorySize);
	~FNRDRelaxHistory();
};

using FNRDRelaxHistoryRef = TSharedPtr<FNRDRelaxHistory, ESPMode::ThreadSafe>;


class NRD_API FNRDDenoisePolychromaticPenumbraHarmonicsHistory final : public ICustomDenoisePolychromaticPenumbraHarmonicsHistory, public FRefCountBase
{
	public:
	
	FNRDRelaxHistoryRef RelaxHistory;

	virtual uint32 AddRef() const final
	{
		return FRefCountBase::AddRef();
	}

	virtual uint32 Release() const final
	{
		return FRefCountBase::Release();
	}

	virtual uint32 GetRefCount() const final
	{
		return FRefCountBase::GetRefCount();
	}

	FNRDDenoisePolychromaticPenumbraHarmonicsHistory(FNRDRelaxHistoryRef InRelaxHistory);
	~FNRDDenoisePolychromaticPenumbraHarmonicsHistory();
};
