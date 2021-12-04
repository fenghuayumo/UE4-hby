#pragma once
#include "CoreMinimal.h"

class FViewInfo;
class FRDGBuilder;
class UTestGIComponent;
class FScene;
class FRHIRayTracingShader;

namespace RayTracingDDIGIUpdate
{
	extern 	void PrepareRayTracingShaders(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders);
	void DDGIUpdatePerFrame_RenderThread(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder);
}
