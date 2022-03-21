#include "WRCVolumeComponent.h"
#include "WRCVolume.h"

// UE4 Public Interfaces
#include "ConvexVolume.h"
#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "SystemTextures.h"

// UE4 Private Interfaces
#include "PostProcess/SceneRenderTargets.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"

#include "RenderGraphUtils.h"


TSet<FWRCVolumeSceneProxy*> FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread;

UWRCVolumeComponent::UWRCVolumeComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bWantsInitializeComponent = true;
}

void UWRCVolumeComponent::InitializeComponent()
{
	Super::InitializeComponent();

	MarkRenderDynamicDataDirty();

	TransformUpdated.AddLambda(
		[this](USceneComponent* /*UpdatedComponent*/, EUpdateTransformFlags /*UpdateTransformFlags*/, ETeleportType /*Teleport*/)
		{
			MarkRenderDynamicDataDirty();
		}
	);
}

bool FWRCVolumeSceneProxy::IntersectsViewFrustum(const FViewInfo& View)
{
	// Get the volume position and scale
	FVector ProxyPosition = ComponentData.Origin;
	FQuat   ProxyRotation = ComponentData.Transform.GetRotation();
	FVector ProxyScale = ComponentData.Transform.GetScale3D();
	FVector ProxyExtent = ProxyScale * 100.0f;

	if (ProxyRotation.IsIdentity())
	{
		// This volume is not rotated, test it against the view frustum
		// Skip this volume if it doesn't intersect the view frustum
		return View.ViewFrustum.IntersectBox(ProxyPosition, ProxyExtent);
	}
	else
	{
		// TODO: optimize CPU performance for many volumes (100s to 1000s)

		// This volume is rotated, transform the view frustum so the volume's
		// oriented bounding box becomes an axis-aligned bounding box.
		FConvexVolume TransformedViewFrustum;
		FMatrix FrustumTransform = FTranslationMatrix::Make(-ProxyPosition)
			* FRotationMatrix::Make(ProxyRotation)
			* FTranslationMatrix::Make(ProxyPosition);

		// Based on SetupViewFrustum()
		if (View.SceneViewInitOptions.OverrideFarClippingPlaneDistance > 0.0f)
		{
			FVector PlaneBasePoint = FrustumTransform.TransformPosition(View.ViewMatrices.GetViewOrigin() + View.GetViewDirection() * View.SceneViewInitOptions.OverrideFarClippingPlaneDistance);
			FVector PlaneNormal = FrustumTransform.TransformVector(View.GetViewDirection());

			const FPlane FarPlane(PlaneBasePoint, PlaneNormal);
			// Derive the view frustum from the view projection matrix, overriding the far plane
			GetViewFrustumBounds(TransformedViewFrustum, FrustumTransform * View.ViewMatrices.GetViewProjectionMatrix(), FarPlane, true, false);
		}
		else
		{
			// Derive the view frustum from the view projection matrix.
			GetViewFrustumBounds(TransformedViewFrustum, FrustumTransform * View.ViewMatrices.GetViewProjectionMatrix(), false);
		}

		// Test the transformed view frustum against the volume
		// Skip this volume if it doesn't intersect the view frustum
		return TransformedViewFrustum.IntersectBox(ProxyPosition, ProxyExtent);
	}
}

void FWRCVolumeSceneProxy::ReallocateSurfaces_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	FIntPoint ProxyDims = ComponentData.Get2DProbeCount();
	int ProbeCount = ProxyDims.X * ProxyDims.Y;
	int AtlasProbeCount = 1 << FMath::CeilLogTwo(sqrt(ProbeCount));
	// Radiance
	{
		int TexelDim = ComponentData.GetProbeTexelDim();
		FIntPoint ProxyTexDims = FIntPoint(AtlasProbeCount * TexelDim, AtlasProbeCount * TexelDim);
		EPixelFormat Format = PF_FloatRGBA;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyTexDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV , false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesRadiance, TEXT("WRCRadiance"), ERenderTargetTransience::NonTransient);
	}

}

void FWRCVolumeSceneProxy::ResetTextures_RenderThread(FRDGBuilder& GraphBuilder)
{
	float ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(ProbesRadiance)), ClearColor);
}


