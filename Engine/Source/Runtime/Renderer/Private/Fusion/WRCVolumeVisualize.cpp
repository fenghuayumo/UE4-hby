
#include "WRCVolumeComponent.h"
#include "WRCVolume.h"
#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "SystemTextures.h"

// UE4 private interfaces
#include "PostProcess/SceneRenderTargets.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

DECLARE_GPU_STAT_NAMED(WRC_Visualizations, TEXT("WRC Visualizations"));


BEGIN_SHADER_PARAMETER_STRUCT(FWRCVolumeVisualizeParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbeOffsets)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ProbeStates)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbeRadiances)
	SHADER_PARAMETER_SAMPLER(SamplerState, ProbeSampler)

	SHADER_PARAMETER(FVector,       VolumeProbeOrigin)
	SHADER_PARAMETER(FVector4,      VolumeProbeRotation)
	SHADER_PARAMETER(int,           NumRaysPerProbe)
	SHADER_PARAMETER(FVector,       ProbeGridSpacing)
	SHADER_PARAMETER(FIntVector,    ProbeGridCounts)
	SHADER_PARAMETER(uint32, 		ProbeDim)
	SHADER_PARAMETER(uint32,		AtlasProbeCount)

	SHADER_PARAMETER(float, ProbeRadius)

	SHADER_PARAMETER(FMatrix, WorldToClip)
	SHADER_PARAMETER(FVector, CameraPosition)
	SHADER_PARAMETER(float, PreExposure)
	SHADER_PARAMETER(int32, ShouldUsePreExposure)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FWRCVolumeVisualizeShaderVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWRCVolumeVisualizeShaderVS);
	SHADER_USE_PARAMETER_STRUCT(FWRCVolumeVisualizeShaderVS, FGlobalShader);

	using FParameters = FWRCVolumeVisualizeParameters;

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

class FWRCVolumeVisualizeShaderPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWRCVolumeVisualizeShaderPS);
	SHADER_USE_PARAMETER_STRUCT(FWRCVolumeVisualizeShaderPS, FGlobalShader);

	using FParameters = FWRCVolumeVisualizeParameters;

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

