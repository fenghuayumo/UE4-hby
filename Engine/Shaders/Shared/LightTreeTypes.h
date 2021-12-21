#pragma once

#define MAX_CUT_NODES 32
//#define LIGHT_CONE
#ifdef __cplusplus

struct FLightNode
{
	FVector	BoundMin;
	float	Intensity;
	FVector	BoundMax;
	int32   ID;
#ifdef LIGHT_CONE
	FVector4 Cone;
#endif
};

struct FVizLightNode
{
	FVector	BoundMin;
	int32	Level;
	FVector BoundMax;
	int32	Index;
};

#else
struct FLightNode
{
	float3	BoundMin;
	float	Intensity;
	float3	BoundMax;
	int   ID;
#ifdef LIGHT_CONE
	float4 Cone;
#endif
};

struct FVizLightNode
{
	float3	BoundMin;
	int	Level;
	float3  BoundMax;
	int	Index;
};

#endif