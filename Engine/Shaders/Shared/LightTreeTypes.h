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

struct MeshLightInstanceTriangle
{
	int32 IndexOffset;
	int32 InstanceID;
};

struct MeshLightInstance
{
	FMatrix 	Transform;
	FVector 	Emission;
	float 		Padding0;
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

struct MeshLightInstanceTriangle
{
	int IndexOffset;
	int InstanceID;
};

struct MeshLightInstance
{
	float4x4 	Transform;
	float3 		Emission;
	float 		Padding0;
};

#endif