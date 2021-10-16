// Copyright Epic Games, Inc. All Rights Reserved.

#include "RayTracingLighting.h"
#include "RHI/Public/RHIDefinitions.h"
#include "RendererPrivate.h"

#if RHI_RAYTRACING

#include "SceneRendering.h"

static TAutoConsoleVariable<int32> CVarRayTracingLightingMissShader(
	TEXT("r.RayTracing.LightingMissShader"),
	1,
	TEXT("Whether evaluate lighting using a miss shader when rendering reflections and translucency instead of doing it in ray generation shader. (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingLightingCells(
	TEXT("r.RayTracing.LightCulling.Cells"),
	16,
	TEXT("Number of cells in each dimension for lighting grid (default 16)"),
	ECVF_RenderThreadSafe);
	
static TAutoConsoleVariable<int32> CVarRayTracingLightFunction(
	TEXT("r.RayTracing.LightFunction"),
	1,
	TEXT("Whether to support light material functions in ray tracing effects. (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingLightMask(
	TEXT("r.RayTracing.LightMask"),
	1,
	TEXT("Whether to support light channel mask in ray tracing effects. (default = 1)"),
	ECVF_RenderThreadSafe);

#define STR(S) _STR(S)
#define _STR(S) #S

static TAutoConsoleVariable<int32> CVarRayTracingMaxLights(
	TEXT("r.RayTracing.Lighting.MaxLights"),
	RAY_TRACING_LIGHT_COUNT_MAXIMUM,
	TEXT("How many lights can affect raytraced geometry (i.e. seen in raytraced reflections).\n")
	TEXT(" Capped to an engine-wide max of " STR(RAY_TRACING_LIGHT_COUNT_MAXIMUM)),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracingMaxShadowLights(
	TEXT("r.RayTracing.Lighting.MaxShadowLights"),
	RAY_TRACING_LIGHT_COUNT_MAXIMUM,
	TEXT("How many lights affecting raytraced geometry cast shadows (i.e. seen in raytraced reflections).\n")
	TEXT(" Capped to an engine-wide max of " STR(RAY_TRACING_LIGHT_COUNT_MAXIMUM)),
	ECVF_RenderThreadSafe
);

#undef STR
#undef _STR

static TAutoConsoleVariable<float> CVarRayTracingLightFrustumBoost(
	TEXT("r.RayTracing.Lighting.Priority.FrustumBoost"),
	0.5f,
	TEXT("Prioritization boost given for RT lights touching frustum camera (0..inf)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingLightAheadBoost(
	TEXT("r.RayTracing.Lighting.Priority.AheadBoost"),
	1.0f,
	TEXT("Prioritization boost given for RT light origins in cone ahead of camera (0..inf)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingLightBehindBoost(
	TEXT("r.RayTracing.Lighting.Priority.BehindBoost"),
	1.0f,
	TEXT("Prioritization boost given for RT light origins in cone behind camera (0..inf)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingLightDistPow(
	TEXT("r.RayTracing.Lighting.Priority.DistPow"),
	2.0f,
	TEXT("Exponent of light prioritization distance-weight damping"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingLightLumPow(
	TEXT("r.RayTracing.Lighting.Priority.LumPow"),
	0.5f,
	TEXT("Exponent of light prioritization luminance-weight damping"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRayTracingLightingCellSize(
	TEXT("r.RayTracing.LightCulling.CellSize"),
	200.0f,
	TEXT("Minimum size of light cell (default 200 units)"),
	ECVF_RenderThreadSafe
);

static int32 GRayTracingLightingObeyShadows = 1;
static FAutoConsoleVariableRef  CVarRayTracingLightingObeyShadows(
	TEXT("r.RayTracing.Lighting.ObeyShadows"),
	GRayTracingLightingObeyShadows,
	TEXT("Whether to obey the light shadow setting in ray traced lights (default 1)\n")
	TEXT("  1 - mark lights as shadowing based on settings in the scene\n")
	TEXT("  0 - mark all lights as shadowing (original UE4 behavior)")
);

// flags bits
#define LIGHT_FLAGS_CASTS_SHADOW		0x1

bool CanUseRayTracingLightingMissShader(EShaderPlatform ShaderPlatform)
{
	return CVarRayTracingLightingMissShader.GetValueOnRenderThread() != 0;
}

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FRaytracingLightDataPacked, "RaytracingLightsDataPacked");


class FSetupRayTracingLightCullData : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupRayTracingLightCullData);
	SHADER_USE_PARAMETER_STRUCT(FSetupRayTracingLightCullData, FGlobalShader)

		static int32 GetGroupSize()
	{
		return 32;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, RankedLights)
		SHADER_PARAMETER(FVector, WorldPos)
		SHADER_PARAMETER(uint32, NumLightsToUse)
		SHADER_PARAMETER(uint32, CellCount)
		SHADER_PARAMETER(float, CellScale)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, LightIndices)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, LightCullingVolume)
		END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FSetupRayTracingLightCullData, "/Engine/Private/RayTracing/GenerateCulledLightListCS.usf", "GenerateCulledLightListCS", SF_Compute);
DECLARE_GPU_STAT_NAMED(LightCullingVolumeCompute, TEXT("RT Light Culling Volume Compute"));

//  Goal is to match shadow casting in scene as well as possible
static bool GetShadowCastingProperty(const FLightSceneInfoCompact& LightSceneInfoCompact)
{
	return GRayTracingLightingObeyShadows ? LightSceneInfoCompact.bCastDynamicShadow || LightSceneInfoCompact.bCastStaticShadow : true;
}

struct FRayTracedLightImportance
{
	float ApproxInfluence;
	int32 LightIndex;
	uint32 LightType;
	bool bWantShadow;
};

static int32 RankLights(TArray<FRayTracedLightImportance> &LightRanking,const TSparseArray<FLightSceneInfoCompact>& Lights, const FViewInfo& View)
{
	const float InfluenceMultiplierInFrustum = CVarRayTracingLightFrustumBoost.GetValueOnRenderThread();
	const float InfluenceMultiplierAhead = CVarRayTracingLightAheadBoost.GetValueOnRenderThread();
	const float InfluenceMultiplierBehind = CVarRayTracingLightBehindBoost.GetValueOnRenderThread();
	const float InfluenceLightDistPow = CVarRayTracingLightDistPow.GetValueOnRenderThread();
	const float InfluenceLightLumPow = CVarRayTracingLightLumPow.GetValueOnRenderThread();

	const FVector ViewPos = View.ViewLocation;
	const FVector ViewDirN = View.GetViewDirection().GetSafeNormal();

	LightRanking.Reserve(Lights.Num());

	for (TSparseArray<FLightSceneInfoCompact>::TConstIterator LightIt(Lights); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		checkSlow(Lights.IsValidIndex(LightSceneInfo->Id));
		const bool bHasStaticLighting = LightSceneInfo->Proxy->HasStaticLighting() && LightSceneInfo->IsPrecomputedLightingValid();
		const bool bAffectReflection = LightSceneInfo->Proxy->AffectReflection();

		if (bHasStaticLighting || !bAffectReflection ) continue;
		const bool bIsVisibleInFrustum = LightSceneInfo->ShouldRenderLight(View);

		const FVector LightPos(LightSceneInfo->Proxy->GetPosition());

		const bool isWholeSceneLight = LightSceneInfoCompact.LightType == ELightComponentType::LightType_Directional;

		float ApproxInfluence = 1.f;

		if (!isWholeSceneLight)
		{
			// shrink ranking according to approximation of relative projected area
			const float ViewLightDistance = FVector::Dist(ViewPos, LightPos);
			ApproxInfluence /= (1.f + FMath::Pow(FMath::Loge(1.f + ViewLightDistance), InfluenceLightDistPow));
		}

		// modulate light ranking according to peak perceptual brightness
		const FLinearColor& Color = LightSceneInfoCompact.Color;
		ApproxInfluence *= FMath::Pow(FMath::Loge(1.f + Color.GetLuminance()), InfluenceLightLumPow);

		// boost light ranking according to how centered they are on view
		const FVector ViewToLightN = (LightPos - ViewPos).GetSafeNormal();
		const float ViewDirToLightDot = isWholeSceneLight ? 1.f : FVector::DotProduct(ViewToLightN, ViewDirN);
		if (ViewDirToLightDot > 0.f)
		{
			ApproxInfluence *= 1.0f + InfluenceMultiplierAhead * ViewDirToLightDot;
		}
		else
		{
			ApproxInfluence *= 1.0f + InfluenceMultiplierBehind * (-ViewDirToLightDot);
		}

		// boost ranking for lights intersecting frustum
		if (bIsVisibleInFrustum)
		{
			ApproxInfluence *= 1.0f + InfluenceMultiplierInFrustum;
		}

		checkSlow(ApproxInfluence >= 0.f);

		FRayTracedLightImportance& LightImportance = LightRanking.Emplace_GetRef();
		LightImportance.ApproxInfluence = ApproxInfluence;
		LightImportance.bWantShadow = GetShadowCastingProperty(LightSceneInfoCompact);
		LightImportance.LightIndex = LightSceneInfo->Id;
		LightImportance.LightType = LightSceneInfoCompact.LightType;
	}

	const int32 LightCountLimit = FMath::Min(CVarRayTracingMaxLights.GetValueOnRenderThread(), RAY_TRACING_LIGHT_COUNT_MAXIMUM);

	if (LightCountLimit > 0 && LightRanking.Num() > LightCountLimit)
	{
		// common case is to have too many lights for raytracing to consume, so we sort lights from highest to lowest importance and then just use the front of the resulting list
		struct FCompareFLightImportance
		{
			FORCEINLINE bool operator()(const FRayTracedLightImportance& A, const FRayTracedLightImportance& B) const
			{
				return A.ApproxInfluence > B.ApproxInfluence;
			}
		};
		LightRanking.Sort(FCompareFLightImportance());
	}

	const int32 NumLightsToUse = FMath::Min(LightRanking.Num(), LightCountLimit);

	if (NumLightsToUse > 0)
	{
		const int32 MaxShadowedLights = CVarRayTracingMaxShadowLights.GetValueOnRenderThread();

		if (MaxShadowedLights >= 0 && MaxShadowedLights < NumLightsToUse)
		{
			// turn off shadowing for the overflow
			for (int32 Index = MaxShadowedLights; Index < NumLightsToUse; Index++)
			{
				LightRanking[Index].bWantShadow = false;
			}
		}

		// re-sort just the head of the light list to bring all the shadowed lights to the front, to allow an optimization in the shader
		// next sort by type to gahter similar lights together to improve coherence
		struct FCompareFLightShadowingThenImportance
		{
			FORCEINLINE bool operator()(const FRayTracedLightImportance& A, const FRayTracedLightImportance& B) const
			{
				if (A.bWantShadow != B.bWantShadow) return A.bWantShadow;
				if (A.LightType != B.LightType) return A.LightType > B.LightType;
				return A.ApproxInfluence > B.ApproxInfluence;
			}
		};
		Sort<FRayTracedLightImportance>(LightRanking.GetData(), NumLightsToUse, FCompareFLightShadowingThenImportance());
	}

	// A light count limit of -1 means don't sort or limit
	return (LightCountLimit >= 0) ? NumLightsToUse : FMath::Min(LightRanking.Num(), RAY_TRACING_LIGHT_COUNT_MAXIMUM);;
}

static void SelectRaytracingLights(
	const TSparseArray<FLightSceneInfoCompact>& Lights,
	const FViewInfo& View,
	TArray<int32>& OutSelectedLights,
	TBitArray<>& OutCastShadow
)
{
	OutSelectedLights.Empty();
	OutCastShadow.Empty();

	TArray<FRayTracedLightImportance> LightRanking;

	int32 NumLights = RankLights(LightRanking, Lights, View);

	check(NumLights <= RAY_TRACING_LIGHT_COUNT_MAXIMUM);

	OutSelectedLights.Reserve(NumLights);
	OutCastShadow.Add(true, Lights.GetMaxIndex() + 1);

	for (int32 LightRank = 0; LightRank < NumLights; LightRank++)
	{
		const auto& Light = LightRanking[LightRank];
		OutSelectedLights.Add(Light.LightIndex);
		OutCastShadow[Light.LightIndex] = Light.bWantShadow;
	}
}

static int32 GetCellsPerDim()
{
	// round to next even as the structure relies on symmetry for address computations
	return (FMath::Max(2, CVarRayTracingLightingCells.GetValueOnRenderThread()) + 1) & (~1); 
}

static void CreateRaytracingLightCullingStructure(
	FRHICommandListImmediate& RHICmdList,
	const TSparseArray<FLightSceneInfoCompact>& Lights,
	const FViewInfo& View,
	const TArray<int32>& LightIndices,
	FRayTracingLightData& OutLightingData)
{
	const int32 NumLightsToUse = LightIndices.Num();

	struct FCullRecord
	{
		uint32 NumLights;
		uint32 Offset;
		uint32 Unused1;
		uint32 Unused2;
	};

	const int32 CellsPerDim = GetCellsPerDim();

	TResourceArray<VectorRegister> RankedLights;
	RankedLights.Reserve(NumLightsToUse);

	// setup light vector array sorted by rank
	for (int32 LightIndex = 0; LightIndex < NumLightsToUse; LightIndex++)
	{
		RankedLights.Push(Lights[LightIndices[LightIndex]].BoundingSphereVector);
	}

	// push null vector to prevent failure in RHICreateStructuredBuffer due to requesting a zero sized allocation
	if (RankedLights.Num() == 0)
	{
		RankedLights.Push(VectorRegister{});
	}

	FRHIResourceCreateInfo CreateInfo;
	CreateInfo.ResourceArray = &RankedLights;

	FStructuredBufferRHIRef RayTracingCullLights = RHICreateStructuredBuffer(sizeof(RankedLights[0]),
		RankedLights.GetResourceDataSize(),
		BUF_Static | BUF_ShaderResource,
		CreateInfo);
	FShaderResourceViewRHIRef RankedLightsSRV = RHICreateShaderResourceView(RayTracingCullLights);

	// Structured buffer version
	FRHIResourceCreateInfo CullStructureCreateInfo;
	CullStructureCreateInfo.DebugName = TEXT("RayTracingLightCullVolume");
	OutLightingData.LightCullVolume = RHICreateStructuredBuffer(sizeof(FUintVector4), CellsPerDim*CellsPerDim*CellsPerDim* sizeof(FUintVector4), BUF_UnorderedAccess | BUF_ShaderResource, CullStructureCreateInfo);
	OutLightingData.LightCullVolumeSRV = RHICmdList.CreateShaderResourceView(OutLightingData.LightCullVolume);
	FUnorderedAccessViewRHIRef LightCullVolumeUAV = RHICmdList.CreateUnorderedAccessView(OutLightingData.LightCullVolume, false, false);

	// ensure zero sized texture isn't requested  to prevent failure in Initialize
	OutLightingData.LightIndices.Initialize(
		sizeof(uint16),
		FMath::Max(NumLightsToUse, 1) * CellsPerDim * CellsPerDim * CellsPerDim,
		EPixelFormat::PF_R16_UINT,
		BUF_UnorderedAccess,
		TEXT("RayTracingLightIndices"));

	{
		auto* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FSetupRayTracingLightCullData> Shader(GlobalShaderMap);

		{
			FSetupRayTracingLightCullData::FParameters Params;
			Params.RankedLights = RankedLightsSRV;

			Params.WorldPos = View.ViewMatrices.GetViewOrigin(); // View.ViewLocation;
			Params.NumLightsToUse = NumLightsToUse;
			Params.LightCullingVolume = LightCullVolumeUAV;
			Params.LightIndices = OutLightingData.LightIndices.UAV;
			Params.CellCount = CellsPerDim;
			Params.CellScale = CVarRayTracingLightingCellSize.GetValueOnRenderThread() / 2.0f; // cells are based on pow2, and initial cell is 2^1, so scale is half min cell size
			{
				SCOPED_GPU_STAT(RHICmdList, LightCullingVolumeCompute);
				FComputeShaderUtils::Dispatch(RHICmdList, Shader, Params, FIntVector(CellsPerDim, CellsPerDim, CellsPerDim));
			}
		}
	}

	{
		FRHITransitionInfo Transitions[] =
		{
			FRHITransitionInfo(LightCullVolumeUAV.GetReference(), ERHIAccess::UAVCompute, ERHIAccess::SRVMask),
			FRHITransitionInfo(OutLightingData.LightIndices.UAV.GetReference(), ERHIAccess::UAVCompute, ERHIAccess::SRVMask)
		};
		RHICmdList.Transition(MakeArrayView(Transitions, UE_ARRAY_COUNT(Transitions)));
	}

}


static void SetupRaytracingLightDataPacked(
	FRHICommandListImmediate& RHICmdList,
	const FScene *Scene,
	const TArray<int32>& LightIndices,
	const TBitArray<>& CastShadow,
	const FViewInfo& View,
	FRaytracingLightDataPacked* LightData,
	TResourceArray<FRTLightingData>& LightDataArray)
{
	const TSparseArray<FLightSceneInfoCompact>& Lights = Scene->Lights;
	TMap<UTextureLightProfile*, int32> IESLightProfilesMap;
	TMap<FRHITexture*, uint32> RectTextureMap;

	const bool bSupportLightFunctions = CVarRayTracingLightFunction.GetValueOnRenderThread() != 0;
	const bool bSupportLightMask = CVarRayTracingLightMask.GetValueOnRenderThread() != 0;

	LightData->Count = 0;
	LightData->LTCMatTexture = GSystemTextures.LTCMat->GetRenderTargetItem().ShaderResourceTexture;
	LightData->LTCMatSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	LightData->LTCAmpTexture = GSystemTextures.LTCAmp->GetRenderTargetItem().ShaderResourceTexture;
	LightData->LTCAmpSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FTextureRHIRef DymmyWhiteTexture = GWhiteTexture->TextureRHI;
	LightData->RectLightTexture0 = DymmyWhiteTexture;
	LightData->RectLightTexture1 = DymmyWhiteTexture;
	LightData->RectLightTexture2 = DymmyWhiteTexture;
	LightData->RectLightTexture3 = DymmyWhiteTexture;
	LightData->RectLightTexture4 = DymmyWhiteTexture;
	LightData->RectLightTexture5 = DymmyWhiteTexture;
	LightData->RectLightTexture6 = DymmyWhiteTexture;
	LightData->RectLightTexture7 = DymmyWhiteTexture;
	static constexpr uint32 MaxRectLightTextureSlos = 8;
	static constexpr uint32 InvalidTextureIndex = 99; // #dxr_todo: share this definition with ray tracing shaders

	{
		// IES profiles
		FRHITexture* IESTextureRHI = nullptr;
		float IESInvProfileCount = 1.0f;

		if (View.IESLightProfileResource && View.IESLightProfileResource->GetIESLightProfilesCount())
		{
			LightData->IESLightProfileTexture = View.IESLightProfileResource->GetTexture();

			uint32 ProfileCount = View.IESLightProfileResource->GetIESLightProfilesCount();
			IESInvProfileCount = ProfileCount ? 1.f / static_cast<float>(ProfileCount) : 0.f;
		}
		else
		{
			LightData->IESLightProfileTexture = GWhiteTexture->TextureRHI;
		}

		LightData->IESLightProfileInvCount = IESInvProfileCount;
		LightData->IESLightProfileTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	}

	for (auto LightIndex : LightIndices)
	{
		auto Light = Lights[LightIndex];
		const bool bHasStaticLighting = Light.LightSceneInfo->Proxy->HasStaticLighting() && Light.LightSceneInfo->IsPrecomputedLightingValid();
		const bool bAffectReflection = Light.LightSceneInfo->Proxy->AffectReflection();
		if (bHasStaticLighting || !bAffectReflection) continue;

		FLightShaderParameters LightParameters;
		Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);

		if (Light.LightSceneInfo->Proxy->IsInverseSquared())
		{
			LightParameters.FalloffExponent = 0;
		}

		int32 IESLightProfileIndex = INDEX_NONE;
		if (View.Family->EngineShowFlags.TexturedLightProfiles)
		{
			UTextureLightProfile* IESLightProfileTexture = Light.LightSceneInfo->Proxy->GetIESTexture();
			if (IESLightProfileTexture)
			{
				int32* IndexFound = IESLightProfilesMap.Find(IESLightProfileTexture);
				if (!IndexFound)
				{
					IESLightProfileIndex = IESLightProfilesMap.Add(IESLightProfileTexture, IESLightProfilesMap.Num());
				}
				else
				{
					IESLightProfileIndex = *IndexFound;
				}
			}
		}

		FRTLightingData LightDataElement;

		LightDataElement.Type = Light.LightType;
		LightDataElement.LightProfileIndex = IESLightProfileIndex;
		LightDataElement.RectLightTextureIndex = InvalidTextureIndex;


		for (int32 Element = 0; Element < 3; Element++)
		{
			LightDataElement.Direction[Element] = LightParameters.Direction[Element];
			LightDataElement.LightPosition[Element] = LightParameters.Position[Element];
			LightDataElement.LightColor[Element] = LightParameters.Color[Element];
			LightDataElement.Tangent[Element] = LightParameters.Tangent[Element];
		}

		// Ray tracing should compute fade parameters ignoring lightmaps
		const FVector2D FadeParams = Light.LightSceneInfo->Proxy->GetDirectionalLightDistanceFadeParameters(View.GetFeatureLevel(), false, View.MaxShadowCascades);
		const FVector2D DistanceFadeMAD = { FadeParams.Y, -FadeParams.X * FadeParams.Y };

		for (int32 Element = 0; Element < 2; Element++)
		{
			LightDataElement.SpotAngles[Element] = LightParameters.SpotAngles[Element];
			LightDataElement.DistanceFadeMAD[Element] = DistanceFadeMAD[Element];
		}

		LightDataElement.InvRadius = LightParameters.InvRadius;
		LightDataElement.SpecularScale = LightParameters.SpecularScale;
		LightDataElement.FalloffExponent = LightParameters.FalloffExponent;
		LightDataElement.SourceRadius = LightParameters.SourceRadius;
		LightDataElement.SourceLength = LightParameters.SourceLength;
		LightDataElement.SoftSourceRadius = LightParameters.SoftSourceRadius;
		LightDataElement.RectLightBarnCosAngle = LightParameters.RectLightBarnCosAngle;
		LightDataElement.RectLightBarnLength = LightParameters.RectLightBarnLength;
		LightDataElement.FlagsLightFunctionAndMask  = 0;

		uint8 Flags = 0;

		if (CastShadow[LightIndex])
		{
			Flags |= LIGHT_FLAGS_CASTS_SHADOW;
		}

		// Stuff directional light's shadow angle factor into a RectLight parameter
		if (Light.LightType == LightType_Directional)
		{
			LightDataElement.RectLightBarnCosAngle = Light.LightSceneInfo->Proxy->GetShadowSourceAngleFactor();
		}

		const int32 *LightFunctionIndex = Scene->RayTracingLightFunctionMap.Find(Light.LightSceneInfo->Proxy->GetLightComponent());

		if (View.Family->EngineShowFlags.LightFunctions && bSupportLightFunctions && LightFunctionIndex && *LightFunctionIndex >= 0)
		{
			// set the light function index, 0 is reserved as none, so the index is offset by 1
			LightDataElement.FlagsLightFunctionAndMask = *LightFunctionIndex + 1;
		}

		// store light channel mask
		uint8 LightMask = bSupportLightMask ? Light.LightSceneInfo->Proxy->GetLightingChannelMask() : 0xff;
		LightDataElement.FlagsLightFunctionAndMask |= LightMask << 8;

		// store Flags
		LightDataElement.FlagsLightFunctionAndMask |= Flags << 16;

		LightDataArray.Add(LightDataElement);

		const bool bRequireTexture = Light.LightType == ELightComponentType::LightType_Rect && LightParameters.SourceTexture;
		uint32 RectLightTextureIndex = InvalidTextureIndex;
		if (bRequireTexture)
		{
			const uint32* IndexFound = RectTextureMap.Find(LightParameters.SourceTexture);
			if (!IndexFound)
			{
				if (RectTextureMap.Num() < MaxRectLightTextureSlos)
				{
					RectLightTextureIndex = RectTextureMap.Num();
					RectTextureMap.Add(LightParameters.SourceTexture, RectLightTextureIndex);
				}
			}
			else
			{
				RectLightTextureIndex = *IndexFound;
			}
		}

		if (RectLightTextureIndex != InvalidTextureIndex)
		{
			LightDataArray[LightData->Count].RectLightTextureIndex = RectLightTextureIndex;
			switch (RectLightTextureIndex)
			{
			case 0: LightData->RectLightTexture0 = LightParameters.SourceTexture; break;
			case 1: LightData->RectLightTexture1 = LightParameters.SourceTexture; break;
			case 2: LightData->RectLightTexture2 = LightParameters.SourceTexture; break;
			case 3: LightData->RectLightTexture3 = LightParameters.SourceTexture; break;
			case 4: LightData->RectLightTexture4 = LightParameters.SourceTexture; break;
			case 5: LightData->RectLightTexture5 = LightParameters.SourceTexture; break;
			case 6: LightData->RectLightTexture6 = LightParameters.SourceTexture; break;
			case 7: LightData->RectLightTexture7 = LightParameters.SourceTexture; break;
			}
		}

		LightData->Count++;
	}

	check(LightData->Count <= RAY_TRACING_LIGHT_COUNT_MAXIMUM);

	// Update IES light profiles texture 
	// TODO (Move to a shared place)
	if (View.IESLightProfileResource != nullptr && IESLightProfilesMap.Num() > 0)
	{
		TArray<UTextureLightProfile*, SceneRenderingAllocator> IESProfilesArray;
		IESProfilesArray.AddUninitialized(IESLightProfilesMap.Num());
		for (auto It = IESLightProfilesMap.CreateIterator(); It; ++It)
		{
			IESProfilesArray[It->Value] = It->Key;
		}

		View.IESLightProfileResource->BuildIESLightProfilesTexture(RHICmdList, IESProfilesArray);
	}
}


FRayTracingLightData CreateRayTracingLightData(
	FRHICommandListImmediate& RHICmdList,
	const FScene* Scene,
	const FViewInfo& View,
	EUniformBufferUsage Usage)
{
	FRayTracingLightData LightingData;
	FRaytracingLightDataPacked LightData;
	TResourceArray<FRTLightingData> LightDataArray;
	TArray<int32> LightIndices;
	TBitArray<> CastShadow;

	SelectRaytracingLights(Scene->Lights, View, LightIndices, CastShadow);

	// Create light culling volume
	CreateRaytracingLightCullingStructure(RHICmdList, Scene->Lights, View, LightIndices, LightingData);
	LightData.CellCount = GetCellsPerDim();
	LightData.CellScale = CVarRayTracingLightingCellSize.GetValueOnRenderThread() / 2.0f;
	LightData.LightCullingVolume = LightingData.LightCullVolumeSRV;
	LightData.LightIndices = LightingData.LightIndices.SRV;

	SetupRaytracingLightDataPacked(RHICmdList, Scene, LightIndices, CastShadow, View, &LightData, LightDataArray);

	check(LightData.Count == LightDataArray.Num());

	// need at least one element
	if (LightDataArray.Num() == 0)
	{
		LightDataArray.AddZeroed(1);
	}

	// This buffer might be best placed as an element of the LightData uniform buffer
	FRHIResourceCreateInfo CreateInfo;
	CreateInfo.ResourceArray = &LightDataArray;

	LightingData.LightBuffer = RHICreateStructuredBuffer(sizeof(FUintVector4), LightDataArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
	LightingData.LightBufferSRV = RHICreateShaderResourceView(LightingData.LightBuffer);

	LightData.LightDataBuffer = LightingData.LightBufferSRV;
	LightData.SSProfilesTexture = View.RayTracingSubSurfaceProfileSRV;

	LightingData.UniformBuffer = CreateUniformBufferImmediate(LightData, Usage);

	return LightingData;
}

class FRayTracingLightingMS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingLightingMS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingLightingMS, FGlobalShader)

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FRaytracingLightDataPacked, LightDataPacked)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingLightingMS, "/Engine/Private/RayTracing/RayTracingLightingMS.usf", "RayTracingLightingMS", SF_RayMiss);

/**
 * FLightFunctionParameters
 * Global constant buffer derived from loose parameters of standard light function materials. Note that it lacks
 * the screen to world transform, as the RT versiosn never have screen as a reference frame
 *
 * function to create the constant buffer is deirved from the LightFunctionMaterial SetParameters code
 */
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FLightFunctionParameters, )
	SHADER_PARAMETER(FMatrix, LightFunctionWorldToLight)
	SHADER_PARAMETER(FVector4, LightFunctionParameters)
	SHADER_PARAMETER(FVector, LightFunctionParameters2)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLightFunctionParameters, "RaytracingLightFunctionParameters");