void UWRCVolumeComponent::UpdateRenderThreadData()
{
	// Send command to the rendering thread to update the transform and other parameters
	if (SceneProxy)
	{
		// Update the volume component's data
		FWRCVolumeSceneProxy::FComponentData ComponentData;
		ComponentData.ProbeRayDim = ProbeRayDim;
		ComponentData.ProbeMaxRayDistance = ProbeMaxRayDistance;
		ComponentData.ProbeCounts = ProbeCounts;
		ComponentData.EnableProbeVisulization = VisualizeProbes;
		ComponentData.EnableVolume = EnableVolume;
		ComponentData.DebugProbeRadius = DebugProbeRadius;
		ComponentData.ProbeHistoryWeight = ProbeHistoryWeight;
		{
			// Finite moveable volume
			// Transform the volume to stay aligned with its parent.
			// Useful for spaces that move, e.g. a ship or train car.
			ComponentData.Transform = GetOwner()->GetTransform();
			ComponentData.Origin = LastOrigin = GetOwner()->GetTransform().GetLocation();
		}

		// If the ProbeCounts are too large to make textures, let's not update the render thread data to avoid a crash.
		// Everything is ok with not getting an update, ever, so this is safe.
		{
			volatile uint32 maxTextureSize = GetMax2DTextureDimension();

			// WRCRadiance
			int ProbeCount = ProbeCounts.X * ProbeCounts.Y * ProbeCounts.Z;
			int AtlasProbeCount = 1 << FMath::CeilLogTwo(sqrt(ProbeCount));

			if (uint32(ComponentData.GetProbeTexelDim() * AtlasProbeCount) > maxTextureSize)
				return;
		}

		FWRCVolumeSceneProxy* WRCProxy = SceneProxy;

		ENQUEUE_RENDER_COMMAND(UpdatVolumeTransformCommand)(
			[WRCProxy, ComponentData](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				bool needReallocate =
					WRCProxy->ComponentData.ProbeCounts != ComponentData.ProbeCounts ||
					WRCProxy->ComponentData.ProbeRayDim != ComponentData.ProbeRayDim;

				// set the data
				WRCProxy->ComponentData = ComponentData;

				if (needReallocate)
				{
					WRCProxy->ReallocateSurfaces_RenderThread(RHICmdList);
					WRCProxy->ResetTextures_RenderThread(GraphBuilder);
					FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread.Add(WRCProxy);
				}

				GraphBuilder.Execute();
			}
		);
	}
}

void UWRCVolumeComponent::EnableVolumeComponent(bool enabled)
{
	EnableVolume = enabled;
	MarkRenderDynamicDataDirty();
}

bool UWRCVolumeComponent::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	return ProcessConsoleExec(Cmd, Ar, NULL);
}

#if WITH_EDITOR
bool UWRCVolumeComponent::CanEditChange(const FProperty* InProperty) const
{
	if (InProperty)
	{
	}

	return Super::CanEditChange(InProperty);
}
#endif

void UWRCVolumeComponent::ClearVolumes()
{
	ENQUEUE_RENDER_COMMAND(ClearVolumesCommand)(
		[](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			for (FWRCVolumeSceneProxy* Proxy : FWRCVolumeSceneProxy::AllProxiesReadyForRender_RenderThread)
			{
				Proxy->ResetTextures_RenderThread(GraphBuilder);
			}

			GraphBuilder.Execute();
		}
	);
}

void UWRCVolumeComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
	UpdateRenderThreadData();
}

void UWRCVolumeComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
	Super::CreateRenderState_Concurrent(Context);
	check(SceneProxy == nullptr);

#if WITH_EDITOR
	if (!GetOwner()->IsTemporarilyHiddenInEditor())
#endif
	{
		SceneProxy = new FWRCVolumeSceneProxy(GetScene());
		UpdateRenderThreadData();
	}
}

void UWRCVolumeComponent::DestroyRenderState_Concurrent()
{
	Super::DestroyRenderState_Concurrent();

	if (SceneProxy)
	{
		FWRCVolumeSceneProxy* Proxy = SceneProxy;
		ENQUEUE_RENDER_COMMAND(DeleteProxy)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				delete Proxy;
			}
		);

		// wait for the above command to finish, so we know we got the load context if present
		FlushRenderingCommands();

		SceneProxy = nullptr;
	}
}

void UWRCVolumeComponent::ClearProbeData()
{
	FWRCVolumeSceneProxy* Proxy = SceneProxy;

	ENQUEUE_RENDER_COMMAND(ClearProbeData)(
		[Proxy](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			Proxy->ResetTextures_RenderThread(GraphBuilder);

			GraphBuilder.Execute();
		}
	);
}
