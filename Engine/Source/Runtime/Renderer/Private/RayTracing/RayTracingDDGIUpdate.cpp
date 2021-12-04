#include "RayTracingDDGIUpdate.h"
// UE4 public interfaces
#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "SceneView.h"
#include "RenderGraph.h"
#include "RayGenShaderUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"

// UE4 private interfaces
#include "ReflectionEnvironment.h"
#include "FogRendering.h"
#include "SceneRendering.h"
#include "SceneTextureParameters.h"
#include "RayTracing/RayTracingLighting.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"

#include <cmath>

// local includes
#include "Components/TestGIComponent.h"
#include "RTDDGIVolumeDesGPU.h"

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
static TAutoConsoleVariable<int> CVarRTDDGIProbesTextureVis(
	TEXT("r.RTGI.DDGI.ProbesTextureVis"),
	0,
	TEXT("If 1, will render what the probes see. If 2, will show misses (blue), hits (green), backfaces (red). \'vis RTDDGIProbesTexure\' to see the output.\n"),
	ECVF_RenderThreadSafe);
#endif

TMap<const FSceneInterface*, float> FTestGIVolumeSceneProxy::SceneRoundRobinValue;

#if RHI_RAYTRACING

static FMatrix ComputeRandomRotation()
{
	// This approach is based on James Arvo's implementation from Graphics Gems 3 (pg 117-120).
	// Also available at: http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.53.1357&rep=rep1&type=pdf

	// Setup a random rotation matrix using 3 uniform RVs
	float u1 = 2.f * 3.14159265359 * FMath::FRand();
	float cos1 = std::cosf(u1);
	float sin1 = std::sinf(u1);

	float u2 = 2.f * 3.14159265359 * FMath::FRand();
	float cos2 = std::cosf(u2);
	float sin2 = std::sinf(u2);

	float u3 = FMath::FRand();
	float sq3 = 2.f * std::sqrtf(u3 * (1.f - u3));

	float s2 = 2.f * u3 * sin2 * sin2 - 1.f;
	float c2 = 2.f * u3 * cos2 * cos2 - 1.f;
	float sc = 2.f * u3 * sin2 * cos2;

	// Create the random rotation matrix
	float _11 = cos1 * c2 - sin1 * sc;
	float _12 = sin1 * c2 + cos1 * sc;
	float _13 = sq3 * cos2;

	float _21 = cos1 * sc - sin1 * s2;
	float _22 = sin1 * sc + cos1 * s2;
	float _23 = sq3 * sin2;

	float _31 = cos1 * (sq3 * cos2) - sin1 * (sq3 * sin2);
	float _32 = sin1 * (sq3 * cos2) + cos1 * (sq3 * sin2);
	float _33 = 1.f - 2.f * u3;

	return FMatrix(
		FPlane(_11, _12, _13, 0.f),
		FPlane(_21, _22, _23, 0.f),
		FPlane(_31, _32, _33, 0.f),
		FPlane(0.f, 0.f, 0.f, 1.f)
	);
	//return FMatrix(
	//	FPlane(1, 0, 0, 0.f),
	//	FPlane(0, 1, 0, 0.f),
	//	FPlane(0, 0, 1, 0.f),
	//	FPlane(0.f, 0.f, 0.f, 1.f)
	//);
}

class FRayTracingGIProbeUpdateRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingGIProbeUpdateRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingGIProbeUpdateRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY"); // If false, it will cull back face triangles. We want this on for probe relocation and to stop light leak.
	class FEnableMaterialsDim : SHADER_PERMUTATION_BOOL("ENABLE_MATERIALS");                 // If false, forces the geo to opaque (no alpha test). We want this off for speed.
	class FEnableRelocation : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_PROBE_RELOCATION");
	class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");
	class FSkyLight : SHADER_PERMUTATION_INT("RTXGI_DDGI_SKY_LIGHT_TYPE", 3);

	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableMaterialsDim, FEnableRelocation, FFormatRadiance, FFormatIrradiance, FEnableScrolling, FSkyLight>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);

		// Set to 1 to be able to visualize this in the editor by typing "vis RTDDGIVolumeUpdateDebug" and later "vis none" to make it go away.
		// Set to 0 to disable and deadstrip everything related
		OutEnvironment.SetDefine(TEXT("DDGIVolumeUpdateDebug"), 0);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)

		SHADER_PARAMETER(uint32, FrameRandomSeed)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DDGIVolume_ProbeIrradiance)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DDGIVolume_ProbeDistance)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DDGIVolume_ProbeOffsets)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, DDGIVolume_ProbeStates)
		SHADER_PARAMETER_SAMPLER(SamplerState, DDGIVolume_LinearClampSampler)
		SHADER_PARAMETER(FVector, DDGIVolume_Radius)
		SHADER_PARAMETER(float, DDGIVolume_IrradianceScalar)
		SHADER_PARAMETER(float, DDGIVolume_EmissiveMultiplier)
		SHADER_PARAMETER(int, DDGIVolume_ProbeIndexStart)
		SHADER_PARAMETER(int, DDGIVolume_ProbeIndexCount)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FRTDDGIVolumeDescGPU, RTDDGIVolume)

		SHADER_PARAMETER(FVector, Sky_Color)
		SHADER_PARAMETER_TEXTURE(Texture2D, Sky_Texture)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sky_TextureSampler)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RadianceOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DebugOutput)  // Per unreal RDG presentation, this is deadstripped if the shader doesn't write to it

		// assorted things needed by material resolves, even though some don't make sense outside of screenspace
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FRaytracingLightDataPacked, LightDataPacked)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingGIProbeUpdateRGS, "/Engine/Private/DDGI/ProbeUpdateRGS.usf", "ProbeUpdateRGS", SF_RayGen);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

class FRayTracingGIProbeViewRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingGIProbeViewRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingGIProbeViewRGS, FGlobalShader)

	class FEnableTwoSidedGeometryDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY"); // If false, it will cull back face triangles. We want this on for probe relocation and to stop light leak.
	class FEnableMaterialsDim : SHADER_PERMUTATION_BOOL("ENABLE_MATERIALS");                 // If false, forces the geo to opaque (no alpha test). We want this off for speed.
	class FVolumeDebugView : SHADER_PERMUTATION_INT("VOLUME_DEBUG_VIEW", 2);

	using FPermutationDomain = TShaderPermutationDomain<FEnableTwoSidedGeometryDim, FEnableMaterialsDim, FVolumeDebugView>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);
		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_RELOCATION"), 0);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)

		SHADER_PARAMETER(uint32, FrameRandomSeed)

		SHADER_PARAMETER(FVector, CameraPos)
		SHADER_PARAMETER(FMatrix, CameraMatrix)

		SHADER_PARAMETER(float, DDGIVolume_PreExposure)
		SHADER_PARAMETER(int32, DDGIVolume_ShouldUsePreExposure)

		SHADER_PARAMETER(FVector, Sky_Color)
		SHADER_PARAMETER_TEXTURE(Texture2D, Sky_Texture)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sky_TextureSampler)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RadianceOutput)

		// assorted things needed by material resolves, even though some don't make sense outside of screenspace
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FRaytracingLightDataPacked, LightDataPacked)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingGIProbeViewRGS, "/Engine/Private/DDGI/ProbeViewRGS.usf", "ProbeViewRGS", SF_RayGen);

