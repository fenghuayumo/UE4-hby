#include "LightCutRendering.h"


#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "SystemTextures.h"

// UE4 Private Interfaces
#include "PostProcess/SceneRenderTargets.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "PathTracing.h"
#include "RayTracingTypes.h"
#include "RayTracingDefinitions.h"
#include "PathTracingDefinitions.h"
#include "RenderGraphUtils.h"
#include "BitonicSortUtils.h"
#include "LightTreeTypes.h"
#include <limits>

TAutoConsoleVariable<bool> CVarVizLightNodeEnable(
	TEXT("r.LightCut.VizLightNode"),
	false,
	TEXT("Whether to visualize Light node (default = false)"),
	ECVF_RenderThreadSafe);


TAutoConsoleVariable<int> CVarVizLightTreeLevel(
	TEXT("r.LightCut.VizLightTreeLevel"),
	0,
	TEXT("Visualize LightTreeLevel)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int> CVarUseApproximateCosineBound(
	TEXT("r.LightCut.UseApproximateCosineBound"),
	1,
	TEXT("Use ApproximateCosineBound)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int> CVarCutBlockSize(
	TEXT("r.LightCut.CutBlockSize"),
	8,
	TEXT("Set Light Cut BlockSize)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int> CVarCutSharing(
	TEXT("r.LightCut.CutShare"),
	1,
	TEXT("Enable or Disable Cut Sharing)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int> CVarMaxCutNodes(
	TEXT("r.LightCut.MaxCutNodes"),
	8,
	TEXT("Set Max Light Cut Nodes)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> CVarErrorLimit(
	TEXT("r.LightCut.ErrorLimit"),
	0.001f,
	TEXT("Set Error Limit)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int> CVarInterleaveRate(
	TEXT("r.LightCut.InterleaveRate"),
	1,
	TEXT("Set Interleave Rate)"),
	ECVF_RenderThreadSafe
);

TAutoConsoleVariable<int>&  GetCVarInterleaveRate()
{
	return CVarInterleaveRate;
}

TAutoConsoleVariable<float>& GetCVarErrorLimit()
{
	return CVarErrorLimit;
}

TAutoConsoleVariable<int>& GetCVarMaxCutNodes()
{
	return CVarMaxCutNodes;
}

TAutoConsoleVariable<int>& GetCVarCutBlockSize()
{
	return CVarCutBlockSize;
}

TAutoConsoleVariable<int>& GetCVarCutSharing()
{
	return CVarCutSharing;
}

TAutoConsoleVariable<int>& GetCVarUseApproximateCosineBound()
{
	return CVarUseApproximateCosineBound;
}

BEGIN_SHADER_PARAMETER_STRUCT(FFindLightCutsShaderParameters, )
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLightNode>, NodesBuffer)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, LightCutBuffer)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
SHADER_PARAMETER(int, MaxCutNodes)
SHADER_PARAMETER(int, CutShareGroupSize)
SHADER_PARAMETER(float, ErrorLimit)
SHADER_PARAMETER(int, UseApproximateCosineBound)
SHADER_PARAMETER(float, SceneLightBoundRadius)
SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
END_SHADER_PARAMETER_STRUCT()


BEGIN_SHADER_PARAMETER_STRUCT(FVisualizeShaderParameters, )
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVizLightNode>, VizNodes)
SHADER_PARAMETER(FMatrix, MVP)
SHADER_PARAMETER(int, ShowLevel)
RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FFindLightCutsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFindLightCutsCS);
	SHADER_USE_PARAMETER_STRUCT(FFindLightCutsCS, FGlobalShader);

	using FParameters = FFindLightCutsShaderParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREAD_BLOCK_SIZE"), GetThreadBlockSize());
	}
	static uint32 GetThreadBlockSize()
	{
		return 16;
	}
};


class FGenerateMortonCodeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateMortonCodeCS)
	SHADER_USE_PARAMETER_STRUCT(FGenerateMortonCodeCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("LIGHT_CUT_THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("GEN_MORTONCODE"), 1);
	}

	static uint32 GetThreadBlockSize() 
	{
		return 512;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, keyIndexList)
		SHADER_PARAMETER(int, QuantLevels)

		SHADER_PARAMETER(FVector, SceneLightBoundsMin)
		SHADER_PARAMETER(FVector, SceneLightDimension)
		SHADER_PARAMETER(uint32, SceneInfiniteLightCount)
	END_SHADER_PARAMETER_STRUCT()
};

class FGenerateLevelZeroCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateLevelZeroCS)
	SHADER_USE_PARAMETER_STRUCT(FGenerateLevelZeroCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("LIGHT_CUT_THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("GEN_LEVEL_ZERO"), 1);
	}
	static uint32 GetThreadBlockSize() 
	{
		return 512;
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, LevelLightCount)
		SHADER_PARAMETER(int, LevelsNumber)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, keyIndexList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FLightNode>, LightNodes)
		SHADER_PARAMETER(uint32, SceneInfiniteLightCount)
	END_SHADER_PARAMETER_STRUCT()
};

class FGenerateLevelUpCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateLevelUpCS)
	SHADER_USE_PARAMETER_STRUCT(FGenerateLevelUpCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("LIGHT_CUT_THREAD_BLOCK_SIZE"), GetThreadBlockSize());
		OutEnvironment.SetDefine(TEXT("GEN_LEVEL_UP"), 1);
	}
	static uint32 GetThreadBlockSize() 
	{
		return 512;
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, SrcLevel)
		SHADER_PARAMETER(int, DstLevelStart)
		SHADER_PARAMETER(int, DstLevelEnd)
		SHADER_PARAMETER(int, NumLevels)
		SHADER_PARAMETER(int, NumDstLevelsLights)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FLightNode>, LightNodes)
	END_SHADER_PARAMETER_STRUCT()
};

class FBuildVizNodeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildVizNodeCS)
	SHADER_USE_PARAMETER_STRUCT(FBuildVizNodeCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), GetThreadBlockSize());
	}
	static uint32 GetThreadBlockSize()
	{
		return 512;
	}
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVizLightNode>, VizNodes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLightNode>, LightNodes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector2<uint32>>, NodeBLASId)
		SHADER_PARAMETER(int, NumNodes)
		SHADER_PARAMETER(int, NeedLevelIds)
	END_SHADER_PARAMETER_STRUCT()
};


class FVisualizeNodeShaderVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVisualizeNodeShaderVS);
	SHADER_USE_PARAMETER_STRUCT(FVisualizeNodeShaderVS, FGlobalShader);

	using FParameters = FVisualizeShaderParameters;
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

};

class FVisualizeNodeShaderPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVisualizeNodeShaderPS);
	SHADER_USE_PARAMETER_STRUCT(FVisualizeNodeShaderPS, FGlobalShader);
	using FParameters = FVisualizeShaderParameters;
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// needed for a typed UAV load. This already assumes we are raytracing, so should be fine.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FGenerateMortonCodeCS, "/Engine/Private/LightCut/GenerateLevel.usf", "GenMortonCode", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGenerateLevelZeroCS, "/Engine/Private/LightCut/GenerateLevel.usf", "GenerateLevelZeroFromLights", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGenerateLevelUpCS, "/Engine/Private/LightCut/GenerateLevel.usf", "GenerateLevelsFromsLevelZero", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFindLightCutsCS, "/Engine/Private/LightCut/LightCutFinder.usf", "LightCutFindCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildVizNodeCS, "/Engine/Private/LightCut/BuildVisNode.usf", "BuildVisNodeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVisualizeNodeShaderVS, "/Engine/Private/LightCut/VisualizeLightNode.usf", "ViszLightTreeVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVisualizeNodeShaderPS, "/Engine/Private/LightCut/VisualizeLightNode.usf", "ViszLightTreePS", SF_Pixel);


DECLARE_GPU_STAT_NAMED(LightTreeBuild, TEXT("Light Tree Build"));
DECLARE_GPU_STAT_NAMED(MortonCodeSort, TEXT("MortonCodeSort"));
DECLARE_GPU_STAT_NAMED(LightTreeGenerateLevelZero, TEXT("GenerateLevelZero"));
DECLARE_GPU_STAT_NAMED(LightTreeGenerateInternalLevels, TEXT("GenerateInternalLevels"));
DECLARE_GPU_STAT_NAMED(LightNode_Visualizations, TEXT("LightNode Visualizations"));
DECLARE_GPU_STAT_NAMED(LightCutsFinder, TEXT("Find LightCuts"));

