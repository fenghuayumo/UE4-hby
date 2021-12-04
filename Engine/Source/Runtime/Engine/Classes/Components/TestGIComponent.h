#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptInterface.h"
#include "Components/SceneComponent.h"
#include "Engine/BlendableInterface.h"
#include "Engine/Scene.h"
#include "Components/ShapeComponent.h"
#include "TestGIComponent.generated.h"


enum class ETestGIIrradianceBits : uint8;
enum class ETestGIDistanceBits : uint8;

// This needs to match the shader code in ProbeBlendingCS.usf
UENUM()
enum class ETestGIRaysPerProbe
{
	n144 = 144 UMETA(DisplayName = "144"),
	n288 = 288 UMETA(DisplayName = "288"),
	n432 = 432 UMETA(DisplayName = "432"),
	n576 = 576 UMETA(DisplayName = "576"),
	n720 = 720 UMETA(DisplayName = "720"),
	n864 = 864 UMETA(DisplayName = "864"),
	n1008 = 1008 UMETA(DisplayName = "1008")
};

UENUM()
enum class ETestGISkyLightType
{
	None UMETA(DisplayName = "None"),
	Raster UMETA(DisplayName = "Raster"),
	RayTracing UMETA(DisplayName = "Ray Tracing")
};

struct FTestGITexturePixels
{
	struct
	{
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Stride = 0;
		uint32 PixelFormat = 0;
	} Desc;
	TArray<uint8> Pixels;
	FTexture2DRHIRef Texture;
};

struct FTestGITextureLoadContext
{
	bool ReadyForLoad = false;
	FTestGITexturePixels Irradiance;
	FTestGITexturePixels Distance;
	FTestGITexturePixels Offsets;
	FTestGITexturePixels States;

	void Clear()
	{
		*this = FTestGITextureLoadContext();
	}
};

USTRUCT(BlueprintType)
struct FTestGIProbeRelocation
{
	GENERATED_USTRUCT_BODY()

		// If true, probes will attempt to relocate within their cell to leave geometry.
		UPROPERTY(EditAnywhere, Category = "GI Probes");
	bool AutomaticProbeRelocation = true;

	// Probe relocation moves probes that see front facing triangles closer than this value.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "0"));
	float ProbeMinFrontfaceDistance = 10.0f;

	// Probe relocation and state classifier assume probes with more than this ratio of backface hits are inside of geometry.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "0", ClampMax = "1"));
	float ProbeBackfaceThreshold = 0.25f;
};

class FTestGIVolumeSceneProxy
{
public:

	/** Initialization constructor. */
	FTestGIVolumeSceneProxy(FSceneInterface* InOwningScene)
	: OwningScene(InOwningScene)
	{

	}


	virtual ~FTestGIVolumeSceneProxy()
	{
	}
	virtual bool IntersectsViewFrustum(const FViewMatrices& ViewMatrix,const FVector& ViewDirction, const FConvexVolume& ViewFrustum,float FarClippingPlaneDistance) const;
	void ReallocateSurfaces_RenderThread(FRHICommandListImmediate& RHICmdList, ETestGIIrradianceBits IrradianceBits, ETestGIDistanceBits DistanceBits);
	void ResetTextures_RenderThread(FRDGBuilder& GraphBuilder);

	// data from the component
	struct FComponentData
	{
		// A shared location cpp side for operational defines
		static const bool c_RTXGI_DDGI_PROBE_CLASSIFICATION = true;

		// It considers this many volumes that pass frustum culling when sampling GI for the scene.
		static const int c_RTXGI_DDGI_MAX_SHADING_VOLUMES = 12;