IMPLEMENT_GLOBAL_SHADER(FWRCVolumeVisualizeShaderVS, "/Engine/Private/WRC/VisualizeProbes.usf", "VisualizeProbesVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWRCVolumeVisualizeShaderPS, "/Engine/Private/WRC//VisualizeProbes.usf", "VisualizeProbesPS", SF_Pixel);

/**
* Probe sphere vertex buffer. Defines a sphere of unit size.
*/
template<int32 NumSphereSides, int32 NumSphereRings, typename VectorType>
class TWRCProbeSphereVertexBuffer : public FVertexBuffer
{
public:

	int32 GetNumRings() const
	{
		return NumSphereRings;
	}

	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{
		const int32 NumSides = NumSphereSides;
		const int32 NumRings = NumSphereRings;
		const int32 NumVerts = (NumSides + 1) * (NumRings + 1);

		const float RadiansPerRingSegment = PI / (float)NumRings;
		float Radius = 1;

		TArray<VectorType, TInlineAllocator<NumRings + 1> > ArcVerts;
		ArcVerts.Empty(NumRings + 1);
		// Calculate verts for one arc
		for (int32 i = 0; i < NumRings + 1; i++)
		{
			const float Angle = i * RadiansPerRingSegment;
			ArcVerts.Add(FVector(0.0f, FMath::Sin(Angle), FMath::Cos(Angle)));
		}

		TResourceArray<VectorType, VERTEXBUFFER_ALIGNMENT> Verts;
		Verts.Empty(NumVerts);
		// Then rotate this arc NumSides + 1 times.
		const FVector Center = FVector(0, 0, 0);
		for (int32 s = 0; s < NumSides + 1; s++)
		{
			FRotator ArcRotator(0, 360.f * ((float)s / NumSides), 0);
			FRotationMatrix ArcRot(ArcRotator);

			for (int32 v = 0; v < NumRings + 1; v++)
			{
				const int32 VIx = (NumRings + 1) * s + v;
				Verts.Add(Center + Radius * ArcRot.TransformPosition(ArcVerts[v]));
			}
		}

		NumSphereVerts = Verts.Num();
		uint32 Size = Verts.GetResourceDataSize();

		// Create vertex buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(&Verts);
		VertexBufferRHI = RHICreateVertexBuffer(Size, BUF_Static, CreateInfo);
	}

	int32 GetVertexCount() const { return NumSphereVerts; }

	/**
	* Calculates the world transform for a sphere.
	* @param OutTransform - The output world transform.
	* @param Sphere - The sphere to generate the transform for.
	* @param PreViewTranslation - The pre-view translation to apply to the transform.
	* @param bConservativelyBoundSphere - when true, the sphere that is drawn will contain all positions in the analytical sphere,
	*		 Otherwise the sphere vertices will lie on the analytical sphere and the positions on the faces will lie inside the sphere.
	*/
	void CalcTransform(FVector4& OutPosAndScale, const FSphere& Sphere, const FVector& PreViewTranslation, bool bConservativelyBoundSphere = true)
	{
		float Radius = Sphere.W;
		if (bConservativelyBoundSphere)
		{
			const int32 NumRings = NumSphereRings;
			const float RadiansPerRingSegment = PI / (float)NumRings;

			// Boost the effective radius so that the edges of the sphere approximation lie on the sphere, instead of the vertices
			Radius /= FMath::Cos(RadiansPerRingSegment);
		}

		const FVector Translate(Sphere.Center + PreViewTranslation);
		OutPosAndScale = FVector4(Translate, Radius);
	}

private:
	int32 NumSphereVerts;
};

/**
* Probe sphere index buffer.
*/
template<int32 NumSphereSides, int32 NumSphereRings>
class TWRCProbeSphereIndexBuffer : public FIndexBuffer
{
public:
	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{
		const int32 NumSides = NumSphereSides;
		const int32 NumRings = NumSphereRings;
		TResourceArray<uint16, INDEXBUFFER_ALIGNMENT> Indices;

		// Add triangles for all the vertices generated
		for (int32 s = 0; s < NumSides; s++)
		{
			const int32 a0start = (s + 0) * (NumRings + 1);
			const int32 a1start = (s + 1) * (NumRings + 1);

			for (int32 r = 0; r < NumRings; r++)
			{
				Indices.Add(a0start + r + 0);
				Indices.Add(a1start + r + 0);
				Indices.Add(a0start + r + 1);
				Indices.Add(a1start + r + 0);
				Indices.Add(a1start + r + 1);
				Indices.Add(a0start + r + 1);
			}
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

struct FVisualWRCProbesVertex
{
	FVector4 Position;
	FVisualWRCProbesVertex() {}
	FVisualWRCProbesVertex(const FVector4& InPosition) : Position(InPosition) {}
};

class FVisualizeWRCProbesVertexDeclaration : public FRenderResource
{
public:

	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual ~FVisualizeWRCProbesVertexDeclaration() {}

	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		uint16 Stride = sizeof(FVisualWRCProbesVertex);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FVisualWRCProbesVertex, Position), VET_Float4, 0, Stride));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVisualizeWRCProbesVertexDeclaration> GVisualizeProbesVertexDeclaration;
TGlobalResource<TWRCProbeSphereVertexBuffer<36, 24, FVector4>> GProbeSphereVertexBuffer;
TGlobalResource<TWRCProbeSphereIndexBuffer<36, 24>> GProbeSphereIndexBuffer;

void WRCProbeRenderDiffuseIndirectVisualizations(
	const FScene& Scene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());

	RDG_GPU_STAT_SCOPE(GraphBuilder, WRC_Visualizations);
	RDG_EVENT_SCOPE(GraphBuilder, "WRC Visualizations");

	// Get other things we'll need for all proxies
	FIntRect ViewRect = View.ViewRect;
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor());
	FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);

	for (FWRCVolumeSceneProxy* proxy : FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread)
	{
		// Skip if the volume visualization is not enabled
		if (!proxy->ComponentData.EnableProbeVisulization) continue;

		// Skip this volume if it isn't part of the current scene
		if (proxy->OwningScene != &Scene) continue;

		// Skip this volume if it is not enabled
		if (!proxy->ComponentData.EnableVolume) continue;

		// Skip this volume if it doesn't intersect the view frustum
		if (!proxy->IntersectsViewFrustum(View)) continue;

		// Get the shader permutation
		FWRCVolumeVisualizeShaderVS::FPermutationDomain PermutationVectorVS;

		FWRCVolumeVisualizeShaderPS::FPermutationDomain PermutationVectorPS;

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
		TShaderMapRef<FWRCVolumeVisualizeShaderVS> VertexShader(GlobalShaderMap, PermutationVectorVS);
		TShaderMapRef<FWRCVolumeVisualizeShaderPS> PixelShader(GlobalShaderMap, PermutationVectorPS);

		// Set shader pass parameters
		FWRCVolumeVisualizeParameters DefaultPassParameters;
		FWRCVolumeVisualizeParameters* PassParameters = GraphBuilder.AllocParameters<FWRCVolumeVisualizeParameters>();
		*PassParameters = DefaultPassParameters;

		PassParameters->ProbeRadiances = GraphBuilder.RegisterExternalTexture(proxy->ProbesRadiance);
	
		PassParameters->WorldToClip = View.ViewMatrices.GetViewProjectionMatrix();
		PassParameters->CameraPosition = View.ViewLocation;

		PassParameters->ShouldUsePreExposure = View.Family->EngineShowFlags.Tonemapper;
		PassParameters->PreExposure = View.PreExposure;

		PassParameters->ProbeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();


		FVector volumeSize = proxy->ComponentData.Transform.GetScale3D() * 200.0f;
		FVector probeGridSpacing;
		probeGridSpacing.X = volumeSize.X / float(proxy->ComponentData.ProbeCounts.X);
		probeGridSpacing.Y = volumeSize.Y / float(proxy->ComponentData.ProbeCounts.Y);
		probeGridSpacing.Z = volumeSize.Z / float(proxy->ComponentData.ProbeCounts.Z);


		PassParameters->VolumeProbeOrigin = proxy->ComponentData.Origin;
		FQuat rotation = proxy->ComponentData.Transform.GetRotation();
		PassParameters->VolumeProbeRotation = FVector4{ rotation.X, rotation.Y, rotation.Z, rotation.W };
		PassParameters->ProbeGridSpacing = probeGridSpacing;
		PassParameters->ProbeGridCounts = proxy->ComponentData.ProbeCounts;
		PassParameters->NumRaysPerProbe = proxy->ComponentData.GetNumRaysPerProbe();
		PassParameters->ProbeDim = proxy->ComponentData.GetProbeTexelDim();

		PassParameters->ProbeRadius = proxy->ComponentData.DebugProbeRadius;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		uint32 NumInstances = uint32(proxy->ComponentData.ProbeCounts.X * proxy->ComponentData.ProbeCounts.Y * proxy->ComponentData.ProbeCounts.Z);
		int AtlasProbeCount = 1 << FMath::CeilLogTwo(sqrt(NumInstances));
		PassParameters->AtlasProbeCount = AtlasProbeCount;

		GraphBuilder.AddPass(
			Forward<FRDGEventName>(RDG_EVENT_NAME("WRC Visualize Probes")),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, GlobalShaderMap, VertexShader, PixelShader, ViewRect, NumInstances](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
				GraphicsPSOInit.BlendState = TStaticBlendStateWriteMask<CW_RGB, CW_RGBA>::GetRHI();

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVisualizeProbesVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

				RHICmdList.SetStreamSource(0, GProbeSphereVertexBuffer.VertexBufferRHI, 0);
				RHICmdList.DrawIndexedPrimitive(GProbeSphereIndexBuffer.IndexBufferRHI, 0, 0, GProbeSphereVertexBuffer.GetVertexCount(), 0, GProbeSphereIndexBuffer.GetIndexCount() / 3, NumInstances);
			}
		);
	}
}
#else
void WRCProbeRenderDiffuseIndirectVisualizations(
	const FScene& Scene,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder){}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)


#endif  // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