void LightTree::Init(int _numLights, int _quantizationLevels)
{
	NumLights = _numLights;
	QuantizationLevels = _quantizationLevels;
	NumFiniteLights = NumLights - SceneInfiniteLightCount;
	int numBboxGroups = (RAY_TRACING_LIGHT_COUNT_MAXIMUM + 1023) / 1024;

	// compute nearest power of two
	
	NumTreeLevels = CalculateTreeLevels(NumFiniteLights);
	NumTreeLights = 1 << (NumTreeLevels - 1);//the light number of the leaf level 
}

void LightTree::Build(FRDGBuilder& GraphBuilder, int lightCounts,int InfiniteLightCount, const FVector& SceneLightBoundMin, const FVector& SceneLightBoundMax, FRDGBufferSRV* LightsSRV, int frameId)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, LightTreeBuild);
	RDG_EVENT_SCOPE(GraphBuilder, "Build Ligh Tree");

	bEnableNodeViz = CVarVizLightNodeEnable.GetValueOnRenderThread();

	SceneInfiniteLightCount = InfiniteLightCount;
	Init(lightCounts, 1024);

	uint32 numStorageNodes = 2 * NumTreeLights;

	{
		size_t DataSize = sizeof(FLightNode) * FMath::Max(numStorageNodes, 1u);
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FLightNode), FMath::Max(numStorageNodes, 1u));
		LightNodesBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("LightNodesBuffer")/*, ERDGBufferFlags::MultiFrame*/);
		BLASViZBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVizLightNode), FMath::Max(numStorageNodes, 1u)), TEXT("Viz Nodes"));
		IndexKeyList = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint64), FMath::Max(NumTreeLights, 1u)), TEXT("GPU Sort List"));
		uint32_t ListCount[1] = { NumFiniteLights };
		ListCounter = CreateVertexBuffer(GraphBuilder, TEXT("GPU List Counter"), FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), ListCount, sizeof(ListCount));
	}
	
	Sort(GraphBuilder, SceneLightBoundMin, SceneLightBoundMax, LightsSRV);
	// fill level zero
	GenerateLevelZero(GraphBuilder, SceneLightBoundMin, SceneLightBoundMax, LightsSRV);
	GenerateInternalLevels(GraphBuilder, 4);
	if (bEnableNodeViz)
	{
		BuildVizNodes(GraphBuilder,2 * NumTreeLights, false);
	}
}

void LightTree::Sort(FRDGBuilder& GraphBuilder, 
	const FVector& SceneLightBoundMin, 
	const FVector& SceneLightBoundMax, 
	FRDGBufferSRV* LightsSRV )
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, MortonCodeSort);
	RDG_EVENT_SCOPE(GraphBuilder, "MortonCodeSort");

	//Generate Morton Code
	TShaderMapRef<FGenerateMortonCodeCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	FGenerateMortonCodeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateMortonCodeCS::FParameters>();
	PassParameters->keyIndexList = GraphBuilder.CreateUAV(IndexKeyList, EPixelFormat::PF_R8_UINT);
	PassParameters->QuantLevels = QuantizationLevels;
	PassParameters->SceneLightCount = NumLights;
	PassParameters->SceneLights = LightsSRV;
	PassParameters->SceneLightDimension = SceneLightBoundMax - SceneLightBoundMin;
	PassParameters->SceneLightBoundsMin = SceneLightBoundMin;
	PassParameters->SceneInfiniteLightCount = SceneInfiniteLightCount;
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateMortonCode"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(NumFiniteLights, FGenerateMortonCodeCS::GetThreadBlockSize()));

	FBitonicSortUtils::Sort(GraphBuilder, IndexKeyList, ListCounter, 0, false, true);
}

void LightTree::GenerateLevelZero(FRDGBuilder& GraphBuilder,
	const FVector& SceneLightBoundMin, 
	const FVector& SceneLightBoundMax, 
	FRDGBufferSRV* LightsSRV)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, LightTreeGenerateLevelZero);
	RDG_EVENT_SCOPE(GraphBuilder, "GenerateLevelZero");
	TShaderMapRef<FGenerateLevelZeroCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	FGenerateLevelZeroCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateLevelZeroCS::FParameters>();
	PassParameters->keyIndexList = GraphBuilder.CreateSRV(IndexKeyList, EPixelFormat::PF_R8_UINT);
	PassParameters->LevelsNumber = NumTreeLevels;
	PassParameters->LevelLightCount = NumTreeLights;
	PassParameters->SceneLightCount = NumLights;
	PassParameters->SceneLights = LightsSRV;
	PassParameters->LightNodes = GraphBuilder.CreateUAV(LightNodesBuffer);
	PassParameters->SceneInfiniteLightCount = SceneInfiniteLightCount;
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateLevelZero"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(NumTreeLights, FGenerateLevelZeroCS::GetThreadBlockSize()));
}