TUniformBufferRef<FLightFunctionParameters> CreateLightFunctionParametersBuffer(
	const FLightSceneInfo* LightSceneInfo,
	const FSceneView& View,
	EUniformBufferUsage Usage)
{
	FLightFunctionParameters LightFunctionParameters;

	const FVector Scale = LightSceneInfo->Proxy->GetLightFunctionScale();
	// Switch x and z so that z of the user specified scale affects the distance along the light direction
	const FVector InverseScale = FVector(1.f / Scale.Z, 1.f / Scale.Y, 1.f / Scale.X);
	const FMatrix WorldToLight = LightSceneInfo->Proxy->GetWorldToLight() * FScaleMatrix(FVector(InverseScale));

	LightFunctionParameters.LightFunctionWorldToLight = WorldToLight;

	const bool bIsSpotLight = LightSceneInfo->Proxy->GetLightType() == LightType_Spot;
	const bool bIsPointLight = LightSceneInfo->Proxy->GetLightType() == LightType_Point;
	const float TanOuterAngle = bIsSpotLight ? FMath::Tan(LightSceneInfo->Proxy->GetOuterConeAngle()) : 1.0f;

	// should this match raster?
	const float ShadowFadeFraction = 1.0f;

	LightFunctionParameters.LightFunctionParameters = FVector4(TanOuterAngle, ShadowFadeFraction, bIsSpotLight ? 1.0f : 0.0f, bIsPointLight ? 1.0f : 0.0f);

	// do we need this?
	const bool bRenderingPreviewShadowIndicator = false;

	LightFunctionParameters.LightFunctionParameters2 = FVector(
		LightSceneInfo->Proxy->GetLightFunctionFadeDistance(),
		LightSceneInfo->Proxy->GetLightFunctionDisabledBrightness(),
		bRenderingPreviewShadowIndicator ? 1.0f : 0.0f);

	return CreateUniformBufferImmediate(LightFunctionParameters, Usage);
}

