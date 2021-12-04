#include "TestGIComponent.h"
#include "Engine/TestGIVolume.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Materials/Material.h"
#include "UObject/RenderingObjectVersion.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "SceneManagement.h"
#include "ComponentReregisterContext.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/BillboardComponent.h"
#include "ComponentRecreateRenderStateContext.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"

UENUM()
enum class ETestGIIrradianceBits : uint8
{
	n10 UMETA(DisplayName = "10 bit"),
	n32 UMETA(DisplayName = "32 bit (for bright lighting and extended luminance range rendering)")
};

UENUM()
enum class ETestGIDistanceBits : uint8
{
	n16 UMETA(DisplayName = "16 bit"),
	n32 UMETA(DisplayName = "32 bit (for larger distances)")
};

TSet<FTestGIVolumeSceneProxy*> FTestGIVolumeSceneProxy::AllProxiesReadyForRender_RenderThread;

// Create a CPU accessible GPU texture and copy the provided GPU texture's contents to it
static FTestGITexturePixels GetTexturePixelsStep1(FRHICommandListImmediate& RHICmdList, FRHITexture* textureGPU)
{
	FTestGITexturePixels ret;

	// Early out if a GPU texture is not provided
	if (!textureGPU) return ret;

	ret.Desc.Width = textureGPU->GetTexture2D()->GetSizeX();
	ret.Desc.Height = textureGPU->GetTexture2D()->GetSizeY();
	ret.Desc.PixelFormat = (int32)textureGPU->GetFormat();

	// Create the texture
	FRHIResourceCreateInfo createInfo(TEXT("RTDDGIGetTexturePixelsSave"));
	ret.Texture = RHICreateTexture2D(
		textureGPU->GetTexture2D()->GetSizeX(),
		textureGPU->GetTexture2D()->GetSizeY(),
		textureGPU->GetFormat(),
		1,
		1,
		TexCreate_ShaderResource | TexCreate_Transient,
		ERHIAccess::CopyDest,
		createInfo);

	// Transition the GPU texture to a copy source
	RHICmdList.Transition(FRHITransitionInfo(textureGPU, ERHIAccess::SRVMask, ERHIAccess::CopySrc));

	// Schedule a copy of the GPU texture to the CPU accessible GPU texture
	RHICmdList.CopyTexture(textureGPU, ret.Texture, FRHICopyTextureInfo{});

	// Transition the GPU texture back to general
	RHICmdList.Transition(FRHITransitionInfo(textureGPU, ERHIAccess::CopySrc, ERHIAccess::SRVMask));

	return ret;
}

// Read the CPU accessible GPU texture data into CPU memory
static void GetTexturePixelsStep2(FRHICommandListImmediate& RHICmdList, FTestGITexturePixels& texturePixels)
{
	// Early out if no texture is provided
	if (!texturePixels.Texture) return;

	// Get a pointer to the CPU memory
	uint8* mappedTextureMemory = (uint8*)RHILockTexture2D(texturePixels.Texture, 0, RLM_ReadOnly, texturePixels.Desc.Stride, false);

	// Copy the texture data to CPU memory
	texturePixels.Pixels.AddZeroed(texturePixels.Desc.Height * texturePixels.Desc.Stride);
	FMemory::Memcpy(&texturePixels.Pixels[0], mappedTextureMemory, texturePixels.Desc.Height * texturePixels.Desc.Stride);

	RHIUnlockTexture2D(texturePixels.Texture, 0, false);
}

static void SaveFTestGITexturePixels(FArchive& Ar, FTestGITexturePixels& texturePixels, bool bSaveFormat)
{
	check(Ar.IsSaving());

	Ar << texturePixels.Desc.Width;
	Ar << texturePixels.Desc.Height;
	Ar << texturePixels.Desc.Stride;
	Ar << texturePixels.Pixels;

	if (bSaveFormat) Ar << texturePixels.Desc.PixelFormat;
}