void LightTree::GenerateInternalLevels(FRDGBuilder& GraphBuilder, int levelGroupSize)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, LightTreeGenerateInternalLevels);
	RDG_EVENT_SCOPE(GraphBuilder, "GenerateInternalLevels");
	{
		const int maxWorkLoad = 2048;
		int srcLevel = 0;
		for (uint32 dstLevelStart = 1; dstLevelStart < NumTreeLevels; )
		{
			uint32 dstLevelEnd;
			int workLoad = 0;
			for (dstLevelEnd = dstLevelStart + 1; dstLevelEnd < NumTreeLevels; dstLevelEnd++)
			{
				workLoad += 1 << (NumTreeLevels - 1 - srcLevel);
				if (workLoad > maxWorkLoad) break;
			}
			GenerateMultipleLevels(GraphBuilder, srcLevel, dstLevelStart, dstLevelEnd);

			srcLevel = dstLevelEnd - 1;
			dstLevelStart = dstLevelEnd;
		}
	}
}

void LightTree::GenerateMultipleLevels(FRDGBuilder& GraphBuilder, int srcLevel, int dstLevelStart, int dstLevelEnd)
{
	TShaderMapRef<FGenerateLevelUpCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	FGenerateLevelUpCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateLevelUpCS::FParameters>();
	PassParameters->SrcLevel = srcLevel;
	PassParameters->DstLevelStart = dstLevelStart;
	PassParameters->DstLevelEnd = dstLevelEnd;
	PassParameters->NumLevels = NumTreeLevels;
	PassParameters->NumDstLevelsLights = (1 << (NumTreeLevels - dstLevelStart)) - (1 << (NumTreeLevels - dstLevelEnd));
	PassParameters->LightNodes = GraphBuilder.CreateUAV(LightNodesBuffer);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateMultipleLevels"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(PassParameters->NumDstLevelsLights, FGenerateLevelUpCS::GetThreadBlockSize()));
}

void LightTree::FindLightCuts(
		const FScene& Scene, 
		const FViewInfo& View, 
		FRDGBuilder& GraphBuilder,
		const FVector& LightBoundMin, 
		const FVector& LightBoundMax)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, LightCutsFinder);
	RDG_EVENT_SCOPE(GraphBuilder, "LightCutsFinder");

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef GBufferATexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferA);
	FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);

	float ScreenScale = 1.0;
	uint32 ScaledViewSizeX = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().X * ScreenScale));
	uint32 ScaledViewSizeY = FMath::Max(1, FMath::CeilToInt(View.ViewRect.Size().Y * ScreenScale));
	FIntPoint ScaledViewSize = FIntPoint(ScaledViewSizeX, ScaledViewSizeY);

	int CutBlockSize = CVarCutBlockSize.GetValueOnRenderThread();
	FIntPoint DispatchResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), CutBlockSize);
	TShaderMapRef<FFindLightCutsCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	FFindLightCutsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFindLightCutsCS::FParameters>();
	LightCutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32),  MAX_CUT_NODES * ((View.ViewRect.Size().X + 7) / 8) * ((View.ViewRect.Size().Y + 7) / 8)), TEXT("Light cut buffer"));
	PassParameters->NodesBuffer = GraphBuilder.CreateSRV(LightNodesBuffer);
	PassParameters->LightCutBuffer = GraphBuilder.CreateUAV(LightCutBuffer);
	PassParameters->MaxCutNodes = CVarMaxCutNodes.GetValueOnRenderThread();
	PassParameters->CutShareGroupSize = CVarCutBlockSize.GetValueOnRenderThread();
	PassParameters->ErrorLimit = CVarErrorLimit.GetValueOnRenderThread();
	PassParameters->UseApproximateCosineBound = CVarUseApproximateCosineBound.GetValueOnRenderThread();
	auto LightBound = (LightBoundMax - LightBoundMin) * 0.5;
	PassParameters->SceneLightBoundRadius = LightBound.Size();
	
	PassParameters->NormalTexture = GBufferATexture;
	PassParameters->DepthTexture = SceneDepthTexture;
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->LinearClampSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->ScaledViewSizeAndInvSize = FVector4(ScaledViewSize.X, ScaledViewSize.Y, 1.0f / ScaledViewSize.X, 1.0f / ScaledViewSize.Y);
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	ClearUnusedGraphResources(ComputeShader, PassParameters);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("LightCutsFinder"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(DispatchResolution, FFindLightCutsCS::GetThreadBlockSize()));
}