		static const EPixelFormat c_pixelFormatRadianceLowBitDepth = EPixelFormat::PF_G32R32F;
		static const EPixelFormat c_pixelFormatRadianceHighBitDepth = EPixelFormat::PF_A32B32G32R32F;
		static const EPixelFormat c_pixelFormatIrradianceLowBitDepth = EPixelFormat::PF_A2B10G10R10;
		static const EPixelFormat c_pixelFormatIrradianceHighBitDepth = EPixelFormat::PF_A32B32G32R32F;
		static const EPixelFormat c_pixelFormatDistanceHighBitDepth = EPixelFormat::PF_G32R32F;
		static const EPixelFormat c_pixelFormatDistanceLowBitDepth = EPixelFormat::PF_G16R16F;
		static const EPixelFormat c_pixelFormatOffsets = EPixelFormat::PF_A16B16G16R16;
		static const EPixelFormat c_pixelFormatStates = EPixelFormat::PF_R8_UINT;

		static const EPixelFormat c_pixelFormatScrollSpace = EPixelFormat::PF_R8_UINT;


		// ProbeBlendingCS (.hlsl in SDK, .usf in plugin) needs this as a define so is a hard coded constant right now.
		// We need that shader to not require that as a define. Then, we can make it a tuneable parameter on the volume.
		// There should be a task on the SDK about this.
		static const uint32 c_NumTexelsIrradiance = 6;
		static const uint32 c_NumTexelsDistance = 14;

		uint32 GetNumRaysPerProbe() const
		{
			switch (RaysPerProbe)
			{
			case ETestGIRaysPerProbe::n144: return 144;
			case ETestGIRaysPerProbe::n288: return 288;
			case ETestGIRaysPerProbe::n432: return 432;
			case ETestGIRaysPerProbe::n576: return 576;
			case ETestGIRaysPerProbe::n720: return 720;
			case ETestGIRaysPerProbe::n864: return 864;
			case ETestGIRaysPerProbe::n1008: return 1008;
			}
			check(false);
			return 144;
		}

		ETestGIRaysPerProbe RaysPerProbe = ETestGIRaysPerProbe::n144;
		float ProbeMaxRayDistance = 1000.0f;
		FTransform Transform = FTransform::Identity;
		FVector Origin = FVector(0);
		FLightingChannels LightingChannels;
		FIntVector ProbeCounts = FIntVector(0); // 0 = invalid, will be written with valid counts before use
		float ProbeDistanceExponent = 1.0f;
		float ProbeIrradianceEncodingGamma = 1.0f;
		int   LightingPriority = 0;
		float UpdatePriority = 1.0f;
		float ProbeHysteresis = 0.0f;
		float ProbeChangeThreshold = 0.0f;
		float ProbeBrightnessThreshold = 0.0f;
		float NormalBias = 0.0f;
		float ViewBias = 0.0f;
		float BlendDistance = 0.0f;
		float BlendDistanceBlack = 0.0f;
		float ProbeBackfaceThreshold = 0.0f;
		float ProbeMinFrontfaceDistance = 0.0f;
		bool EnableProbeRelocation = false;
		bool EnableProbeScrolling = false;
		bool EnableProbeVisulization = false;
		bool EnableVolume = true;
		FIntVector ProbeScrollOffsets = FIntVector{ 0, 0, 0 };
		float IrradianceScalar = 1.0f;
		float EmissiveMultiplier = 1.0f;
		float LightingMultiplier = 1.0f;
		bool RuntimeStatic = false; // If true, does not update during gameplay, only during editor.
		ETestGISkyLightType SkyLightTypeOnRayMiss = ETestGISkyLightType::Raster;
		FMatrix		RayRotationTransform ;
		// This is GetDDGIVolumeProbeCounts() from the SDK
		FIntPoint Get2DProbeCount() const
		{
			return FIntPoint(
				ProbeCounts.Y * ProbeCounts.Z,
				ProbeCounts.X
			);
		}

		int GetProbeCount() const
		{
			return ProbeCounts.X * ProbeCounts.Y * ProbeCounts.Z;
		}
	};
	FComponentData ComponentData;
	FTestGITextureLoadContext TextureLoadContext;
	TRefCountPtr<IPooledRenderTarget> ProbesIrradiance;
	TRefCountPtr<IPooledRenderTarget> ProbesDistance;
	TRefCountPtr<IPooledRenderTarget> ProbesOffsets;
	TRefCountPtr<IPooledRenderTarget> ProbesStates;
	TRefCountPtr<IPooledRenderTarget> ProbesSpace;
	FRDGTextureRef					ProbesRadianceTex;
	FRDGTextureUAVRef				ProbesRadianceUAV;