static void LoadFTestGITexturePixels(FArchive& Ar, FTestGITexturePixels& texturePixels, EPixelFormat expectedPixelFormat, bool bLoadFormat)
{
	check(Ar.IsLoading());

	// Load the texture data
	Ar << texturePixels.Desc.Width;
	Ar << texturePixels.Desc.Height;
	Ar << texturePixels.Desc.Stride;
	Ar << texturePixels.Pixels;

	if (bLoadFormat)
	{
		Ar << texturePixels.Desc.PixelFormat;

		// Early out if the loaded pixel format doesn't match our expected format
		if (texturePixels.Desc.PixelFormat != expectedPixelFormat) return;
	}

	// Early out if no data was loaded
	if (texturePixels.Desc.Width == 0 || texturePixels.Desc.Height == 0 || texturePixels.Desc.Stride == 0) return;

	// Create the texture resource
	FRHIResourceCreateInfo createInfo(TEXT("RTDDGITextureLoad"));
	texturePixels.Texture = RHICreateTexture2D(
		texturePixels.Desc.Width,
		texturePixels.Desc.Height,
		expectedPixelFormat,
		1,
		1,
		TexCreate_ShaderResource | TexCreate_Transient,
		createInfo);

	// Copy the texture's data to the staging buffer
	ENQUEUE_RENDER_COMMAND(DDGILoadTex)(
		[&texturePixels](FRHICommandListImmediate& RHICmdList)
		{
			if (texturePixels.Pixels.Num() == texturePixels.Desc.Height * texturePixels.Desc.Stride)
			{
				uint32 destStride;
				uint8* mappedTextureMemory = (uint8*)RHILockTexture2D(texturePixels.Texture, 0, RLM_WriteOnly, destStride, false);
				if (texturePixels.Desc.Stride == destStride)
				{
					// Loaded data has the same stride as expected by the runtime
					// Copy the entire texture at once
					FMemory::Memcpy(mappedTextureMemory, &texturePixels.Pixels[0], texturePixels.Desc.Height * texturePixels.Desc.Stride);
				}
				else
				{
					// Loaded data has a different stride than expected by the runtime
					// Texture data was stored with a different API than what is running now (D3D12->VK, VK->D3D12)
					// Copy each row of the source data to the texture
					const uint8* SourceBuffer = &texturePixels.Pixels[0];
					for (uint32 Row = 0; Row < texturePixels.Desc.Height; ++Row)
					{
						FMemory::Memcpy(mappedTextureMemory, SourceBuffer, FMath::Min(texturePixels.Desc.Stride, destStride));

						mappedTextureMemory += destStride;
						SourceBuffer += texturePixels.Desc.Stride;
					}
				}
				RHIUnlockTexture2D(texturePixels.Texture, 0, false);
			}

			// Only clear the texels when in a game.
			// Cooking needs this data to write textures to disk on save, after load, when headless etc.
#if !WITH_EDITOR
			texturePixels.Pixels.Reset();
#endif
		}
	);
}

bool FTestGIVolumeSceneProxy::IntersectsViewFrustum(const FViewMatrices& ViewMatrix, const FVector& ViewDirection, const FConvexVolume& ViewFrustum, float FarClippingPlaneDistance) const
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
		return ViewFrustum.IntersectBox(ProxyPosition, ProxyExtent);
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
		if (FarClippingPlaneDistance > 0.0f)
		{
			FVector PlaneBasePoint = FrustumTransform.TransformPosition(ViewMatrix.GetViewOrigin() + ViewDirection * FarClippingPlaneDistance);
			FVector PlaneNormal = FrustumTransform.TransformVector(ViewDirection);

			const FPlane FarPlane(PlaneBasePoint, PlaneNormal);
			// Derive the view frustum from the view projection matrix, overriding the far plane
			GetViewFrustumBounds(TransformedViewFrustum, FrustumTransform * ViewMatrix.GetViewProjectionMatrix(), FarPlane, true, false);
		}
		else
		{
			// Derive the view frustum from the view projection matrix.
			GetViewFrustumBounds(TransformedViewFrustum, FrustumTransform * ViewMatrix.GetViewProjectionMatrix(), false);
		}

		// Test the transformed view frustum against the volume
		// Skip this volume if it doesn't intersect the view frustum
		return TransformedViewFrustum.IntersectBox(ProxyPosition, ProxyExtent);
	}
}

