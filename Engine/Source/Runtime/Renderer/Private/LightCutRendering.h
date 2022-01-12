#pragma once
#include "ShaderParameterMacros.h"

class FRDGBuilder;
class FScene;
class FViewInfo;
struct IPooledRenderTarget;
class FRDGPooledBuffer;
class FRDGBuffer;
using FRDGBufferRef = FRDGBuffer*;
class FRDGBufferSRV;
//class FShaderResourceViewRHI;
//using FShaderResourceViewRHIRef = FShaderResourceViewRHI*;
BEGIN_SHADER_PARAMETER_STRUCT(FLightCutCommonParameter,)
SHADER_PARAMETER(int, MaxCutNodes)
SHADER_PARAMETER(int, CutShareGroupSize)
SHADER_PARAMETER(float, ErrorLimit)
SHADER_PARAMETER(int, UseApproximateCosineBound)
SHADER_PARAMETER(int, InterleaveRate)
END_SHADER_PARAMETER_STRUCT()

class LightTree
{
public:


	int GetLeafStartIndex()
	{
		return 1 << (NumTreeLevels - 1);
	};

	inline int CalculateTreeLevels(uint32_t numLights)
	{
		if (numLights == 1)
			return 2;
		return FMath::CeilLogTwo(numLights) + 1;
	}

	void Init(int _numLights,int _quantizationLevels);

	void Build(FRDGBuilder& GraphBuilder, 
		int lightCounts,
		int SceneInfiniteLightCount,
		const FVector& SceneLightBoundMin,
		const FVector& SceneLightBoundMax,
		FRDGBufferSRV* LightsSRV);

	void Sort(FRDGBuilder& GraphBuilder, 
		const FVector& SceneLightBoundMin,
		const FVector& SceneLightBoundMax,
		FRDGBufferSRV* LightsSRV);

	void GenerateLevelZero(FRDGBuilder& GraphBuilder,
		const FVector& SceneLightBoundMin,
		const FVector& SceneLightBoundMax,
		 FRDGBufferSRV* LightsSRV);

	void GenerateInternalLevels(FRDGBuilder& GraphBuilder);
	void GenerateMultipleLevels(FRDGBuilder& GraphBuilder, int srcLevel, int dstLevelStart, int dstLevelEnd);

	void FindLightCuts(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		const FVector& LightBoundMin,
		const FVector& LightBoundMax,
		float	ScreenUpScale = 1.0);

	void VisualizeNodes(
		const FScene& Scene,
		const FViewInfo& View, 
		FRDGBuilder& GraphBuilder, 
		int showLevel);

	void BuildVizNodes(FRDGBuilder& GraphBuilder, int numNodes);

	void VisualizeNodesLevel(const FScene& Scene,
		const FViewInfo& View, 
		FRDGBuilder& GraphBuilder);

	uint32_t NumLights;
	uint32_t QuantizationLevels;
	uint32_t NumTreeLevels;
	uint32_t NumTreeLights;
	uint32_t SceneInfiniteLightCount;
	uint32_t NumFiniteLights;
	bool bEnableNodeViz = false;

public:
	FRDGBufferRef	LightNodesBuffer, BLASViZBuffer, IndexKeyList, ListCounter, LightCutBuffer;
};

class MeshLightTree
{
public:

	int GetLeafStartIndex()
	{
		return 1 << (NumTreeLevels - 1);
	};

	inline int CalculateTreeLevels(uint32_t numLights)
	{
		if (numLights == 1)
			return 2;
		return FMath::CeilLogTwo(numLights) + 1;
	}

	void Build(FRDGBuilder& GraphBuilder,
		int TriLightCount,
		const FVector& SceneLightBoundMin,
		const FVector& SceneLightBoundMax,
		FShaderResourceViewRHIRef MeshLightIndexBuffer,
		FShaderResourceViewRHIRef MeshLightVertexBuffer,
		FShaderResourceViewRHIRef MeshLightInstanceBuffer,
		FShaderResourceViewRHIRef MeshLightInstancePrimitiveBuffer);

	void Sort(FRDGBuilder& GraphBuilder,
		const FVector& SceneLightBoundMin,
		const FVector& SceneLightBoundMax);

	void GenerateLeafNodes(FRDGBuilder& GraphBuilder, 
		FShaderResourceViewRHIRef MeshLightIndexBuffer,
		FShaderResourceViewRHIRef MeshLightVertexBuffer,
		FShaderResourceViewRHIRef MeshLightInstanceBuffer,
		FShaderResourceViewRHIRef MeshLightInstancePrimitiveBuffer);

	void GenerateInternalNodes(FRDGBuilder& GraphBuilder);
	void GenerateMultipleLevels(FRDGBuilder& GraphBuilder, int srcLevel, int dstLevelStart, int dstLevelEnd);

	void FindLightCuts(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		const FVector& LightBoundMin,
		const FVector& LightBoundMax,
		float	ScreenUpScale = 1.0);

	void VisualizeNodes(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		int showLevel);

	void BuildVizNodes(FRDGBuilder& GraphBuilder, int numNodes);

	void VisualizeNodesLevel(const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder);

	uint32_t NumTriLights;
	uint32_t QuantizationLevels;
	uint32_t NumTreeLevels;
	uint32_t NumTreeLights;
	bool bEnableNodeViz = false;

public:
	FRDGBufferRef	LightNodesBuffer, BLASViZBuffer, IndexKeyList, ListCounter, LightCutBuffer;
	FRDGBufferRef	LeafNodesBuffer;
};

extern TAutoConsoleVariable<int>& GetCVarInterleaveRate();
extern TAutoConsoleVariable<float>& GetCVarErrorLimit();
extern TAutoConsoleVariable<int>& GetCVarMaxCutNodes();
extern TAutoConsoleVariable<int>& GetCVarCutBlockSize();
extern TAutoConsoleVariable<int>& GetCVarCutSharing();
extern TAutoConsoleVariable<int>& GetCVarUseApproximateCosineBound();