#endif // #if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

class FRTDDGIIrradianceBlend : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIIrradianceBlend)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIIrradianceBlend, FGlobalShader)

		class FRaysPerProbeEnum : SHADER_PERMUTATION_SPARSE_INT("RAYS_PER_PROBE",
			int32(ETestGIRaysPerProbe::n144),
			int32(ETestGIRaysPerProbe::n288),
			int32(ETestGIRaysPerProbe::n432),
			int32(ETestGIRaysPerProbe::n576),
			int32(ETestGIRaysPerProbe::n720),
			int32(ETestGIRaysPerProbe::n864),
			int32(ETestGIRaysPerProbe::n1008));
	class FEnableRelocation : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_PROBE_RELOCATION");
	class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");

	using FPermutationDomain = TShaderPermutationDomain<FRaysPerProbeEnum, FEnableRelocation, FFormatRadiance, FFormatIrradiance, FEnableScrolling>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);

		OutEnvironment.SetDefine(TEXT("PROBE_NUM_TEXELS"), FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance);
		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_BLEND_RADIANCE"), 1);

		// Set to 1 to be able to visualize this in the editor by typing "vis RTDDGIIrradianceBlendDebug" and later "vis none" to make it go away.
		// Set to 0 to disable and deadstrip everything related
		OutEnvironment.SetDefine(TEXT("DDGIIrradianceBlendDebug"), 0);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int, ProbeIndexStart)
		SHADER_PARAMETER(int, ProbeIndexCount)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FRTDDGIVolumeDescGPU, RTDDGIVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeRayDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeProbeDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, DDGIVolumeProbeStatesTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DDGIProbeScrollSpace)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DebugOutput)  // Per unreal RDG presentation, this is deadstripped if the shader doesn't write to it

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIIrradianceBlend, "/Engine/Private/DDGI/ProbeBlendingCS.usf", "DDGIProbeBlendingCS", SF_Compute);

class FRTDDGIDistanceBlend : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIDistanceBlend)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIDistanceBlend, FGlobalShader)

	class FRaysPerProbeEnum : SHADER_PERMUTATION_SPARSE_INT("RAYS_PER_PROBE",
			int32(ETestGIRaysPerProbe::n144),
			int32(ETestGIRaysPerProbe::n288),
			int32(ETestGIRaysPerProbe::n432),
			int32(ETestGIRaysPerProbe::n576),
			int32(ETestGIRaysPerProbe::n720),
			int32(ETestGIRaysPerProbe::n864),
			int32(ETestGIRaysPerProbe::n1008));
	class FEnableRelocation : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_PROBE_RELOCATION");
	class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");

	using FPermutationDomain = TShaderPermutationDomain<FRaysPerProbeEnum, FEnableRelocation, FFormatRadiance, FFormatIrradiance, FEnableScrolling>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);
		OutEnvironment.SetDefine(TEXT("PROBE_NUM_TEXELS"), FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance);
		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_BLEND_RADIANCE"), 0);

		// Set to 1 to be able to visualize this in the editor by typing "vis RTDDGIDistanceBlendDebug" and later "vis none" to make it go away.
		// Set to 0 to disable and deadstrip everything related
		OutEnvironment.SetDefine(TEXT("DDGIDistanceBlendDebug"), 0);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(int, ProbeIndexStart)
		SHADER_PARAMETER(int, ProbeIndexCount)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FRTDDGIVolumeDescGPU, RTDDGIVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeRayDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeProbeDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, DDGIVolumeProbeStatesTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DDGIProbeScrollSpace)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DebugOutput)  // Per unreal RDG presentation, this is deadstripped if the shader doesn't write to it

		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIDistanceBlend, "/Engine/Private/DDGI/ProbeBlendingCS.usf", "DDGIProbeBlendingCS", SF_Compute);

class FRTDDGIBorderRowUpdate : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIBorderRowUpdate)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIBorderRowUpdate, FGlobalShader)

		class FProbeNumTexels : SHADER_PERMUTATION_SPARSE_INT("PROBE_NUM_TEXELS",
			FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance,
			FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance);

	using FPermutationDomain = TShaderPermutationDomain<FProbeNumTexels>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeProbeDataUAV)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIBorderRowUpdate, "/Engine/Private/DDGI/ProbeBorderUpdateCS.usf", "DDGIProbeBorderRowUpdateCS", SF_Compute);

class FRTDDGIBorderColumnUpdate : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIBorderColumnUpdate)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIBorderColumnUpdate, FGlobalShader)

		class FProbeNumTexels : SHADER_PERMUTATION_SPARSE_INT("PROBE_NUM_TEXELS",
			FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance,
			FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance);

	using FPermutationDomain = TShaderPermutationDomain<FProbeNumTexels>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeProbeDataUAV)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIBorderColumnUpdate, "/Engine/Private/DDGI/ProbeBorderUpdateCS.usf", "DDGIProbeBorderColumnUpdateCS", SF_Compute);
//
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FRTDDGIVolumeDescGPU, "RTDDGIVolume");

class FRTDDGIProbesRelocate : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIProbesRelocate)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIProbesRelocate, FGlobalShader)

		class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");

	using FPermutationDomain = TShaderPermutationDomain<FFormatRadiance, FFormatIrradiance, FEnableScrolling>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION ? 1 : 0);
		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_RELOCATION"), 1);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, ProbeDistanceScale)
		SHADER_PARAMETER(int, ProbeIndexStart)
		SHADER_PARAMETER(int, ProbeIndexCount)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FRTDDGIVolumeDescGPU, RTDDGIVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeRayDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeProbeOffsetsUAV)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIProbesRelocate, "/Engine/Private/DDGI/ProbeRelocationCS.usf", "DDGIProbeRelocationCS", SF_Compute);