void FTestGIVolumeSceneProxy::ReallocateSurfaces_RenderThread(FRHICommandListImmediate& RHICmdList, ETestGIIrradianceBits IrradianceBits, ETestGIDistanceBits DistanceBits)
{
	FIntPoint ProxyDims = ComponentData.Get2DProbeCount();

	// Irradiance
	{
		int NumTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance;
		FIntPoint ProxyTexDims = ProxyDims * (NumTexels + 2);
		EPixelFormat Format = (IrradianceBits == ETestGIIrradianceBits::n32) ? FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatIrradianceHighBitDepth : FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatIrradianceLowBitDepth;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyTexDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesIrradiance, TEXT("RTDDGIIrradiance"), ERenderTargetTransience::NonTransient);
	}

	// Distance
	{
		int NumTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance;
		FIntPoint ProxyTexDims = ProxyDims * (NumTexels + 2);
		EPixelFormat Format = (DistanceBits == ETestGIDistanceBits::n32) ? FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatDistanceHighBitDepth : FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatDistanceLowBitDepth;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyTexDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesDistance, TEXT("RTDDGIDistance"), ERenderTargetTransience::NonTransient);
	}

	// Offsets - only pay the cost of this resource if this volume is actually doing relocation
	if (ComponentData.EnableProbeRelocation)
	{
		EPixelFormat Format = FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatOffsets;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesOffsets, TEXT("RTDDGIOffsets"), ERenderTargetTransience::NonTransient);
	}
	else
	{
		ProbesOffsets.SafeRelease();
	}

	// probe classifications
	if (FTestGIVolumeSceneProxy::FComponentData::c_RTXGI_DDGI_PROBE_CLASSIFICATION)
	{
		EPixelFormat Format = FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatStates;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesStates, TEXT("RTDDGIStates"), ERenderTargetTransience::NonTransient);
	}
	else
	{
		ProbesStates.SafeRelease();
	}

	if (ComponentData.EnableProbeScrolling)
	{
		EPixelFormat Format = FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatScrollSpace;

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ProxyDims, Format, FClearValueBinding::Transparent, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ProbesSpace, TEXT("RTDDGIScrollSpace"), ERenderTargetTransience::NonTransient);
	}
	else
	{
		ProbesSpace.SafeRelease();
	}
}

void FTestGIVolumeSceneProxy::ResetTextures_RenderThread(FRDGBuilder& GraphBuilder)
{
	float ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(ProbesIrradiance)), ClearColor);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(ProbesDistance)), ClearColor);

	if (ProbesOffsets)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(ProbesOffsets)), ClearColor);
	}

	if (ProbesStates)
	{
		uint32 StatesClearColor[] = { 0u, 0u, 0u, 0u };
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(ProbesStates)), StatesClearColor);
	}
}


UTestGIComponent::UTestGIComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

}

void UTestGIComponent::InitializeComponent()
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

// Serialization version for stored DDGIVolume data
struct  FRTDDGICustomVersion
{
	enum Type
	{
		AddingCustomVersion = 1,
		SaveLoadProbeTextures,     // save pixels and width/height
		SaveLoadProbeTexturesFmt,  // save texel format since the format can change in the project settings
		SaveLoadProbeDataIsOptional, // Probe data is optionally stored depending on project settings
	};

	// The GUID for this custom version number
	const static FGuid GUID;

private:
	FRTDDGICustomVersion() {}
};
const FGuid FRTDDGICustomVersion::GUID(0xc15f0537, 0x7546d9c5, 0x356fbba3, 0x758ab145);

// Register the custom version with core
FCustomVersionRegistration GRegCustomVersion(FRTDDGICustomVersion::GUID, FRTDDGICustomVersion::SaveLoadProbeDataIsOptional, TEXT("RTDDGIVolCompVer"));

void UTestGIComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
	Super::CreateRenderState_Concurrent(Context);
	check(SceneProxy == nullptr);

#if WITH_EDITOR
	if (!GetOwner()->IsTemporarilyHiddenInEditor())
