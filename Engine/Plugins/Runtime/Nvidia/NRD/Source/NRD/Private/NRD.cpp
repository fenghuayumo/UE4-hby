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
#include "NRD.h"
#include "CoreMinimal.h"
#include "NRDPrivate.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "Logging/LogMacros.h"
#include "ScreenSpaceDenoise.h"
#include "NRDDenoiser.h"


#define LOCTEXT_NAMESPACE "NRDFModule"

DEFINE_LOG_CATEGORY(LogNRD);

void FNRDModule::StartupModule()
{
	UE_LOG(LogNRD, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));
	
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("NRD"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/NRD"), PluginShaderDir);


	const IScreenSpaceDenoiser* DenoiserToWrap = GScreenSpaceDenoiser ? GScreenSpaceDenoiser : IScreenSpaceDenoiser::GetDefaultDenoiser();
	UE_LOG(LogNRD, Log, TEXT("Wrapping %s with FNRDDenoiser "), DenoiserToWrap->GetDebugName());

	NRDDenoiser.Reset(new FNRDDenoiser(DenoiserToWrap));
	GScreenSpaceDenoiser = NRDDenoiser.Get();
	
	UE_LOG(LogNRD, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

void FNRDModule::ShutdownModule()
{
	UE_LOG(LogNRD, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));

	check(IScreenSpaceDenoiser::GetDefaultDenoiser() == NRDDenoiser->GetWrappedDenoiser());
	GScreenSpaceDenoiser = NRDDenoiser->GetWrappedDenoiser();
	NRDDenoiser.Reset();
	
	UE_LOG(LogNRD, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNRDModule, NRD)