/**
 * Generic light function for ray tracing compilalbe as miss shader with lighting or as callable
 */
template<bool IsMissShader>
class TLightFunctionRayTracingShader : public FMaterialShader
{
	DECLARE_SHADER_TYPE(TLightFunctionRayTracingShader, Material);
public:

	/**
	  * Makes sure only shaders for materials that are explicitly flagged
	  * as 'UsedAsLightFunction' in the Material Editor gets compiled into
	  * the shader cache.
	  */
	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.MaterialDomain == MD_LightFunction && ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	TLightFunctionRayTracingShader() {}
	TLightFunctionRayTracingShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
		LightMaterialsParameter.Bind(Initializer.ParameterMap, TEXT("RaytracingLightFunctionParameters"));
		LightDataPacked.Bind(Initializer.ParameterMap, TEXT("RaytracingLightsDataPacked"));
	}

	void GetShaderBindings(
		const FScene* Scene,
		ERHIFeatureLevel::Type FeatureLevel,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material,
		const FViewInfo& View,
		const TUniformBufferRef<FDeferredLightUniformStruct>& DeferredLightBuffer,
		const TUniformBufferRef<FLightFunctionParameters>& LightFunctionParameters,
		const TUniformBufferRef<FRaytracingLightDataPacked>& LightDataPackedBuffer,
		FMeshDrawSingleShaderBindings& ShaderBindings) const
	{
		FMaterialShader::GetShaderBindings(Scene, FeatureLevel, MaterialRenderProxy, Material, ShaderBindings);

		// Bind view
		ShaderBindings.Add(GetUniformBufferParameter<FViewUniformShaderParameters>(), View.ViewUniformBuffer);

		// Bind light parameters
		ShaderBindings.Add(GetUniformBufferParameter<FDeferredLightUniformStruct>(), DeferredLightBuffer);

		//bind Lightfunction parameters
		ShaderBindings.Add(LightMaterialsParameter, LightFunctionParameters);

		//bind light data
		ShaderBindings.Add(LightDataPacked, LightDataPackedBuffer);

	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("SUPPORT_LIGHT_FUNCTION"), 1);
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