	// Where to start the probe update from, for updating a subset of probes
	int ProbeIndexStart = 0;
	int ProbeIndexCount = 0;
	static TSet<FTestGIVolumeSceneProxy*> AllProxiesReadyForRender_RenderThread;
	static TMap<const FSceneInterface*, float> SceneRoundRobinValue;
	// Only render volumes in the scenes they are present in
	FSceneInterface* OwningScene;
};


UCLASS(HideCategories = (Tags, AssetUserData, Collision, Cooking, Transform, Rendering, Mobility, LOD))
class UTestGIComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

protected:
	void InitializeComponent() override final;

	//~ Begin UActorComponent Interface
	virtual bool ShouldCreateRenderState() const override { return true; }
	virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	virtual void SendRenderTransform_Concurrent() override;
	virtual void DestroyRenderState_Concurrent() override;
	//~ Begin UActorComponent Interface
	
	//~ Begin UObject Interface
	virtual void Serialize(FArchive& Ar) override;
	//~ End UObject Interface

public:

	virtual FTestGIVolumeSceneProxy* CreateTestGIProxy() const;
public:
	void UpdateRenderThreadData();
	void EnableVolumeComponent(bool enabled);

#if WITH_EDITOR
	virtual bool CanEditChange(const FProperty* InProperty) const override;
#endif // WITH_EDITOR