void LightTree::BuildVizNodes(FRDGBuilder& GraphBuilder, int numNodes, bool needLevelIds)
{
	TShaderMapRef<FBuildVizNodeCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	FBuildVizNodeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBuildVizNodeCS::FParameters>();
	PassParameters->NumNodes = numNodes;
	PassParameters->NeedLevelIds = needLevelIds;
	PassParameters->VizNodes = GraphBuilder.CreateUAV(BLASViZBuffer);
	PassParameters->LightNodes = GraphBuilder.CreateSRV(LightNodesBuffer);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateLevelZero"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(numNodes, FBuildVizNodeCS::GetThreadBlockSize()));
}

void LightTree::VisualizeNodesLevel(
	const FScene& Scene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder)
{
	if (bEnableNodeViz)
	{
		auto level = CVarVizLightTreeLevel.GetValueOnRenderThread();
		VisualizeNodes(Scene, View, GraphBuilder, level);
	}
}

/**
* VizNode vertex buffer. Define Cube
*/
class FVizNodeVertexBuffer : public FVertexBuffer
{
public:
	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{

		TResourceArray<FVector4, VERTEXBUFFER_ALIGNMENT> Verts;
		//top
		Verts.Add(FVector(-1.0f, -1.0f, -1.0f));
		Verts.Add(FVector(-1.0f, 1.0f, -1.0f));

		Verts.Add(FVector(1.0f, 1.0f, -1.0f));
		Verts.Add(FVector(1.0f, -1.0f, -1.f));

		Verts.Add(FVector(-1.0f, 1.0f, -1.0f));
		Verts.Add(FVector(1.0f, 1.0f, -1.f));

		Verts.Add(FVector(-1.0f, -1.0f, -1.0f));
		Verts.Add(FVector(1.0f, -1.0f, -1.f));

		//buttom
		Verts.Add(FVector(-1.0f, -1.0f, 1.0f));
		Verts.Add(FVector(-1.0f, 1.0f, 1.0f));
		
		Verts.Add(FVector(1.0f, 1.0f, 1.0f));
		Verts.Add(FVector(1.0f, -1.0f, 1.f));

		Verts.Add(FVector(-1.0f, 1.0f, 1.0f));
		Verts.Add(FVector(1.0f, 1.0f, 1.f));

		Verts.Add(FVector(-1.0f, -1.0f, 1.0f));
		Verts.Add(FVector(1.0f, -1.0f, 1.f));

		//
		Verts.Add(FVector(1.0f, 1.0f, -1.0f));
		Verts.Add(FVector(1.0f, 1.0f, 1.0f));

		Verts.Add(FVector(1.0f, -1.0f, -1.0f));
		Verts.Add(FVector(1.0f, -1.0f, 1.0f));

		Verts.Add(FVector(-1.0f, 1.0f, -1.0f));
		Verts.Add(FVector(-1.0f, 1.0f, 1.0f));

		Verts.Add(FVector(-1.0f, -1.0f, -1.0f));
		Verts.Add(FVector(-1.0f, -1.0f, 1.0f));

		//Verts.Add(FVector(-1.0f, -1.0f, -1.0f));
		//Verts.Add(FVector(-1.0f, 1.0f, -1.0f));

		//Verts.Add(FVector(1.0f, 1.0f, -1.0f));
		//Verts.Add(FVector(1.0f, -1.0f, -1.f));

		//Verts.Add(FVector(-1.0f, -1.0f, 1.0f));
		//Verts.Add(FVector(-1.0f, 1.0f, 1.0f));

		//Verts.Add(FVector(1.0f, 1.0f, 1.0f));
		//Verts.Add(FVector(1.0f, -1.0f, 1.f));
		NumVerts = Verts.Num();
		uint32 Size = Verts.GetResourceDataSize();

		// Create vertex buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(&Verts);
		VertexBufferRHI = RHICreateVertexBuffer(Size, BUF_Static, CreateInfo);
	}

	int32 GetVertexCount() const { return NumVerts; }

private:
	int32 NumVerts;
};

