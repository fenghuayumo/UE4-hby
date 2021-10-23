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

#include "NRDDenoiserHistory.h"
#include "NRDPrivate.h"

#include "PostProcess/SceneRenderTargets.h"

#define LOCTEXT_NAMESPACE "FNRDModule"


FNRDRelaxHistory::FNRDRelaxHistory(FIntPoint InHistorySize)
	: HistorySize(InHistorySize)
{
	UE_LOG(LogNRD, Log, TEXT("Creating FNRDRelaxHistory   This = %p HistorySize=%s FrameIndex = %u"), this, *HistorySize.ToString(), FrameIndex);
}

FNRDRelaxHistory::~FNRDRelaxHistory()
{
	UE_LOG(LogNRD, Log, TEXT("Destroying FNRDRelaxHistory This = %p HistorySize=%s FrameIndex = %u"), this, *HistorySize.ToString(), FrameIndex);
}

FNRDDenoisePolychromaticPenumbraHarmonicsHistory::FNRDDenoisePolychromaticPenumbraHarmonicsHistory(FNRDRelaxHistoryRef InRelaxHistory):
	RelaxHistory(InRelaxHistory)
{
//	UE_LOG(LogNRD, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));
	check(RelaxHistory)
//	UE_LOG(LogNRD, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

FNRDDenoisePolychromaticPenumbraHarmonicsHistory::~FNRDDenoisePolychromaticPenumbraHarmonicsHistory()
{
//	UE_LOG(LogNRD, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));
//	UE_LOG(LogNRD, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

#undef LOCTEXT_NAMESPACE