#endif
	{
		SceneProxy = new FTestGIVolumeSceneProxy(GetScene());
		UpdateRenderThreadData();
	}

	//{
	//	UWorld* World = GetWorld();
	//	World->Scene->AddTestGIVolume(this);
	//}
}

void UTestGIComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
	UpdateRenderThreadData();
}

void UTestGIComponent::DestroyRenderState_Concurrent()
{
	Super::DestroyRenderState_Concurrent();

	if (SceneProxy)
	{
		FTestGITextureLoadContext& ComponentLoadContext = LoadContext;

		FTestGIVolumeSceneProxy* DDGIProxy = SceneProxy;
		ENQUEUE_RENDER_COMMAND(DeleteProxy)(
			[DDGIProxy, &ComponentLoadContext, this](FRHICommandListImmediate& RHICmdList)
			{
				// If the component has textures pending load, nothing to do here. Those are the most authoritative.
				if (!ComponentLoadContext.ReadyForLoad)
				{
					// If the proxy has textures pending load which haven't been serviced yet, the component should take those
					// in case it creates another proxy.
					if (DDGIProxy->TextureLoadContext.ReadyForLoad)
					{
						ComponentLoadContext = DDGIProxy->TextureLoadContext;
					}
					// otherwise, we should copy the textures from this proxy into textures for the TextureLoadContext
					// to make them survive to the next proxy for this component if one is created.
					else
					{
						ComponentLoadContext.ReadyForLoad = true;
						ComponentLoadContext.Irradiance = GetTexturePixelsStep1(RHICmdList, DDGIProxy->ProbesIrradiance->GetTargetableRHI());
						ComponentLoadContext.Distance = GetTexturePixelsStep1(RHICmdList, DDGIProxy->ProbesDistance->GetTargetableRHI());
						ComponentLoadContext.Offsets = GetTexturePixelsStep1(RHICmdList, DDGIProxy->ProbesOffsets ? DDGIProxy->ProbesOffsets->GetTargetableRHI() : nullptr);
						ComponentLoadContext.States = GetTexturePixelsStep1(RHICmdList, DDGIProxy->ProbesStates ? DDGIProxy->ProbesStates->GetTargetableRHI() : nullptr);
					}
				}
				UWorld* MyWorld = GetWorld();
				check(MyWorld != nullptr);
				if (ensure(MyWorld->Scene != nullptr))
				{
					MyWorld->Scene->RemoveTestGIVolumeSceneProxy_RenderThread(DDGIProxy);
				}
				delete DDGIProxy;
			}
		);

		// wait for the above command to finish, so we know we got the load context if present
		FlushRenderingCommands();

		SceneProxy = nullptr;
	}

	//Super::DestroyRenderState_Concurrent();
	//UWorld* MyWorld = GetWorld();
	//check(MyWorld != nullptr);
	//if (ensure(MyWorld->Scene != nullptr))
	//{
	//	MyWorld->Scene->RemoveTestGIVolume(this);
	//}
}