private:
	//FShaderUniformBufferParameter LightMaterialsParameter;
	//FShaderUniformBufferParameter LightDataPacked;

	//LAYOUT_FIELD(FShaderParameter, SvPositionToLight);
	LAYOUT_FIELD(FShaderUniformBufferParameter, LightMaterialsParameter);
	LAYOUT_FIELD(FShaderUniformBufferParameter, LightDataPacked);
	//LAYOUT_FIELD(FSceneTextureShaderParameters, SceneTextureParameters);
};

IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TLightFunctionRayTracingShader<true>, TEXT("/Engine/Private/RayTracing/RayTracingLightingMS.usf"), TEXT("RayTracingLightingMS"), SF_RayMiss);

// Ported from LightRendering, create a buffer and return it rather than immediately attaching to a shader
TUniformBufferRef<FDeferredLightUniformStruct> CreateDeferredLightParametersBuffer(
	const FLightSceneInfo* LightSceneInfo,
	const FSceneView& View,
	EUniformBufferUsage Usage)
{
	return CreateUniformBufferImmediate(GetDeferredLightParameters(View, *LightSceneInfo), Usage);
}

class FRayTracingLightFunctionMaterial
{
public:
	FMeshDrawShaderBindings ShaderBindings;

	uint32 MaterialShaderIndex = UINT_MAX;