class FRTDDGIProbesClassify : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRTDDGIProbesClassify)
	SHADER_USE_PARAMETER_STRUCT(FRTDDGIProbesClassify, FGlobalShader)

		class FEnableRelocation : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_PROBE_RELOCATION");
	class FFormatRadiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_RADIANCE");
	class FFormatIrradiance : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_FORMAT_IRRADIANCE");
	class FEnableScrolling : SHADER_PERMUTATION_BOOL("RTXGI_DDGI_INFINITE_SCROLLING_VOLUME");

	using FPermutationDomain = TShaderPermutationDomain<FEnableRelocation, FFormatRadiance, FFormatIrradiance, FEnableScrolling>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("RTXGI_DDGI_PROBE_CLASSIFICATION"), 1);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, ProbeIndexStart)
		SHADER_PARAMETER(int, ProbeIndexCount)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FRTDDGIVolumeDescGPU, RTDDGIVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DDGIVolumeRayDataUAV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DDGIVolumeProbeStatesUAV)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRTDDGIProbesClassify, "/Engine/Private/DDGI/ProbeStateClassifierCS.usf", "DDGIProbeStateClassifierCS", SF_Compute);

#endif // RHI_RAYTRACING

namespace RayTracingDDIGIUpdate
{

#if RHI_RAYTRACING

	void DDGIUpdateVolume_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	void DDGIUpdateVolume_RenderThread_DDGIProbesTextureVis(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder);
#endif 

	void DDGIUpdateVolume_RenderThread_RTRadiance(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureRef ProbesRadianceTex, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount);
	void DDGIUpdateVolume_RenderThread_IrradianceBlend(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount);
	void DDGIUpdateVolume_RenderThread_DistanceBlend(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount);
	void DDGIUpdateVolume_RenderThread_IrradianceBorderUpdate(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy);
	void DDGIUpdateVolume_RenderThread_DistanceBorderUpdate(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy);
	void DDGIUpdateVolume_RenderThread_RelocateProbes(FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount);
	void DDGIUpdateVolume_RenderThread_ClassifyProbes(FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount);

	//void PrepareRayTracingShaders(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders);
#endif // RHI_RAYTRACING


#if RHI_RAYTRACING