void UTestGIComponent::UpdateRenderThreadData()
{
	// Send command to the rendering thread to update the transform and other parameters
	if (SceneProxy)
	{
		// Update the volume component's data
		FTestGIVolumeSceneProxy::FComponentData ComponentData;
		ComponentData.RaysPerProbe = RaysPerProbe;
		ComponentData.ProbeMaxRayDistance = ProbeMaxRayDistance;
		ComponentData.LightingChannels = LightingChannels;
		ComponentData.ProbeCounts = ProbeCounts;
		ComponentData.ProbeDistanceExponent = probeDistanceExponent;
		ComponentData.ProbeIrradianceEncodingGamma = probeIrradianceEncodingGamma;
		ComponentData.LightingPriority = LightingPriority;
		ComponentData.UpdatePriority = UpdatePriority;
		ComponentData.ProbeHysteresis = ProbeHistoryWeight;
		ComponentData.ProbeChangeThreshold = probeChangeThreshold;
		ComponentData.ProbeBrightnessThreshold = probeBrightnessThreshold;
		ComponentData.NormalBias = NormalBias;
		ComponentData.ViewBias = ViewBias;
		ComponentData.BlendDistance = BlendingDistance;
		ComponentData.BlendDistanceBlack = BlendingCutoffDistance;
		ComponentData.ProbeBackfaceThreshold = ProbeRelocation.ProbeBackfaceThreshold;
		ComponentData.ProbeMinFrontfaceDistance = ProbeRelocation.ProbeMinFrontfaceDistance;
		ComponentData.EnableProbeRelocation = ProbeRelocation.AutomaticProbeRelocation;
		ComponentData.EnableProbeScrolling = ScrollProbesInfinitely;
		ComponentData.EnableProbeVisulization = VisualizeProbes;
		ComponentData.EnableVolume = EnableVolume;
		ComponentData.IrradianceScalar = IrradianceScalar;
		ComponentData.EmissiveMultiplier = EmissiveMultiplier;
		ComponentData.LightingMultiplier = LightMultiplier;
		ComponentData.RuntimeStatic = RuntimeStatic;
		ComponentData.SkyLightTypeOnRayMiss = SkyLightTypeOnRayMiss;

		if (ScrollProbesInfinitely)
		{
			// Infinite Scrolling Volume
			// Disable volume transformations and instead move the volume by "scrolling" the probes over an infinite space.
			// Offset "planes" of probes from one end of the volume to the other (in the direction  of movement).
			// Useful for computing GI around a moving object, e.g. characters.
			// NB: scrolling probes can be disruptive when recursive probe sampling is enabled and the volume is small. Sudden changes in scrolled probes will propogate to nearby probes!
			FVector CurrentOrigin = GetOwner()->GetTransform().GetLocation();
			FVector MovementDelta = CurrentOrigin - LastOrigin;

			FVector ProbeGridSpacing;
			FVector VolumeSize = GetOwner()->GetTransform().GetScale3D() * 200.f;
			ProbeGridSpacing.X = VolumeSize.X / float(ProbeCounts.X);
			ProbeGridSpacing.Y = VolumeSize.Y / float(ProbeCounts.Y);
			ProbeGridSpacing.Z = VolumeSize.Z / float(ProbeCounts.Z);

			if (FMath::Abs(MovementDelta.X) >= ProbeGridSpacing.X || FMath::Abs(MovementDelta.Y) >= ProbeGridSpacing.Y || FMath::Abs(MovementDelta.Z) >= ProbeGridSpacing.Z)
			{
				auto absFloor = [](float f)
				{
					return f >= 0.f ? int(floor(f)) : int(ceil(f));
				};

				// Calculate the number of grid cells that have been moved
				FIntVector Translation;
				Translation.X = int(absFloor(MovementDelta.X / ProbeGridSpacing.X));
				Translation.Y = int(absFloor(MovementDelta.Y / ProbeGridSpacing.Y));
				Translation.Z = int(absFloor(MovementDelta.Z / ProbeGridSpacing.Z));

				// Move the volume origin the number of grid cells * the distance between cells
				LastOrigin.X += float(Translation.X) * ProbeGridSpacing.X;
				LastOrigin.Y += float(Translation.Y) * ProbeGridSpacing.Y;
				LastOrigin.Z += float(Translation.Z) * ProbeGridSpacing.Z;

				// Update the probe scroll offset count
				ProbeScrollOffset.X += Translation.X;
				ProbeScrollOffset.Y += Translation.Y;
				ProbeScrollOffset.X += Translation.Z;
			}

			// Set the probe scroll offsets.
			// It is required that the offset will be positive and we need to be able to distinguish between various offsets
			// to reset probes when they are moved to the other side of the volume in Infinite Scrolling Volume case.
			// Therefore, instead of having negative offsets that we roll into positive ones, we center offset around INT_MAX / 2.
			int32 HalfIntMax = INT_MAX / 2;
			FIntVector RoundedHalfIntMax = FIntVector(
				(HalfIntMax / ProbeCounts.X) * ProbeCounts.X,
				(HalfIntMax / ProbeCounts.Y) * ProbeCounts.Y,
				(HalfIntMax / ProbeCounts.Z) * ProbeCounts.Z
			);
			FIntVector ProbeScrollOffsetPositive = ProbeScrollOffset + RoundedHalfIntMax;

			ComponentData.ProbeScrollOffsets.X = (ProbeScrollOffsetPositive.X % ProbeCounts.X) + (ProbeScrollOffsetPositive.X / ProbeCounts.X) * ProbeCounts.X;
			ComponentData.ProbeScrollOffsets.Y = (ProbeScrollOffsetPositive.Y % ProbeCounts.Y) + (ProbeScrollOffsetPositive.Y / ProbeCounts.Y) * ProbeCounts.Y;
			ComponentData.ProbeScrollOffsets.Z = (ProbeScrollOffsetPositive.Z % ProbeCounts.Z) + (ProbeScrollOffsetPositive.Z / ProbeCounts.Z) * ProbeCounts.Z;

			// Set the volume origin and scale (rotation not allowed)
			ComponentData.Origin = LastOrigin;
			ComponentData.Transform.SetScale3D(GetOwner()->GetTransform().GetScale3D());
		}
		else
		{
			// Finite moveable volume
			// Transform the volume to stay aligned with its parent.
			// Useful for spaces that move, e.g. a ship or train car.
			ComponentData.Transform = GetOwner()->GetTransform();
			ComponentData.Origin = LastOrigin = GetOwner()->GetTransform().GetLocation();
			ComponentData.ProbeScrollOffsets = FIntVector{ 0, 0, 0 };
		}

		// If the ProbeCounts are too large to make textures, let's not update the render thread data to avoid a crash.
		// Everything is ok with not getting an update, ever, so this is safe.
		{
			volatile uint32 maxTextureSize = GetMax2DTextureDimension();

			// DDGIRadiance
			if (uint32(ProbeCounts.X * ProbeCounts.Y * ProbeCounts.Z) > maxTextureSize)
				return;

			FIntPoint ProxyDims = ComponentData.Get2DProbeCount();

			// DDGIIrradiance
			{
				int numTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsIrradiance;
				FIntPoint ProxyTexDims = ProxyDims * (numTexels + 2);
				if (uint32(ProxyTexDims.X) > maxTextureSize || uint32(ProxyTexDims.Y) > maxTextureSize)
					return;
			}

			// DDGIDistance
			{
				int numTexels = FTestGIVolumeSceneProxy::FComponentData::c_NumTexelsDistance;
				FIntPoint ProxyTexDims = ProxyDims * (numTexels + 2);
				if (uint32(ProxyTexDims.X) > maxTextureSize || uint32(ProxyTexDims.Y) > maxTextureSize)
					return;
			}
		}

		FTestGIVolumeSceneProxy* DDGIProxy = SceneProxy;
		ETestGIIrradianceBits IrradianceBits =/* GetDefault<URTXGIPluginSettings>()->IrradianceBits*/ ETestGIIrradianceBits::n10;
		ETestGIDistanceBits DistanceBits = /*GetDefault<URTXGIPluginSettings>()->DistanceBits*/ETestGIDistanceBits::n16;

		FTestGITextureLoadContext TextureLoadContext = LoadContext;
		LoadContext.ReadyForLoad = false;

		ENQUEUE_RENDER_COMMAND(UpdateGIVolumeTransformCommand)(
			[DDGIProxy, ComponentData, TextureLoadContext, IrradianceBits, DistanceBits, this](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				bool needReallocate =
					DDGIProxy->ComponentData.ProbeCounts != ComponentData.ProbeCounts ||
					DDGIProxy->ComponentData.RaysPerProbe != ComponentData.RaysPerProbe ||
					DDGIProxy->ComponentData.EnableProbeRelocation != ComponentData.EnableProbeRelocation;

				// set the data
				DDGIProxy->ComponentData = ComponentData;

				// handle state textures ready to load from serialization
				if (TextureLoadContext.ReadyForLoad)
					DDGIProxy->TextureLoadContext = TextureLoadContext;

				if (needReallocate)
				{
					DDGIProxy->ReallocateSurfaces_RenderThread(RHICmdList, IrradianceBits, DistanceBits);
					DDGIProxy->ResetTextures_RenderThread(GraphBuilder);
					//FTestGIVolumeSceneProxy::AllProxiesReadyForRender_RenderThread.Add(DDGIProxy);
					UWorld* MyWorld = GetWorld();
					check(MyWorld != nullptr);
					if (ensure(MyWorld->Scene != nullptr))
					{
						MyWorld->Scene->AddTestGIVolumeSceneProxy_RenderThread(DDGIProxy);
					}
				}

				GraphBuilder.Execute();
			}
		);
	}
}