	/** Sets ray hit group shaders on the mesh command and allocates room for the shader bindings. */
	void SetShaders(const TMaterialProcessorShaders<FMaterialShader>& Shaders, int32 Index)
	{
		MaterialShaderIndex = Index;
		ShaderBindings.Initialize(Shaders);
	}
};

/**
 * Template function to either bind the callable or the miss shader version of the light function
 */
template<bool bIsMissShader>
void BindLightFunction(
	FRHICommandList& RHICmdList,
	const FScene* Scene,
	const FViewInfo& View,
	const FMaterial& Material,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const TUniformBufferRef<FDeferredLightUniformStruct>& DeferredLightBuffer,
	const TUniformBufferRef<FLightFunctionParameters>& LightFunctionParameters,
	int32 Index
	)
{
	FRHIRayTracingScene* RTScene = View.RayTracingScene.RayTracingSceneRHI;
	FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
	const FMaterialShaderMap* MaterialShaderMap = Material.GetRenderingThreadShaderMap();

	FRayTracingLightFunctionMaterial LightFunction;

	auto Shader = MaterialShaderMap->GetShader<TLightFunctionRayTracingShader<bIsMissShader>>();

#if 1
	TMeshProcessorShaders<
		FMaterialShader,
		FMaterialShader,
		FMaterialShader,
		FMaterialShader,
		FMaterialShader,
		TLightFunctionRayTracingShader<bIsMissShader>,
		FMaterialShader> RayTracingShaders;
#else
	TMeshProcessorShaders<
		TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>,
		//TLightFunctionRayTracingShader<bIsMissShader>,
		TLightFunctionRayTracingShader<bIsMissShader>> RayTracingShaders;
#endif

	RayTracingShaders.RayHitGroupShader = Shader;

	// Yes, the following is in fact correct C++ syntax, and certain compilers require it
	LightFunction.SetShaders(RayTracingShaders.template GetUntypedShaders<TMaterialProcessorShaders<FMaterialShader> >(), Index);

	int32 DataOffset = 0;
	FMeshDrawSingleShaderBindings ShaderBindings = LightFunction.ShaderBindings.GetSingleShaderBindings( SF_RayMiss, DataOffset);

	RayTracingShaders.RayHitGroupShader->GetShaderBindings(Scene, Scene->GetFeatureLevel(), MaterialRenderProxy, Material, View, DeferredLightBuffer, LightFunctionParameters, View.RayTracingLightData.UniformBuffer, ShaderBindings);

	if (bIsMissShader)
	{
		LightFunction.ShaderBindings.SetRayTracingShaderBindingsForMissShader(RHICmdList, RTScene, Pipeline, Index, Index);
	}
	else
	{
		check(0);
	}
}

