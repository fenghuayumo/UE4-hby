#pragma once
// UE4 public interfaces
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineTypes.h"
#include "Components/SceneComponent.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RendererInterface.h"

#include "WRCVolumeComponent.generated.h"


UENUM()
enum class WRCProbeDim
{
	n8x8 = 8 UMETA(DisplayName = "8"),
	n16x16 = 16 UMETA(DisplayName = "16"),
	n32x32 = 32 UMETA(DisplayName = "32"),
	n64x64 = 64 UMETA(DisplayName = "64"),
	n128x128 = 128 UMETA(DisplayName = "128"),
};

USTRUCT(BlueprintType)
struct FWRCProbeRelocation
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

class FWRCVolumeSceneProxy
{
public:

	/** Initialization constructor. */
	FWRCVolumeSceneProxy(FSceneInterface* InOwningScene)
		: OwningScene(InOwningScene)
	{
	}

	virtual ~FWRCVolumeSceneProxy()
	{
		check(IsInRenderingThread() || IsInParallelRenderingThread());
		AllProxiesReadyForRender_RenderThread.Remove(this);
	}

	bool IntersectsViewFrustum(const FViewInfo& View);

	void ReallocateSurfaces_RenderThread(FRHICommandListImmediate& RHICmdList);
	void ResetTextures_RenderThread(FRDGBuilder& GraphBuilder);

	// data from the component
	struct FComponentData
	{

		uint32 GetNumRaysPerProbe() const
		{
			switch (ProbeRayDim)
			{
				case WRCProbeDim::n8x8: return 64;
				case WRCProbeDim::n16x16: return 256;
				case WRCProbeDim::n32x32: return 1024;
				case WRCProbeDim::n64x64: return 4096;
				case WRCProbeDim::n128x128: return 16384;
			}
			return 256;
		}

		uint32 GetProbeTexelDim() const
		{
			switch (ProbeRayDim)
			{
				case WRCProbeDim::n8x8: return 8;
				case WRCProbeDim::n16x16: return 16;
				case WRCProbeDim::n32x32: return 32;
				case WRCProbeDim::n64x64: return 64;
				case WRCProbeDim::n128x128: return 128;
			}
			return 16;
		}
		WRCProbeDim ProbeRayDim = WRCProbeDim::n16x16;
		float ProbeMaxRayDistance = 1000.0f;
		FTransform Transform = FTransform::Identity;
		FVector Origin = FVector(0);
		FIntVector ProbeCounts = FIntVector(0); // 0 = invalid, will be written with valid counts before use
		bool EnableProbeVisulization = false;
		bool EnableVolume = true;
		float DebugProbeRadius = 5.0f;
		float ProbeHistoryWeight = 0.99f;
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


	TRefCountPtr<IPooledRenderTarget> ProbesRadiance;

	static TSet<FWRCVolumeSceneProxy*> AllProxiesReadyForRender_RenderThread;

	// Only render volumes in the scenes they are present in
	FSceneInterface* OwningScene;
};

UCLASS(HideCategories = (Tags, AssetUserData, Collision, Cooking, Transform, Rendering, Mobility, LOD))
class UWRCVolumeComponent : public USceneComponent, public FSelfRegisteringExec
{
	GENERATED_UCLASS_BODY()

protected:
	void InitializeComponent() override final;

	//~ Begin UActorComponent Interface
	virtual bool ShouldCreateRenderState() const override { return true; }
	virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	virtual void DestroyRenderState_Concurrent() override;
	virtual void SendRenderDynamicData_Concurrent() override;
	//~ Begin UActorComponent Interface

public:
	void UpdateRenderThreadData();
	void EnableVolumeComponent(bool enabled);

	void ClearProbeData();
#if WITH_EDITOR
	virtual bool CanEditChange(const FProperty* InProperty) const override;
#endif // WITH_EDITOR

	/**
	 * FExec interface
	 */
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override;

	// Clears the probe textures on all volumes
	UFUNCTION(exec)
	void ClearVolumes();

public:
	// --- "GI Volume" properties

	// If true, the volume will be a candidate to be updated and render indirect light into the scene (if also in the view frustum).
	UPROPERTY(EditAnywhere, Category = "GI Volume");
	bool EnableVolume = true;


	UPROPERTY(VisibleDefaultsOnly, AdvancedDisplay, Category = "GI Volume");
	FVector LastOrigin = FVector{ 0.0f, 0.0f, 0.0f };

	// --- "GI Probes" properties

	// Number of rays shot for each probe when updating probe data.
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	WRCProbeDim ProbeRayDim = WRCProbeDim::n16x16;

	// Number of probes on each axis.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "1"));
	FIntVector ProbeCounts = FIntVector(8, 8, 8);

	// Maximum distance a probe ray may travel. Shortening this can increase performance. If you shorten it too much, it can miss geometry.
	UPROPERTY(EditAnywhere, Category = "GI Probes", meta = (ClampMin = "0"));
	float ProbeMaxRayDistance = 100000.0f;

	// Toggle probes visualization, Probes visualization modes can be changed from Project Settings
	UPROPERTY(EditAnywhere, Category = "GI Probes");
	bool VisualizeProbes = false;

	/** The radius of the spheres that visualize the radiance probes */
	UPROPERTY(config, EditAnywhere, Category="GI Probes")
	float DebugProbeRadius = 5.0f;

	/** The history weight used to blend */
	UPROPERTY(config, EditAnywhere, Category="GI Probes")
	float ProbeHistoryWeight = 0.99f;

	FWRCVolumeSceneProxy* SceneProxy;

};

