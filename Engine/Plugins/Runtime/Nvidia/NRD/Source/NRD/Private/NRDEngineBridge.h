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
#include "RendererInterface.h"
#include "ScreenPass.h"

class FSceneTextureParameters;

struct FNRDResources
{
	FRDGTextureRef ViewDepth;
	FRDGTextureRef MotionVectors;

	FRDGTextureRef NormalAndRoughness;
	FRDGTextureRef DiffuseHit;
	FRDGTextureRef SpecularHit;
};

struct FEngineResources
{
	FEngineResources(const FSceneTextureParameters& InSceneTextures)
		: SceneTextures(InSceneTextures)
	{

	}

	const FSceneTextureParameters& SceneTextures;
	FRDGTextureRef Diffuse;
	FRDGTextureRef DiffuseRayHitDistance;
	FRDGTextureRef Specular;
	FRDGTextureRef SpecularRayHitDistance;
};

struct FNRDPackInputsArguments
{
	bool bPackDiffuseHitDistance = false;
	bool bPackSpecularHitDistance = false;

	bool bCompressDiffuseRadiance = false;
	bool bCompressSpecularRadiance = false;
};

// this extracts & packs data to the format that NRD expects
extern NRD_API FNRDResources NRDPackInputs(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FEngineResources& InEngineResources,
	const FNRDPackInputsArguments& InArguments
);

// this copies the data from the NRD buffers to the viewport location the engine expects
NRD_API FRDGTextureRef NRDUnpackOutput(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef InNRDResource,
	const FRDGTextureDesc& OutputDesc,
	const TCHAR* Name
);