void SetupLightFunction(
	FRHICommandList& RHICmdList,
	const FScene* Scene,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	int32 Index,
	bool bSupportMissShaders)
{
	auto MaterialProxy = LightSceneInfo->Proxy->GetLightFunctionMaterial();

	// Catch the fallback material case
	const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
	const FMaterial& Material = MaterialProxy->GetMaterialWithFallback(Scene->GetFeatureLevel(), FallbackMaterialRenderProxyPtr);

	const FMaterialRenderProxy& MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? *FallbackMaterialRenderProxyPtr : *MaterialProxy;

	//create the uniform buffers we need
	TUniformBufferRef<FDeferredLightUniformStruct> DeferredLightBuffer = CreateDeferredLightParametersBuffer(LightSceneInfo, View, EUniformBufferUsage::UniformBuffer_SingleFrame);
	TUniformBufferRef<FLightFunctionParameters> LightFunctionParameters = CreateLightFunctionParametersBuffer(LightSceneInfo, View, EUniformBufferUsage::UniformBuffer_SingleFrame);


	if (bSupportMissShaders)
	{
		int32 MissIndex = Index + RAY_TRACING_NUM_MISS_SHADER_SLOTS;
		BindLightFunction<true>(RHICmdList, Scene, View, Material, MaterialRenderProxy, DeferredLightBuffer, LightFunctionParameters, MissIndex);
	}
}