public:
	// --- "GI Volume" properties

	// If true, the volume will be a candidate to be updated and render indirect light into the scene (if also in the view frustum).
	UPROPERTY(EditAnywhere, Category = "GI Volume");
	bool EnableVolume = true;

	// A priority value for scheduling updates to this volume's probes. Volumes with higher priority values get updated more often. Weighted round robin updating.
	UPROPERTY(EditAnywhere, Category = "GI Volume", meta = (ClampMin = "0.0001", ClampMax = "100.0"));
	float UpdatePriority = 1.0f;

	// A priority value used to select volumes when applying lighting. The volume with the lowest priority value is selected.
	// If volumes have the same priority, then volumes are selected based on probe density. The highest density volume is selected.
	UPROPERTY(EditAnywhere, Category = "GI Volume", meta = (ClampMin = "0", ClampMax = "10"));
	int32 LightingPriority = 0;

	// The distance in world units that this volume blends to a volume it overlaps, or fades out.
	UPROPERTY(EditAnywhere, Category = "GI Volume");
	float BlendingDistance = 20.0f;

	// The distance from the edge of a volume at which it has zero weighting (turns black or yields to an encompassing volume). Useful if you don't want a linear fade all the way to the edge, which can be useful for scrolling volumes, hiding probes that haven't converged yet.
	// Volume Blend Distance begins at this distance from the edge.
	UPROPERTY(EditAnywhere, Category = "GI Volume");
	float BlendingCutoffDistance = 0.0f;

	// If true, the volume will not update at runtime, and will keep the lighting values seen when the level is saved.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GI Volume");
	bool RuntimeStatic = false;

	UPROPERTY(VisibleDefaultsOnly, AdvancedDisplay, Category = "GI Volume");
	FVector LastOrigin = FVector{ 0.0f, 0.0f, 0.0f };

	// --- "GI Probes" properties

	// Number of rays shot for each probe when updating probe data.
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	ETestGIRaysPerProbe RaysPerProbe = ETestGIRaysPerProbe::n288;

	// Number of probes on each axis.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "1"));
	FIntVector ProbeCounts = FIntVector(8, 8, 8);

	// Maximum distance a probe ray may travel. Shortening this can increase performance. If you shorten it too much, it can miss geometry.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "0"));
	float ProbeMaxRayDistance = 100000.0f;

	// Controls the influence of new rays when updating each probe. Values towards 1 will keep history longer, while values towards 0 will be more responsive to current values.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "0", ClampMax = "1"));
	float ProbeHistoryWeight = 0.97f;

	// Probes relocation.
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	FTestGIProbeRelocation  ProbeRelocation;

	// If true, probes will keep their same position in world space as the volume moves around. Useful for moving volumes to have more temporally stable probes.
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	bool ScrollProbesInfinitely = false;

	// Toggle probes visualization, Probes visualization modes can be changed from Project Settings
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	bool VisualizeProbes = false;

	UPROPERTY(VisibleDefaultsOnly, AdvancedDisplay, Category = "GI Probes");
	FIntVector ProbeScrollOffset = FIntVector{ 0, 0, 0 };

	// Exponent for depth testing. A high value will rapidly react to depth discontinuities, but risks causing banding.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GI Probes");
	float probeDistanceExponent = 50.f;

	// Irradiance blending happens in post-tonemap space
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GI Probes");
	float probeIrradianceEncodingGamma = 5.f;

	// A threshold ratio used during probe radiance blending that determines if a large lighting change has happened.
	// If the max color component difference is larger than this threshold, the hysteresis will be reduced.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GI Probes");
	float probeChangeThreshold = 0.2f;

	// A threshold value used during probe radiance blending that determines the maximum allowed difference in brightness
	// between the previous and current irradiance values. This prevents impulses from drastically changing a
	// texel's irradiance in a single update cycle.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GI Probes");
	float probeBrightnessThreshold = 2.0f;

	// --- "GI Lighting" properties

	// What type of skylight should contribute to GI
	UPROPERTY(EditAnywhere, Category = "GI Lighting");
	ETestGISkyLightType SkyLightTypeOnRayMiss = ETestGISkyLightType::Raster;

	// Bias values for Indirect Lighting
	UPROPERTY(EditAnywhere, Category = "GI Lighting", meta = (ClampMin = "0"));
	float ViewBias = 40.0f;

	// Bias values for Indirect Lighting
	UPROPERTY(EditAnywhere, Category = "GI Lighting", meta = (ClampMin = "0"));
	float NormalBias = 10.0f;

	// If you want to artificially increase the amount of lighting given by this volume, you can modify this lighting multiplier to do so.
	UPROPERTY(EditAnywhere, Category = "GI Lighting", meta = (ClampMin = "0"));
	float LightMultiplier = 1.0f;

	// Use this to artificially modify how much emissive lighting contributes to GI
	UPROPERTY(EditAnywhere, Category = "GI Lighting", meta = (ClampMin = "0"));
	float EmissiveMultiplier = 1.0f;

	// Multiplier to compensate for irradiance clipping that might happen in 10-bit mode (use smaller values for higher irradiance).
	// 32 - bit irradiance textures can be set from project settings to avoid clipping but will have higher memory cost and slower to update.
	UPROPERTY(EditAnywhere, DisplayName = "10-Bit Irradiance Scalar", Category = "GI Lighting", meta = (ClampMin = "0.001", ClampMax = "1"));
	float IrradianceScalar = 1.0f;

	// Objects with overlapping channel flags will receive lighting from this volume
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "GI Lighting")
		FLightingChannels LightingChannels;

	// Blueprint Nodes
	//UFUNCTION(BlueprintCallable, Category = "TestGI")
	//	void ClearProbeData();

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		void ToggleVolume(bool IsVolumeEnabled);

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		float GetIrradianceScalar() const;

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		void SetIrradianceScalar(float NewIrradianceScalar);

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		float GetEmissiveMultiplier() const;

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		void SetEmissiveMultiplier(float NewEmissiveMultiplier);

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		float GetLightMultiplier() const;

	UFUNCTION(BlueprintCallable, Category = "TestGI")
		void SetLightMultiplier(float NewLightMultiplier);

	FTestGIVolumeSceneProxy* SceneProxy;

	// When loading a volume we get data for it's textures but don't have a scene proxy yet.
	// This is where that data is stored until the scene proxy is ready to take it.
	FTestGITextureLoadContext LoadContext;
};