void UTestGIComponent::EnableVolumeComponent(bool enabled)
{
	EnableVolume = enabled;
	MarkRenderDynamicDataDirty();
}


void UTestGIComponent::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FRTDDGICustomVersion::GUID);
	if (Ar.CustomVer(FRTDDGICustomVersion::GUID) < FRTDDGICustomVersion::AddingCustomVersion)
	{
		if (Ar.IsLoading())
		{
			uint32 w, h;
			TArray<float> pixels;
			Ar << w;
			Ar << h;
			Ar << pixels;
		}
	}
	else if (Ar.CustomVer(FRTDDGICustomVersion::GUID) >= FRTDDGICustomVersion::SaveLoadProbeTextures)
	{
		// Save and load DDGIVolume texture resources when entering a level
		// Also applicable when ray tracing is not available (DX11 and Vulkan RHI).
		bool bSaveFormat = Ar.CustomVer(FRTDDGICustomVersion::GUID) >= FRTDDGICustomVersion::SaveLoadProbeTexturesFmt;

		FTestGIVolumeSceneProxy* proxy = SceneProxy;

		if (Ar.IsSaving())
		{
			// Probe data can be optionally not saved depending on project settings.
			bool bSeralizeProbesIsOptional = Ar.CustomVer(FRTDDGICustomVersion::GUID) >= FRTDDGICustomVersion::SaveLoadProbeDataIsOptional;
			bool bProbesSerialized = true;// bSeralizeProbesIsOptional ? GetDefault<URTXGIPluginSettings>()->SerializeProbes : true;
			if (bSeralizeProbesIsOptional)
				Ar << bProbesSerialized;

			if (bProbesSerialized)
			{
				FTestGITexturePixels Irradiance, Distance, Offsets, States;

				// When we are *not* cooking and ray tracing is available, copy the DDGIVolume probe texture resources
				// to CPU memory otherwise, write out the DDGIVolume texture resources acquired at load time
				if (!Ar.IsCooking() && IsRayTracingEnabled() && proxy)
				{
					// Copy textures to CPU accessible texture resources
					ENQUEUE_RENDER_COMMAND(DDGISaveTexStep1)(
						[&Irradiance, &Distance, &Offsets, &States, proxy](FRHICommandListImmediate& RHICmdList)
						{
							Irradiance = GetTexturePixelsStep1(RHICmdList, proxy->ProbesIrradiance->GetTargetableRHI());
							Distance = GetTexturePixelsStep1(RHICmdList, proxy->ProbesDistance->GetTargetableRHI());
							Offsets = GetTexturePixelsStep1(RHICmdList, proxy->ProbesOffsets ? proxy->ProbesOffsets->GetTargetableRHI() : nullptr);
							States = GetTexturePixelsStep1(RHICmdList, proxy->ProbesStates ? proxy->ProbesStates->GetTargetableRHI() : nullptr);
						}
					);
					FlushRenderingCommands();

					// Read the GPU texture data to CPU memory
					ENQUEUE_RENDER_COMMAND(DDGISaveTexStep2)(
						[&Irradiance, &Distance, &Offsets, &States](FRHICommandListImmediate& RHICmdList)
						{
							GetTexturePixelsStep2(RHICmdList, Irradiance);
							GetTexturePixelsStep2(RHICmdList, Distance);
							GetTexturePixelsStep2(RHICmdList, Offsets);
							GetTexturePixelsStep2(RHICmdList, States);
						}
					);
					FlushRenderingCommands();
				}
				else
				{
					Irradiance = LoadContext.Irradiance;
					Distance = LoadContext.Distance;
					Offsets = LoadContext.Offsets;
					States = LoadContext.States;
				}

				// Write the volume data
				SaveFTestGITexturePixels(Ar, Irradiance, bSaveFormat);
				SaveFTestGITexturePixels(Ar, Distance, bSaveFormat);
				SaveFTestGITexturePixels(Ar, Offsets, bSaveFormat);
				SaveFTestGITexturePixels(Ar, States, bSaveFormat);
			}
		}
		else if (Ar.IsLoading())
		{
			bool bSeralizeProbesIsOptional = Ar.CustomVer(FRTDDGICustomVersion::GUID) >= FRTDDGICustomVersion::SaveLoadProbeDataIsOptional;
			bool bProbesSerialized = true;
			if (bSeralizeProbesIsOptional)
				Ar << bProbesSerialized;

			if (bProbesSerialized)
			{
				ETestGIIrradianceBits IrradianceBits = ETestGIIrradianceBits::n10;
				ETestGIDistanceBits DistanceBits = ETestGIDistanceBits::n16;
				bool bLoadFormat = Ar.CustomVer(FRTDDGICustomVersion::GUID) >= FRTDDGICustomVersion::SaveLoadProbeTexturesFmt;

				// Read the volume texture data in and note that it's ready for load
				LoadFTestGITexturePixels(Ar, LoadContext.Irradiance, (IrradianceBits == ETestGIIrradianceBits::n32) ? FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatIrradianceHighBitDepth : FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatIrradianceLowBitDepth, bLoadFormat);
				LoadFTestGITexturePixels(Ar, LoadContext.Distance, (DistanceBits == ETestGIDistanceBits::n32) ? FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatDistanceHighBitDepth : FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatDistanceLowBitDepth, bLoadFormat);
				LoadFTestGITexturePixels(Ar, LoadContext.Offsets, FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatOffsets, bLoadFormat);
				LoadFTestGITexturePixels(Ar, LoadContext.States, FTestGIVolumeSceneProxy::FComponentData::c_pixelFormatStates, bLoadFormat);

				bool& ReadyForLoad = LoadContext.ReadyForLoad;
				ENQUEUE_RENDER_COMMAND(DDGILoadReady)(
					[&ReadyForLoad](FRHICommandListImmediate& RHICmdList)
					{
						ReadyForLoad = true;
					}
				);
			}
		}
	}
}