FRHIRayTracingShader* FDeferredShadingSceneRenderer::GetRayTracingLightingMissShader(FViewInfo& View)
{
	return View.ShaderMap->GetShader<FRayTracingLightingMS>().GetRayTracingShader();
}

template <typename ShaderType>
void PrepareLightFunctionShaders(
	const TArray<FLightSceneInfo*>& LightFunctionLights,
	const class FViewInfo& View,
	TArray<FRHIRayTracingShader*>& OutShaders)
{
	for (int32 Index = 0; Index < LightFunctionLights.Num(); Index++)
	{
		auto LightSceneInfo = LightFunctionLights[Index];

		auto MaterialProxy = LightSceneInfo->Proxy->GetLightFunctionMaterial();
		if (MaterialProxy)
		{
			const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
			const FMaterial& Material = MaterialProxy->GetMaterialWithFallback(View.GetFeatureLevel(), FallbackMaterialRenderProxyPtr);
			if (Material.IsLightFunction())
			{
				const FMaterialShaderMap* MaterialShaderMap = Material.GetRenderingThreadShaderMap();
				auto Shader = MaterialShaderMap->GetShader<ShaderType>();

				OutShaders.Add(Shader.GetRayTracingShader());
			}
		}
	}
}


void PrepareLightFunctionMissShaders(
	const TArray<FLightSceneInfo*>& LightFunctionLights,
	const class FViewInfo& View,
	TArray<FRHIRayTracingShader*>& OutMissShaders)
{
	PrepareLightFunctionShaders<TLightFunctionRayTracingShader<true>>(LightFunctionLights, View, OutMissShaders);
}

