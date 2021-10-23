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

#include "NRDEngineBridge.h"

class FSceneTextureParameters;
class FNRDRelaxHistory;
using FNRDRelaxHistoryRef = TSharedPtr<FNRDRelaxHistory, ESPMode::ThreadSafe>;


// contains engine side textures and such
struct FRelaxPassParameters
{
	FRDGTextureRef Diffuse = nullptr;
	FRDGTextureRef Specular = nullptr;

	FRDGTextureRef NormalAndRoughness = nullptr;
	FRDGTextureRef MotionVectors = nullptr;
	FRDGTextureRef ViewZ = nullptr;
	void Validate() const 
	{
		check(Diffuse);
		check(Specular);
		check(NormalAndRoughness);
		check(MotionVectors);
		check(ViewZ);
	}
};

struct FRelaxOutputs
{
	FRDGTextureRef Diffuse = nullptr;
	FRDGTextureRef Specular = nullptr;
};

FNRDPackInputsArguments RelaxPackInputArguments();

FRelaxOutputs AddRelaxPasses(
	FRDGBuilder& GraphBuilder, 
	const FViewInfo& View, 
	const FRelaxPassParameters& Inputs,
	FNRDRelaxHistoryRef History
);