/**
* VizNode index buffer.
*/
class FVizNodeIndexBuffer : public FIndexBuffer
{
public:
	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{
		uint32_t indices[] = {
			// bottom
			0, 1, 2, 0, 2, 3,
			// top
			4, 5, 6, 4, 6, 7,
			// front
			0, 1, 5, 0, 5, 4,
			// back
			3, 2, 6, 3, 6, 7,
			// right
			1, 2, 6, 1, 6, 5,
			// left
			0, 3, 7, 0, 7, 4
		};
		TResourceArray<uint16, INDEXBUFFER_ALIGNMENT> Indices;

		// Add triangles for all the vertices generated
		for (int32 s = 0; s < 36; s++)
		{
			Indices.Add(indices[s]);
		}

		NumIndices = Indices.Num();
		const uint32 Size = Indices.GetResourceDataSize();
		const uint32 Stride = sizeof(uint16);

		// Create index buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(&Indices);
		IndexBufferRHI = RHICreateIndexBuffer(Stride, Size, BUF_Static, CreateInfo);
	}

	int32 GetIndexCount() const { return NumIndices; };

private:
	int32 NumIndices;
};

struct FVisualNodeVertex
{
	FVector4 Position;
	FVisualNodeVertex() {}
	FVisualNodeVertex(const FVector4& InPosition) : Position(InPosition) {}
};

class FVisualizeNodesVertexDeclaration : public FRenderResource
{
public:

	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual ~FVisualizeNodesVertexDeclaration() {}

	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		uint16 Stride = sizeof(FVisualNodeVertex);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FVisualNodeVertex, Position), VET_Float4, 0, Stride));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVisualizeNodesVertexDeclaration> GVisualizeNodeVertexDeclaration;
TGlobalResource<FVizNodeVertexBuffer> GVizNodeVertexBuffer;
TGlobalResource<FVizNodeIndexBuffer> GVizNodeIndexBuffer;


void LightTree::VisualizeNodes(
	const FScene& Scene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder,
	int showLevel)
{

	RDG_GPU_STAT_SCOPE(GraphBuilder, LightNode_Visualizations);
	RDG_EVENT_SCOPE(GraphBuilder, "LightNode Visualizations");

	FIntRect ViewRect = View.ViewRect;
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor());
	FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);

	// Get the shader permutation
	FVisualizeNodeShaderVS::FPermutationDomain PermutationVectorVS;
	FVisualizeNodeShaderPS::FPermutationDomain PermutationVectorPS;

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
	TShaderMapRef<FVisualizeNodeShaderVS> VertexShader(GlobalShaderMap, PermutationVectorVS);
	TShaderMapRef<FVisualizeNodeShaderPS> PixelShader(GlobalShaderMap, PermutationVectorPS);

	// Set shader pass parameters
	FVisualizeShaderParameters DefaultPassParameters;
	FVisualizeShaderParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeShaderParameters>();
	*PassParameters = DefaultPassParameters;

	PassParameters->ShowLevel = showLevel - 1;
	PassParameters->MVP = View.ViewMatrices.GetViewProjectionMatrix();
	PassParameters->VizNodes = GraphBuilder.CreateSRV(BLASViZBuffer);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

	uint32 NumInstances = uint32(BLASViZBuffer->Desc.NumElements);

	GraphBuilder.AddPass(
		Forward<FRDGEventName>(RDG_EVENT_NAME("Visualize Nodes")),
		PassParameters,
		ERDGPassFlags::Raster,
		[PassParameters, GlobalShaderMap, VertexShader, PixelShader, ViewRect, NumInstances](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Wireframe, CM_CCW>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
			GraphicsPSOInit.BlendState = TStaticBlendStateWriteMask<CW_RGB, CW_RGBA>::GetRHI();

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVisualizeNodeVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_LineList;
			
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			RHICmdList.SetStreamSource(0, GVizNodeVertexBuffer.VertexBufferRHI, 0); 
			RHICmdList.DrawPrimitive(0, GVizNodeVertexBuffer.GetVertexCount() / 2, NumInstances);
	/*		RHICmdList.DrawIndexedPrimitive(GVizNodeIndexBuffer.IndexBufferRHI, 0, 0, GVizNodeVertexBuffer.GetVertexCount(), 0, GVizNodeIndexBuffer.GetIndexCount() / 3, NumInstances);*/
		}
	);
}