	void PrepareRayTracingShaders(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
	{
		const auto FeatureLevel = GMaxRHIFeatureLevel;
		auto ShaderMap = GetGlobalShaderMap(FeatureLevel);

		for (int i = 0; i < 8; ++i)
		{
			for (int j = 0; j < 3; ++j)
			{
				FRayTracingGIProbeUpdateRGS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableTwoSidedGeometryDim>(true);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableMaterialsDim>(false);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableRelocation>((i & 1) != 0 ? true : false);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FFormatRadiance>((i & 2) != 0 ? true : false);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FFormatIrradiance>((i & 2) != 0 ? true : false);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableScrolling>((i & 4) != 0 ? true : false);
				PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FSkyLight>(j);
				TShaderMapRef<FRayTracingGIProbeUpdateRGS> RayGenerationShader(ShaderMap, PermutationVector);

				OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
			}
		}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		for (int i = 0; i < 2; ++i)
		{
			FRayTracingGIProbeViewRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRayTracingGIProbeViewRGS::FEnableTwoSidedGeometryDim>(true);
			PermutationVector.Set<FRayTracingGIProbeViewRGS::FEnableMaterialsDim>(false);
			PermutationVector.Set<FRayTracingGIProbeViewRGS::FVolumeDebugView>((i & 1) != 0 ? true : false);
			TShaderMapRef<FRayTracingGIProbeViewRGS> RayGenerationShader(ShaderMap, PermutationVector);

			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	}

	bool ShouldRenderRayTracingEffect(bool bEffectEnabled)
	{
		if (!IsRayTracingEnabled())
		{
			return false;
		}

		static auto CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.ForceAllRayTracingEffects"));
		const int32 OverrideMode = CVar != nullptr ? CVar->GetInt() : -1;

		if (OverrideMode >= 0)
		{
			return OverrideMode > 0;
		}
		else
		{
			return bEffectEnabled;
		}
	}

	bool ShouldDynamicUpdate(const FViewInfo& View)
	{
		return ShouldRenderRayTracingEffect(true) && View.RayTracingScene.RayTracingSceneRHI != nullptr;
	}

	void DDGIUpdateVolume_RenderThread_RTRadiance(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureRef ProbesRadianceTex, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount)
	{
		// Deal with probe ray budgets, and updating probes in a round robin fashion within the volume
		int ProbeUpdateRayBudget = 0;
		if (ProbeUpdateRayBudget == 0)
		{
			VolProxy->ProbeIndexStart = 0;
			VolProxy->ProbeIndexCount = VolProxy->ComponentData.GetProbeCount();
		}
		else
		{
			int ProbeCount = VolProxy->ComponentData.GetProbeCount();
			int ProbeUpdateBudget = ProbeUpdateRayBudget / VolProxy->ComponentData.GetNumRaysPerProbe();
			if (ProbeUpdateBudget < 1)
				ProbeUpdateBudget = 1;
			if (ProbeUpdateBudget > ProbeCount)
				ProbeUpdateBudget = ProbeCount;
			VolProxy->ProbeIndexStart += ProbeUpdateBudget;
			VolProxy->ProbeIndexStart = VolProxy->ProbeIndexStart % ProbeCount;
			VolProxy->ProbeIndexCount = ProbeUpdateBudget;
		}

		const auto FeatureLevel = GMaxRHIFeatureLevel;
		auto ShaderMap = GetGlobalShaderMap(FeatureLevel);

		FRayTracingGIProbeUpdateRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableTwoSidedGeometryDim>(true);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableMaterialsDim>(false);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableRelocation>(VolProxy->ComponentData.EnableProbeRelocation);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FFormatRadiance>(highBitCount);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FFormatIrradiance>(highBitCount);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FEnableScrolling>(VolProxy->ComponentData.EnableProbeScrolling);
		PermutationVector.Set<FRayTracingGIProbeUpdateRGS::FSkyLight>(int(VolProxy->ComponentData.SkyLightTypeOnRayMiss));
		TShaderMapRef<FRayTracingGIProbeUpdateRGS> RayGenerationShader(ShaderMap, PermutationVector);

		FRayTracingGIProbeUpdateRGS::FParameters DefaultPassParameters;
		FRayTracingGIProbeUpdateRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingGIProbeUpdateRGS::FParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
		check(PassParameters->TLAS);
		PassParameters->RadianceOutput = ProbesRadianceUAV;
		PassParameters->FrameRandomSeed = GFrameNumber;

		// skylight parameters
		if (Scene.SkyLight && Scene.SkyLight->ProcessedTexture)
		{
			PassParameters->Sky_Color = FVector(Scene.SkyLight->GetEffectiveLightColor());
			PassParameters->Sky_Texture = Scene.SkyLight->ProcessedTexture->TextureRHI;
			PassParameters->Sky_TextureSampler = Scene.SkyLight->ProcessedTexture->SamplerStateRHI;
		}
		else
		{
			PassParameters->Sky_Color = FVector(0.0);
			PassParameters->Sky_Texture = GBlackTextureCube->TextureRHI;
			PassParameters->Sky_TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}

		// DDGI Volume Parameters
		{
			PassParameters->DDGIVolume_ProbeIrradiance = GraphBuilder.RegisterExternalTexture(VolProxy->ProbesIrradiance);
			PassParameters->DDGIVolume_ProbeDistance = GraphBuilder.RegisterExternalTexture(VolProxy->ProbesDistance);
			PassParameters->DDGIVolume_ProbeOffsets = RegisterExternalTextureWithFallback(GraphBuilder, VolProxy->ProbesOffsets, GSystemTextures.BlackDummy);
			PassParameters->DDGIVolume_ProbeStates = RegisterExternalTextureWithFallback(GraphBuilder, VolProxy->ProbesStates, GSystemTextures.BlackDummy);
			PassParameters->DDGIVolume_LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

			PassParameters->DDGIVolume_Radius = VolProxy->ComponentData.Transform.GetScale3D() * 100.0f;
			PassParameters->DDGIVolume_IrradianceScalar = VolProxy->ComponentData.IrradianceScalar;
			PassParameters->DDGIVolume_EmissiveMultiplier = VolProxy->ComponentData.EmissiveMultiplier;
			PassParameters->DDGIVolume_ProbeIndexStart = VolProxy->ProbeIndexStart;
			PassParameters->DDGIVolume_ProbeIndexCount = VolProxy->ProbeIndexCount;

			// calculate grid spacing based on size (scale) and probe count
			// regarding the *200: the scale is the radius so we need to double it. There is also an implict * 100 of the basic box.
			FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
			FVector probeGridSpacing;
			probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
			probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
			probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

			FRTDDGIVolumeDescGPU DefaultDDGIVolumeDescGPU;
			FRTDDGIVolumeDescGPU* DDGIVolumeDescGPU = GraphBuilder.AllocParameters<FRTDDGIVolumeDescGPU>();
			*DDGIVolumeDescGPU = DefaultDDGIVolumeDescGPU;
			DDGIVolumeDescGPU->origin = VolProxy->ComponentData.Origin;
			FQuat rotation = VolProxy->ComponentData.Transform.GetRotation();
			DDGIVolumeDescGPU->rotation = FVector4{ rotation.X, rotation.Y, rotation.Z, rotation.W };
			DDGIVolumeDescGPU->probeMaxRayDistance = VolProxy->ComponentData.ProbeMaxRayDistance;
			DDGIVolumeDescGPU->probeGridCounts = VolProxy->ComponentData.ProbeCounts;
			DDGIVolumeDescGPU->probeRayRotationTransform = ProbeRayRotationTransform;
			DDGIVolumeDescGPU->numRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
			DDGIVolumeDescGPU->probeGridSpacing = probeGridSpacing;
			DDGIVolumeDescGPU->probeNumIrradianceTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance;
			DDGIVolumeDescGPU->probeNumDistanceTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance;
			DDGIVolumeDescGPU->probeIrradianceEncodingGamma = VolProxy->ComponentData.ProbeIrradianceEncodingGamma;
			DDGIVolumeDescGPU->normalBias = VolProxy->ComponentData.NormalBias;
			DDGIVolumeDescGPU->viewBias = VolProxy->ComponentData.ViewBias;
			DDGIVolumeDescGPU->probeScrollOffsets = VolProxy->ComponentData.ProbeScrollOffsets;

			PassParameters->RTDDGIVolume = GraphBuilder.CreateUniformBuffer(DDGIVolumeDescGPU);
		}

		FRDGTextureDesc DDGIDebugOutputDesc = FRDGTextureDesc::Create2D(
			ProbesRadianceTex->Desc.Extent,
			ProbesRadianceTex->Desc.Format,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV
		);
		PassParameters->DebugOutput = GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(DDGIDebugOutputDesc, TEXT("DDGIVolumeUpdateDebug")));

		PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		PassParameters->LightDataPacked = View.RayTracingLightData.UniformBuffer;

		FIntPoint DispatchSize = ProbesRadianceTex->Desc.Extent;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DDGI RTRadiance %dx%d", DispatchSize.X, DispatchSize.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchSize, ProbesRadianceTex](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchSize.X, DispatchSize.Y);
			}
		);
	}


	void DDGIUpdateVolume_RenderThread_IrradianceBlend(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
		FRTDDGIIrradianceBlend::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRTDDGIIrradianceBlend::FRaysPerProbeEnum>(int(VolProxy->ComponentData.RaysPerProbe));
		PermutationVector.Set<FRTDDGIIrradianceBlend::FEnableRelocation>(VolProxy->ComponentData.EnableProbeRelocation);
		PermutationVector.Set<FRTDDGIIrradianceBlend::FFormatRadiance>(highBitCount);
		PermutationVector.Set<FRTDDGIIrradianceBlend::FFormatIrradiance>(highBitCount);
		PermutationVector.Set<FRTDDGIIrradianceBlend::FEnableScrolling>(VolProxy->ComponentData.EnableProbeScrolling);
		TShaderMapRef<FRTDDGIIrradianceBlend> ComputeShader(ShaderMap, PermutationVector);

		// calculate grid spacing based on size (scale) and probe count
		// regarding the *200: the scale is the radius so we need to double it. There is also an implict * 100 of the basic box.
		FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

		// set up the shader parameters
		FRTDDGIVolumeDescGPU DefaultDDGIVolumeDescGPU;
		FRTDDGIVolumeDescGPU* DDGIVolumeDescGPU = GraphBuilder.AllocParameters<FRTDDGIVolumeDescGPU>();
		*DDGIVolumeDescGPU = DefaultDDGIVolumeDescGPU;
		DDGIVolumeDescGPU->probeGridSpacing = probeGridSpacing;
		DDGIVolumeDescGPU->probeGridCounts = VolProxy->ComponentData.ProbeCounts;
		DDGIVolumeDescGPU->numRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
		DDGIVolumeDescGPU->probeRayRotationTransform = ProbeRayRotationTransform;
		DDGIVolumeDescGPU->probeDistanceExponent = VolProxy->ComponentData.ProbeDistanceExponent;
		DDGIVolumeDescGPU->probeInverseIrradianceEncodingGamma = 1.0f / VolProxy->ComponentData.ProbeIrradianceEncodingGamma;
		DDGIVolumeDescGPU->probeHysteresis = VolProxy->ComponentData.ProbeHysteresis;
		DDGIVolumeDescGPU->probeChangeThreshold = VolProxy->ComponentData.ProbeChangeThreshold;
		DDGIVolumeDescGPU->probeBrightnessThreshold = VolProxy->ComponentData.ProbeBrightnessThreshold;
		DDGIVolumeDescGPU->probeScrollOffsets = VolProxy->ComponentData.ProbeScrollOffsets;

		FRTDDGIIrradianceBlend::FParameters DefaultPassParameters;
		FRTDDGIIrradianceBlend::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIIrradianceBlend::FParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->ProbeIndexStart = VolProxy->ProbeIndexStart;
		PassParameters->ProbeIndexCount = VolProxy->ProbeIndexCount;

		PassParameters->RTDDGIVolume = GraphBuilder.CreateUniformBuffer(DDGIVolumeDescGPU);

		PassParameters->DDGIVolumeRayDataUAV = ProbesRadianceUAV;
		PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesIrradiance));
		PassParameters->DDGIVolumeProbeStatesTexture = RegisterExternalTextureWithFallback(GraphBuilder, VolProxy->ProbesStates, GSystemTextures.BlackDummy);

		if (VolProxy->ComponentData.EnableProbeScrolling)
			PassParameters->DDGIProbeScrollSpace = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesSpace));

		FRDGTextureDesc DDGIDebugOutputDesc = FRDGTextureDesc::Create2D(
			VolProxy->ProbesIrradiance->GetTargetableRHI()->GetTexture2D()->GetSizeXY(),
			VolProxy->ProbesIrradiance->GetTargetableRHI()->GetFormat(),
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV
		);
		PassParameters->DebugOutput = GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(DDGIDebugOutputDesc, TEXT("DDGIIrradianceBlendDebug")));

		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("DDGI Radiance Blend"),
			ComputeShader,
			PassParameters,
			FIntVector(ProbeCount2D.X, ProbeCount2D.Y, 1)
		);
	}

	void DDGIUpdateVolume_RenderThread_DistanceBlend(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
		FRTDDGIDistanceBlend::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRTDDGIDistanceBlend::FRaysPerProbeEnum>(int(VolProxy->ComponentData.RaysPerProbe));
		PermutationVector.Set<FRTDDGIDistanceBlend::FEnableRelocation>(int(VolProxy->ComponentData.EnableProbeRelocation));
		PermutationVector.Set<FRTDDGIDistanceBlend::FFormatRadiance>(highBitCount);
		PermutationVector.Set<FRTDDGIDistanceBlend::FFormatIrradiance>(highBitCount);
		PermutationVector.Set<FRTDDGIDistanceBlend::FEnableScrolling>(int(VolProxy->ComponentData.EnableProbeScrolling));
		TShaderMapRef<FRTDDGIDistanceBlend> ComputeShader(ShaderMap, PermutationVector);

		// calculate grid spacing based on size (scale) and probe count
		// regarding the *200: the scale is the radius so we need to double it. There is also an implict * 100 of the basic box.
		FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

		FRTDDGIVolumeDescGPU DefaultDDGIVolumeDescGPU;
		FRTDDGIVolumeDescGPU* DDGIVolumeDescGPU = GraphBuilder.AllocParameters<FRTDDGIVolumeDescGPU>();
		*DDGIVolumeDescGPU = DefaultDDGIVolumeDescGPU;
		DDGIVolumeDescGPU->probeGridSpacing = probeGridSpacing;
		DDGIVolumeDescGPU->probeGridCounts = VolProxy->ComponentData.ProbeCounts;
		DDGIVolumeDescGPU->numRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
		DDGIVolumeDescGPU->probeRayRotationTransform = ProbeRayRotationTransform;
		DDGIVolumeDescGPU->probeDistanceExponent = VolProxy->ComponentData.ProbeDistanceExponent;
		DDGIVolumeDescGPU->probeInverseIrradianceEncodingGamma = 1.0f / VolProxy->ComponentData.ProbeIrradianceEncodingGamma;
		DDGIVolumeDescGPU->probeHysteresis = VolProxy->ComponentData.ProbeHysteresis;
		DDGIVolumeDescGPU->probeChangeThreshold = VolProxy->ComponentData.ProbeChangeThreshold;
		DDGIVolumeDescGPU->probeBrightnessThreshold = VolProxy->ComponentData.ProbeBrightnessThreshold;
		DDGIVolumeDescGPU->probeScrollOffsets = VolProxy->ComponentData.ProbeScrollOffsets;

		FRTDDGIDistanceBlend::FParameters DefaultPassParameters;
		FRTDDGIDistanceBlend::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIDistanceBlend::FParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->ProbeIndexStart = VolProxy->ProbeIndexStart;
		PassParameters->ProbeIndexCount = VolProxy->ProbeIndexCount;

		PassParameters->RTDDGIVolume = GraphBuilder.CreateUniformBuffer(DDGIVolumeDescGPU);

		PassParameters->DDGIVolumeRayDataUAV = ProbesRadianceUAV;
		PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesDistance));
		PassParameters->DDGIVolumeProbeStatesTexture = RegisterExternalTextureWithFallback(GraphBuilder, VolProxy->ProbesStates, GSystemTextures.BlackDummy);

		if (VolProxy->ComponentData.EnableProbeScrolling)
			PassParameters->DDGIProbeScrollSpace = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesSpace));

		FRDGTextureDesc DDGIDebugOutputDesc = FRDGTextureDesc::Create2D(
			VolProxy->ProbesDistance->GetTargetableRHI()->GetTexture2D()->GetSizeXY(),
			VolProxy->ProbesDistance->GetTargetableRHI()->GetFormat(),
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV
		);
		PassParameters->DebugOutput = GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(DDGIDebugOutputDesc, TEXT("DDGIDistanceBlendDebug")));

		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("DDGI Distance Blend"),
			ComputeShader,
			PassParameters,
			FIntVector(ProbeCount2D.X, ProbeCount2D.Y, 1)
		);
	}

	void DDGIUpdateVolume_RenderThread_IrradianceBorderUpdate(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy)
	{
		float groupSize = 8.0f;
		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();

		// Row
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
			FRTDDGIBorderRowUpdate::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRTDDGIBorderRowUpdate::FProbeNumTexels>(FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance);
			TShaderMapRef<FRTDDGIBorderRowUpdate> ComputeShader(ShaderMap, PermutationVector);

			FRTDDGIBorderRowUpdate::FParameters DefaultPassParameters;
			FRTDDGIBorderRowUpdate::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIBorderRowUpdate::FParameters>();
			*PassParameters = DefaultPassParameters;

			PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesIrradiance));

			uint32 numThreadsX = (ProbeCount2D.X * (FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance + 2));
			uint32 numThreadsY = ProbeCount2D.Y;
			uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSize);
			uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("DDGI Irradiance Border Update Row"),
				ComputeShader,
				PassParameters,
				FIntVector(numGroupsX, numGroupsY, 1)
			);
		}

		// Column
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
			FRTDDGIBorderColumnUpdate::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRTDDGIBorderColumnUpdate::FProbeNumTexels>(FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance);
			TShaderMapRef<FRTDDGIBorderColumnUpdate> ComputeShader(ShaderMap, PermutationVector);

			FRTDDGIBorderColumnUpdate::FParameters DefaultPassParameters;
			FRTDDGIBorderColumnUpdate::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIBorderColumnUpdate::FParameters>();
			*PassParameters = DefaultPassParameters;

			PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesIrradiance));

			uint32 numThreadsX = (ProbeCount2D.X * 2);
			uint32 numThreadsY = (ProbeCount2D.Y * (FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance + 2));
			uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSize);
			uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("DDGI Irradiance Border Update Column"),
				ComputeShader,
				PassParameters,
				FIntVector(numGroupsX, numGroupsY, 1)
			);
		}
	}

	void DDGIUpdateVolume_RenderThread_DistanceBorderUpdate(const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy)
	{
		float groupSize = 8.0f;
		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();

		// Row
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
			FRTDDGIBorderRowUpdate::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRTDDGIBorderRowUpdate::FProbeNumTexels>(FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance);
			TShaderMapRef<FRTDDGIBorderRowUpdate> ComputeShader(ShaderMap, PermutationVector);

			FRTDDGIBorderRowUpdate::FParameters DefaultPassParameters;
			FRTDDGIBorderRowUpdate::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIBorderRowUpdate::FParameters>();
			*PassParameters = DefaultPassParameters;

			PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesDistance));

			uint32 numThreadsX = (ProbeCount2D.X * (FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance + 2));
			uint32 numThreadsY = ProbeCount2D.Y;
			uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSize);
			uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("DDGI Distance Border Update Row"),
				ComputeShader,
				PassParameters,
				FIntVector(numGroupsX, numGroupsY, 1)
			);
		}

		// Column
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
			FRTDDGIBorderColumnUpdate::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRTDDGIBorderColumnUpdate::FProbeNumTexels>(FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance);
			TShaderMapRef<FRTDDGIBorderColumnUpdate> ComputeShader(ShaderMap, PermutationVector);

			FRTDDGIBorderColumnUpdate::FParameters DefaultPassParameters;
			FRTDDGIBorderColumnUpdate::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIBorderColumnUpdate::FParameters>();
			*PassParameters = DefaultPassParameters;

			PassParameters->DDGIVolumeProbeDataUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesDistance));

			uint32 numThreadsX = (ProbeCount2D.X * 2);
			uint32 numThreadsY = (ProbeCount2D.Y * (FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance + 2));
			uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSize);
			uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("DDGI Distance Border Update Column"),
				ComputeShader,
				PassParameters,
				FIntVector(numGroupsX, numGroupsY, 1)
			);
		}
	}

	void DDGIUpdateVolume_RenderThread_RelocateProbes(FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, const FMatrix& ProbeRayRotationTransform, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount)
	{
		FRTDDGIProbesRelocate::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRTDDGIProbesRelocate::FFormatRadiance>(highBitCount);
		PermutationVector.Set<FRTDDGIProbesRelocate::FFormatIrradiance>(highBitCount);
		PermutationVector.Set<FRTDDGIProbesRelocate::FEnableScrolling>(VolProxy->ComponentData.EnableProbeScrolling);
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
		TShaderMapRef<FRTDDGIProbesRelocate> ComputeShader(ShaderMap, PermutationVector);

		// calculate grid spacing based on size (scale) and probe count
		// regarding the *200: the scale is the radius so we need to double it. There is also an implict * 100 of the basic box.
		FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

		FRTDDGIVolumeDescGPU DefaultDDGIVolumeDescGPU;
		FRTDDGIVolumeDescGPU* DDGIVolumeDescGPU = GraphBuilder.AllocParameters<FRTDDGIVolumeDescGPU>();
		*DDGIVolumeDescGPU = DefaultDDGIVolumeDescGPU;
		DDGIVolumeDescGPU->probeGridSpacing = probeGridSpacing;
		DDGIVolumeDescGPU->probeGridCounts = VolProxy->ComponentData.ProbeCounts;
		DDGIVolumeDescGPU->numRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
		DDGIVolumeDescGPU->probeScrollOffsets = VolProxy->ComponentData.ProbeScrollOffsets;
		DDGIVolumeDescGPU->probeBackfaceThreshold = VolProxy->ComponentData.ProbeBackfaceThreshold;
		DDGIVolumeDescGPU->probeRayRotationTransform = ProbeRayRotationTransform;
		DDGIVolumeDescGPU->probeMinFrontfaceDistance = VolProxy->ComponentData.ProbeMinFrontfaceDistance;

		FRTDDGIProbesRelocate::FParameters DefaultPassParameters;
		FRTDDGIProbesRelocate::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIProbesRelocate::FParameters>();
		*PassParameters = DefaultPassParameters;

		// run every frame with full distance scale value for continuous relocation
		PassParameters->ProbeDistanceScale = 1.0f;

		PassParameters->ProbeIndexStart = VolProxy->ProbeIndexStart;
		PassParameters->ProbeIndexCount = VolProxy->ProbeIndexCount;

		PassParameters->RTDDGIVolume = GraphBuilder.CreateUniformBuffer(DDGIVolumeDescGPU);

		PassParameters->DDGIVolumeRayDataUAV = ProbesRadianceUAV;
		// This resource is required if this method was called.
		check(VolProxy->ProbesOffsets);
		PassParameters->DDGIVolumeProbeOffsetsUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesOffsets));

		float groupSizeX = 8.f;
		float groupSizeY = 4.f;

		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();
		uint32 numThreadsX = ProbeCount2D.X;
		uint32 numThreadsY = ProbeCount2D.Y;
		uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSizeX);
		uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSizeY);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("DDGI Probe Relocation"),
			ComputeShader,
			PassParameters,
			FIntVector(numGroupsX, numGroupsY, 1)
		);
	}

	void DDGIUpdateVolume_RenderThread_ClassifyProbes(FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy, FRDGTextureUAVRef ProbesRadianceUAV, bool highBitCount)
	{
		// get the permuted shader
		FRTDDGIProbesClassify::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRTDDGIProbesClassify::FEnableRelocation>(VolProxy->ComponentData.EnableProbeRelocation);
		PermutationVector.Set <FRTDDGIProbesClassify::FFormatRadiance>(highBitCount);
		PermutationVector.Set <FRTDDGIProbesClassify::FFormatIrradiance>(highBitCount);
		PermutationVector.Set <FRTDDGIProbesClassify::FEnableScrolling>(VolProxy->ComponentData.EnableProbeScrolling);
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
		TShaderMapRef<FRTDDGIProbesClassify> ComputeShader(ShaderMap, PermutationVector);

		// calculate grid spacing based on size (scale) and probe count
		// regarding the *200: the scale is the radius so we need to double it. There is also an implict * 100 of the basic box.
		FVector volumeSize = VolProxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(VolProxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(VolProxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(VolProxy->ComponentData.ProbeCounts.Z);

		// set up the shader parameters
		FRTDDGIVolumeDescGPU DefaultDDGIVolumeDescGPU;
		FRTDDGIVolumeDescGPU* DDGIVolumeDescGPU = GraphBuilder.AllocParameters<FRTDDGIVolumeDescGPU>();
		*DDGIVolumeDescGPU = DefaultDDGIVolumeDescGPU;
		DDGIVolumeDescGPU->probeGridSpacing = probeGridSpacing;
		DDGIVolumeDescGPU->probeGridCounts = VolProxy->ComponentData.ProbeCounts;
		DDGIVolumeDescGPU->numRaysPerProbe = VolProxy->ComponentData.GetNumRaysPerProbe();
		DDGIVolumeDescGPU->probeBackfaceThreshold = VolProxy->ComponentData.ProbeBackfaceThreshold;
		DDGIVolumeDescGPU->probeScrollOffsets = VolProxy->ComponentData.ProbeScrollOffsets;

		FRTDDGIProbesClassify::FParameters DefaultPassParameters;
		FRTDDGIProbesClassify::FParameters* PassParameters = GraphBuilder.AllocParameters<FRTDDGIProbesClassify::FParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->ProbeIndexStart = VolProxy->ProbeIndexStart;
		PassParameters->ProbeIndexCount = VolProxy->ProbeIndexCount;

		PassParameters->RTDDGIVolume = GraphBuilder.CreateUniformBuffer(DDGIVolumeDescGPU);

		PassParameters->DDGIVolumeRayDataUAV = ProbesRadianceUAV;
		// This resource is required if this method was called.
		check(VolProxy->ProbesStates);
		PassParameters->DDGIVolumeProbeStatesUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(VolProxy->ProbesStates));

		// Dispatch the compute shader
		float groupSizeX = 8.f;
		float groupSizeY = 4.f;

		FIntPoint ProbeCount2D = VolProxy->ComponentData.Get2DProbeCount();
		uint32 numThreadsX = ProbeCount2D.X;
		uint32 numThreadsY = ProbeCount2D.Y;
		uint32 numGroupsX = (uint32)ceil((float)numThreadsX / groupSizeX);
		uint32 numGroupsY = (uint32)ceil((float)numThreadsY / groupSizeY);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("DDGI Probe Classification"),
			ComputeShader,
			PassParameters,
			FIntVector(numGroupsX, numGroupsY, 1)
		);
	}