FTestGIVolumeSceneProxy* UTestGIComponent::CreateTestGIProxy() const
{
	return new FTestGIVolumeSceneProxy(GetScene());
}

bool UTestGIComponent::CanEditChange(const FProperty* InProperty) const
{
	if (InProperty)
	{
		const FString PropertyName = InProperty->GetName();
		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FTestGIProbeRelocation, ProbeMinFrontfaceDistance))
		{
			if (!ProbeRelocation.AutomaticProbeRelocation)
			{
				return false;
			}
		}
		else if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FTestGIProbeRelocation, ProbeBackfaceThreshold))
		{
			if (!ProbeRelocation.AutomaticProbeRelocation)
			{
				return false;
			}
		}
	}

	return Super::CanEditChange(InProperty);
}


void UTestGIComponent::ToggleVolume(bool IsVolumeEnabled)
{
	EnableVolumeComponent(IsVolumeEnabled);
}

float UTestGIComponent::GetIrradianceScalar() const
{
	return IrradianceScalar;
}

void UTestGIComponent::SetIrradianceScalar(float NewIrradianceScalar)
{
	IrradianceScalar = NewIrradianceScalar;
	MarkRenderDynamicDataDirty();
}

float UTestGIComponent::GetEmissiveMultiplier() const
{
	return EmissiveMultiplier;
}

void UTestGIComponent::SetEmissiveMultiplier(float NewEmissiveMultiplier)
{
	EmissiveMultiplier = NewEmissiveMultiplier;
	MarkRenderDynamicDataDirty();
}

float UTestGIComponent::GetLightMultiplier() const
{
	return LightMultiplier;
}

void UTestGIComponent::SetLightMultiplier(float NewLightMultiplier)
{
	LightMultiplier = NewLightMultiplier;
	MarkRenderDynamicDataDirty();
}
