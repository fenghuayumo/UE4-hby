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
#include "HAL/IConsoleManager.h"
DECLARE_LOG_CATEGORY_EXTERN(LogNRD, Verbose, All);


// Cvar with min and max range
// Cvar description will include the range and default automatically
template <typename Type>
struct NRDCVar
{
	NRDCVar(const TCHAR* InCVarName, Type InDefault, const TCHAR* InCVarDescription, Type InMin, Type InMax)
		: DescriptionWithMinMaxDefault(FString::Format(TEXT("{0} [{1} ... {2}] (Default = {3}) "), { InCVarDescription, InMin, InMax, InDefault }))
		, ConsoleVariable(
			InCVarName,
			InDefault,
			*DescriptionWithMinMaxDefault,
			ECVF_RenderThreadSafe)
		, Min(InMin)
		, Max(InMax)
	{
	}

	operator Type() const
	{
		return FMath::Clamp<Type>(ConsoleVariable.GetValueOnRenderThread(), Min, Max);
	}

	FString DescriptionWithMinMaxDefault;
	TAutoConsoleVariable<Type> ConsoleVariable;

	Type Min;
	Type Max;
};