#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	void DDGIUpdateVolume_RenderThread_DDGIProbesTextureVis(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder)
	{
		// Early out if not visualizing probes
		int DDGIProbesTextureVis = FMath::Clamp(CVarRTDDGIProbesTextureVis.GetValueOnRenderThread(), 0, 2);
		if (DDGIProbesTextureVis == 0 || View.RayTracingScene.RayTracingSceneRHI == nullptr) return;

		static const int c_probeVisWidth = 800;
		static const int c_probeVisHeight = 600;

		// create the texture and uav being rendered to
		FRDGTextureDesc ProbeVisTex = FRDGTextureDesc::Create2D(
			FIntPoint(c_probeVisWidth, c_probeVisHeight),
			EPixelFormat::PF_A32B32G32R32F,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV
		);
		FRDGTextureUAVRef ProbeVisUAV = GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(ProbeVisTex, TEXT("RTDDGIProbesTexture")));

		// get the shader
		const auto FeatureLevel = GMaxRHIFeatureLevel;
		auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
		FRayTracingGIProbeViewRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracingGIProbeViewRGS::FEnableTwoSidedGeometryDim>(true);
		PermutationVector.Set<FRayTracingGIProbeViewRGS::FEnableMaterialsDim>(false);
		PermutationVector.Set<FRayTracingGIProbeViewRGS::FVolumeDebugView>(DDGIProbesTextureVis - 1);
		TShaderMapRef<FRayTracingGIProbeViewRGS> RayGenerationShader(ShaderMap, PermutationVector);

		// fill out shader parameters
		FRayTracingGIProbeViewRGS::FParameters DefaultPassParameters;
		FRayTracingGIProbeViewRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingGIProbeViewRGS::FParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->DDGIVolume_PreExposure = View.PreExposure;
		PassParameters->DDGIVolume_ShouldUsePreExposure = View.Family->EngineShowFlags.Tonemapper;

		PassParameters->CameraPos = View.ViewMatrices.GetViewOrigin();
		PassParameters->CameraMatrix = View.ViewMatrices.GetViewMatrix().Inverse();

		PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
		check(PassParameters->TLAS);
		PassParameters->RadianceOutput = ProbeVisUAV;
		PassParameters->FrameRandomSeed = GFrameNumber;

		// skylight parameters
		if (Scene.SkyLight && Scene.SkyLight->ProcessedTexture)
		{
			PassParameters->Sky_Color = FVector(Scene.SkyLight->GetEffectiveLightColor());
			PassParameters->Sky_Texture = Scene.SkyLight->ProcessedTexture->TextureRHI;
			PassParameters->Sky_TextureSampler = Scene.SkyLight->ProcessedTexture->SamplerStateRHI;
		}
		else
		{
			PassParameters->Sky_Color = FVector(0.0);
			PassParameters->Sky_Texture = GBlackTextureCube->TextureRHI;
			PassParameters->Sky_TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}

		PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);
		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		PassParameters->LightDataPacked = View.RayTracingLightData.UniformBuffer;

		FIntPoint DispatchSize(c_probeVisWidth, c_probeVisHeight);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DDGI RTRadiance %dx%d", DispatchSize.X, DispatchSize.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchSize](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchSize.X, DispatchSize.Y);
			}
		);
	}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	void DDGIUpdateVolume_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FTestGIVolumeSceneProxy* VolProxy)
	{
		// Early out if ray tracing is not enabled
		if (!ShouldDynamicUpdate(View)) return;

		bool highBitCount = false;
		// ASSUMES RENDERTHREAD
		check(IsInRenderingThread() || IsInParallelRenderingThread());
		check(VolProxy);

		FMatrix ProbeRayRotationTransform = ComputeRandomRotation();

		// Create the temporary radiance texture & UAV
		FRDGTextureRef ProbesRadianceTex;
		FRDGTextureUAVRef ProbesRadianceUAV;
		{
			const FTestGIVolumeSceneProxy::FComponentData& ComponentData = VolProxy->ComponentData;
			FRDGTextureDesc DDGIDebugOutputDesc = FRDGTextureDesc::Create2D(
				FIntPoint
				{
					(int32)ComponentData.GetNumRaysPerProbe(),
					(int32)ComponentData.ProbeCounts.X * ComponentData.ProbeCounts.Y * ComponentData.ProbeCounts.Z,
				},
				// This texture stores both color and distance
				highBitCount ? FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatRadianceHighBitDepth : FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatRadianceLowBitDepth,
				FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV
				);

			ProbesRadianceTex = GraphBuilder.CreateTexture(DDGIDebugOutputDesc, TEXT("DDGIVolumeRadiance"));
			ProbesRadianceUAV = GraphBuilder.CreateUAV(ProbesRadianceTex);
		}
		VolProxy->ProbesRadianceTex = ProbesRadianceTex;
		VolProxy->ProbesRadianceUAV = ProbesRadianceUAV;
		VolProxy->ComponentData.RayRotationTransform = ProbeRayRotationTransform;

		DDGIUpdateVolume_RenderThread_RTRadiance(Scene, View, GraphBuilder, VolProxy, ProbeRayRotationTransform, ProbesRadianceTex, ProbesRadianceUAV, highBitCount);
		DDGIUpdateVolume_RenderThread_IrradianceBlend(View, GraphBuilder, VolProxy, ProbeRayRotationTransform, ProbesRadianceUAV, highBitCount);
		DDGIUpdateVolume_RenderThread_DistanceBlend(View, GraphBuilder, VolProxy, ProbeRayRotationTransform, ProbesRadianceUAV, highBitCount);
		DDGIUpdateVolume_RenderThread_IrradianceBorderUpdate(View, GraphBuilder, VolProxy);
		DDGIUpdateVolume_RenderThread_DistanceBorderUpdate(View, GraphBuilder, VolProxy);

		if (VolProxy->ComponentData.EnableProbeRelocation)
		{
			DDGIUpdateVolume_RenderThread_RelocateProbes(GraphBuilder, VolProxy, ProbeRayRotationTransform, ProbesRadianceUAV, highBitCount);
		}

		if (FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION)
		{
			DDGIUpdateVolume_RenderThread_ClassifyProbes(GraphBuilder, VolProxy, ProbesRadianceUAV, highBitCount);
		}
	}

	void DDGIUpdatePerFrame_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder)
	{
		check(IsInRenderingThread() || IsInParallelRenderingThread());

		// Gather the list of volumes to update and load data if it's available.
		// Loading static data is the only thing that happens if ray tracing is not available.
		TArray<FTestGIVolumeSceneProxy*> sceneVolumes;
		float totalPriority = 0.0f;
		for (FTestGIVolumeSceneProxy* proxy : Scene.TestGIProxies)
		{
			// Copy the volume's texture data to the GPU, if loading from disk has finished
			if (proxy->TextureLoadContext.ReadyForLoad)
			{
				if (proxy->TextureLoadContext.Irradiance.Texture)
				{
					TRefCountPtr<IPooledRenderTarget> IrradianceLoaded = CreateRenderTarget(proxy->TextureLoadContext.Irradiance.Texture.GetReference(), TEXT("RTDDGIIrradianceLoaded"));
					AddCopyTexturePass(GraphBuilder, GraphBuilder.RegisterExternalTexture(IrradianceLoaded), GraphBuilder.RegisterExternalTexture(proxy->ProbesIrradiance), FRHICopyTextureInfo{});
				}

				if (proxy->TextureLoadContext.Distance.Texture)
				{
					TRefCountPtr<IPooledRenderTarget> DistanceLoaded = CreateRenderTarget(proxy->TextureLoadContext.Distance.Texture.GetReference(), TEXT("RTDDGIDistanceLoaded"));
					AddCopyTexturePass(GraphBuilder, GraphBuilder.RegisterExternalTexture(DistanceLoaded), GraphBuilder.RegisterExternalTexture(proxy->ProbesDistance), FRHICopyTextureInfo{});
				}

				if (proxy->TextureLoadContext.Offsets.Texture && proxy->ProbesOffsets)
				{
					TRefCountPtr<IPooledRenderTarget> OffsetsLoaded = CreateRenderTarget(proxy->TextureLoadContext.Offsets.Texture.GetReference(), TEXT("RTDDGIOffsetsLoaded"));
					AddCopyTexturePass(GraphBuilder, GraphBuilder.RegisterExternalTexture(OffsetsLoaded), GraphBuilder.RegisterExternalTexture(proxy->ProbesOffsets), FRHICopyTextureInfo{});
				}

				if (proxy->TextureLoadContext.States.Texture && proxy->ProbesStates)
				{
					TRefCountPtr<IPooledRenderTarget> StatesLoaded = CreateRenderTarget(proxy->TextureLoadContext.States.Texture.GetReference(), TEXT("RTDDGIStatesLoaded"));
					AddCopyTexturePass(GraphBuilder, GraphBuilder.RegisterExternalTexture(StatesLoaded), GraphBuilder.RegisterExternalTexture(proxy->ProbesStates), FRHICopyTextureInfo{});
				}

				proxy->TextureLoadContext.Clear();
			}

			// Don't update the volume if it isn't part of the current scene
			if (proxy->OwningScene != &Scene) continue;

			// Don't update static runtime volumes during gameplay
			if (View.bIsGameView && proxy->ComponentData.RuntimeStatic) continue;

			// Don't update the volume if it is disabled
			if (!proxy->ComponentData.EnableVolume) continue;

			sceneVolumes.Add(proxy);
			totalPriority += proxy->ComponentData.UpdatePriority;
		}
#if RHI_RAYTRACING

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		DDGIUpdateVolume_RenderThread_DDGIProbesTextureVis(Scene, View, GraphBuilder);
#endif

		// Advance the scene's round robin value by the golden ratio (conjugate) and use that 
		// as a "random number" to give each volume a fair turn at recieving an update.
		float& value = FTestGIVolumeSceneProxy::SceneRoundRobinValue.FindOrAdd(&Scene);
		value += 0.61803398875f;
		value -= floor(value);

		// Update the relevant volumes with ray tracing
		float desiredPriority = totalPriority * value;
		for (int index = 0; index < sceneVolumes.Num(); ++index)
		{
			desiredPriority -= sceneVolumes[index]->ComponentData.UpdatePriority;
			if (desiredPriority <= 0.0f || index == sceneVolumes.Num() - 1)
			{
				DDGIUpdateVolume_RenderThread(Scene, View, GraphBuilder, sceneVolumes[index]);
				break;
			}
		}

#endif // RHI_RAYTRACING
	}
#endif
}
