

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"



/**
* This resource stores a simple unit cube AABB BLAS ranging from (0,0,0) to (1,1,1).
*/
class FUnitCubeAABB : public FRenderResource
{
public:
	FUnitCubeAABB() {}
	virtual ~FUnitCubeAABB() {}

	// FRenderResource interface.
	virtual void InitRHI() override;

	virtual void ReleaseRHI() override;

	virtual FString GetFriendlyName() const override
	{
		return TEXT("FUnitCubeAABB");
	}

	FRayTracingGeometry RayTracingGeometry;
	

private:
	FVertexBufferRHIRef VertexData;
};