void BindLightFunctionShaders(
	FRHICommandList& RHICmdList,
	const FScene* Scene,
	const TArray<FLightSceneInfo*>& LightFunctionLights,
	const class FViewInfo& View,
	bool bSupportMissShaders)
{
	for (int32 Index = 0; Index < LightFunctionLights.Num(); Index++)
	{
		auto LightSceneInfo = LightFunctionLights[Index];

		auto MaterialProxy = LightSceneInfo->Proxy->GetLightFunctionMaterial();
		if (MaterialProxy && MaterialProxy->GetIncompleteMaterialWithFallback(View.GetFeatureLevel()).IsLightFunction())
		{
			SetupLightFunction(RHICmdList, Scene, View, LightSceneInfo, Index, bSupportMissShaders);
		}
	}
}

template< typename ShaderClass>
static int32 BindParameters(const TShaderRef<ShaderClass>& Shader, typename ShaderClass::FParameters & Parameters, int32 MaxParams, const FRHIUniformBuffer **OutUniformBuffers)
{
	FRayTracingShaderBindingsWriter ResourceBinder;

	auto &ParameterMap = Shader->ParameterMapInfo;

	// all parameters should be in uniform buffers
	check(ParameterMap.LooseParameterBuffers.Num() == 0);
	check(ParameterMap.SRVs.Num() == 0);
	check(ParameterMap.TextureSamplers.Num() == 0);

	SetShaderParameters(ResourceBinder, Shader, Parameters);

	FMemory::Memzero(OutUniformBuffers, sizeof(FRHIUniformBuffer *)*MaxParams);

	const int32 NumUniformBuffers = ParameterMap.UniformBuffers.Num();

	int32 MaxUniformBufferUsed = -1;
	for (int32 UniformBufferIndex = 0; UniformBufferIndex < NumUniformBuffers; UniformBufferIndex++)
	{
		const FShaderParameterInfo &Parameter = ParameterMap.UniformBuffers[UniformBufferIndex];
		checkSlow(Parameter.BaseIndex < MaxParams);
		const FRHIUniformBuffer* UniformBuffer = ResourceBinder.UniformBuffers[UniformBufferIndex];
		if (Parameter.BaseIndex < MaxParams)
		{
			OutUniformBuffers[Parameter.BaseIndex] = UniformBuffer;
			MaxUniformBufferUsed = FMath::Max((int32)Parameter.BaseIndex, MaxUniformBufferUsed);
		}
	}

	return MaxUniformBufferUsed + 1;
}

void FDeferredShadingSceneRenderer::SetupRayTracingLightingMissShader(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	FRayTracingLightingMS::FParameters MissParameters;
	MissParameters.LightDataPacked = View.RayTracingLightData.UniformBuffer;
	MissParameters.ViewUniformBuffer = View.ViewUniformBuffer;

	static constexpr uint32 MaxUniformBuffers = UE_ARRAY_COUNT(FRayTracingShaderBindings::UniformBuffers);
	const FRHIUniformBuffer* MissData[MaxUniformBuffers] = {};
	auto MissShader = View.ShaderMap->GetShader<FRayTracingLightingMS>();

	int32 ParameterSlots = BindParameters(MissShader, MissParameters, MaxUniformBuffers, MissData);

	RHICmdList.SetRayTracingMissShader(View.RayTracingScene.RayTracingSceneRHI,
		RAY_TRACING_MISS_SHADER_SLOT_LIGHTING, // Shader slot in the scene
		View.RayTracingMaterialPipeline,
		RAY_TRACING_MISS_SHADER_SLOT_LIGHTING, // Miss shader index in the pipeline
		ParameterSlots, (FRHIUniformBuffer**)MissData, 0);
}

void GatherLightFunctionLights(FScene* Scene, ERHIFeatureLevel::Type InFeatureLevel, bool bSupportMissShaders)
{
	Scene->RayTracingLightFunctionMap.Empty();
	Scene->RayTracingLightFunctionLights.Empty();

	if (bSupportMissShaders)
	{
		for (TSparseArray<FLightSceneInfoCompact>::TIterator LightIt(Scene->Lights); LightIt; ++LightIt)
		{
			FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
			FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

			auto MaterialProxy = LightSceneInfo->Proxy->GetLightFunctionMaterial();
			if ( MaterialProxy && MaterialProxy->GetIncompleteMaterialWithFallback(InFeatureLevel).IsLightFunction())
			{
				const ULightComponent *Component = LightSceneInfo->Proxy->GetLightComponent();
				int32 Index = Scene->RayTracingLightFunctionMap.Num();

				if (Index < RAY_TRACING_MAX_LIGHT_FUNCTIONS)
				{
					Scene->RayTracingLightFunctionMap.Add(Component, Index);
					Scene->RayTracingLightFunctionLights.Add(LightSceneInfo);
				}
				else
				{
					Scene->RayTracingLightFunctionMap.Add(Component, -1);
				}
			}
		}
	}

}

#endif // RHI_RAYTRACING
