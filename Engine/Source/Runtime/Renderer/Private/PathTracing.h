// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ShaderParameterMacros.h"

// this struct holds skylight parameters
BEGIN_SHADER_PARAMETER_STRUCT(FPathTracingSkylight, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkylightTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkylightPdf)
	SHADER_PARAMETER_SAMPLER(SamplerState, SkylightTextureSampler)
	SHADER_PARAMETER(float, SkylightInvResolution)
	SHADER_PARAMETER(int32, SkylightMipCount)
END_SHADER_PARAMETER_STRUCT()

// this struct holds a light grid for both building or rendering
BEGIN_SHADER_PARAMETER_STRUCT(FPathTracingLightGrid, RENDERER_API)
SHADER_PARAMETER(uint32, SceneInfiniteLightCount)
SHADER_PARAMETER(FVector, SceneLightsBoundMin)
SHADER_PARAMETER(FVector, SceneLightsBoundMax)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightGrid)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LightGridData)
SHADER_PARAMETER(unsigned, LightGridResolution)
SHADER_PARAMETER(unsigned, LightGridMaxCount)
SHADER_PARAMETER(int, LightGridAxis)
END_SHADER_PARAMETER_STRUCT()

class FRDGBuilder;
class FScene;
class FViewInfo;

bool PrepareSkyTexture(FRDGBuilder& GraphBuilder, FScene* Scene, const FViewInfo& View, bool SkylightEnabled, bool UseMISCompensation, FPathTracingSkylight* SkylightParameters);
