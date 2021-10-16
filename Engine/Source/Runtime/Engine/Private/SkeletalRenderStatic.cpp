// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SkeletalRenderStatic.cpp: CPU skinned skeletal mesh rendering code.
=============================================================================*/

#include "SkeletalRenderStatic.h"
#include "EngineStats.h"
#include "Components/SkeletalMeshComponent.h"
#include "SceneManagement.h"
#include "SkeletalRender.h"
#include "Rendering/SkeletalMeshRenderData.h"

FSkeletalMeshObjectStatic::FSkeletalMeshObjectStatic(USkinnedMeshComponent* InMeshComponent, FSkeletalMeshRenderData* InSkelMeshRenderData, ERHIFeatureLevel::Type InFeatureLevel)
	: FSkeletalMeshObject(InMeshComponent, InSkelMeshRenderData, InFeatureLevel)
{
	// create LODs to match the base mesh
	for (int32 LODIndex = 0; LODIndex < InSkelMeshRenderData->LODRenderData.Num(); LODIndex++)
	{
		new(LODs) FSkeletalMeshObjectLOD(InFeatureLevel, InSkelMeshRenderData, LODIndex);
	}

	InitResources(InMeshComponent);
}

FSkeletalMeshObjectStatic::~FSkeletalMeshObjectStatic()
{
}

void FSkeletalMeshObjectStatic::InitResources(USkinnedMeshComponent* InMeshComponent)
{
	for( int32 LODIndex=0;LODIndex < LODs.Num();LODIndex++ )
	{
		FSkeletalMeshObjectLOD& SkelLOD = LODs[LODIndex];
		
		// Skip LODs that have their render data stripped
		if (SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].GetNumVertices() > 0)
		{
			FSkelMeshComponentLODInfo* CompLODInfo = nullptr;
			if (InMeshComponent->LODInfo.IsValidIndex(LODIndex))
			{
				CompLODInfo = &InMeshComponent->LODInfo[LODIndex];
			}

			SkelLOD.InitResources(CompLODInfo);

#if RHI_RAYTRACING
			if (IsRayTracingEnabled() && SkelLOD.SkelMeshRenderData->bSupportRayTracing)
			{
				if (SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].NumReferencingStaticSkeletalMeshObjects == 0)
				{
					check(SkelLOD.SkelMeshRenderData);
					check(SkelLOD.SkelMeshRenderData->LODRenderData.IsValidIndex(LODIndex));

					FSkeletalMeshLODRenderData& LODModel = SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex];
					FVertexBufferRHIRef VertexBufferRHI = LODModel.StaticVertexBuffers.PositionVertexBuffer.VertexBufferRHI;
					FIndexBufferRHIRef IndexBufferRHI = LODModel.MultiSizeIndexContainer.GetIndexBuffer()->IndexBufferRHI;
					uint32 VertexBufferStride = LODModel.StaticVertexBuffers.PositionVertexBuffer.GetStride();

					uint32 TrianglesCount = 0;
					for (int32 SectionIndex = 0; SectionIndex < LODModel.RenderSections.Num(); SectionIndex++)
					{
						const FSkelMeshRenderSection& Section = LODModel.RenderSections[SectionIndex];
						TrianglesCount += Section.NumTriangles;
					}

					TArray<FSkelMeshRenderSection>* RenderSections = &LODModel.RenderSections;
					ENQUEUE_RENDER_COMMAND(InitSkeletalRenderStaticRayTracingGeometry)(
						[this, VertexBufferRHI, IndexBufferRHI, VertexBufferStride, TrianglesCount, RenderSections, 
						LODIndex = LODIndex, 
						SkelMeshRenderData = SkelLOD.SkelMeshRenderData, 
						&RayTracingGeometry = SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].StaticRayTracingGeometry,
						&bReferencedByStaticSkeletalMeshObjects_RenderThread = SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].bReferencedByStaticSkeletalMeshObjects_RenderThread](FRHICommandListImmediate& RHICmdList)
						{
							FRayTracingGeometryInitializer Initializer;
							static const FName DebugName("FSkeletalMeshObjectLOD");
							static int32 DebugNumber = 0;
							Initializer.DebugName = FName(DebugName, DebugNumber++);
							Initializer.IndexBuffer = IndexBufferRHI;
							Initializer.TotalPrimitiveCount = TrianglesCount;
							Initializer.GeometryType = RTGT_Triangles;
							Initializer.bFastBuild = false;

							TArray<FRayTracingGeometrySegment> GeometrySections;
							GeometrySections.Reserve(RenderSections->Num());
							for (const FSkelMeshRenderSection& Section : *RenderSections)
							{
								FRayTracingGeometrySegment Segment;
								Segment.VertexBuffer = VertexBufferRHI;
								Segment.VertexBufferElementType = VET_Float3;
								Segment.VertexBufferOffset = 0;
								Segment.VertexBufferStride = VertexBufferStride;
								Segment.FirstPrimitive = Section.BaseIndex / 3;
								Segment.NumPrimitives = Section.NumTriangles;
								Segment.bEnabled = !Section.bDisabled;
								GeometrySections.Add(Segment);
							}
							Initializer.Segments = GeometrySections;

							RayTracingGeometry.SetInitializer(Initializer);

							if (LODIndex >= SkelMeshRenderData->CurrentFirstLODIdx) // According to GetMeshElementsConditionallySelectable(), non-resident LODs should just be skipped
							{
								RayTracingGeometry.InitResource();
							}

							bReferencedByStaticSkeletalMeshObjects_RenderThread = true;
						}
					);
				}

				SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].NumReferencingStaticSkeletalMeshObjects++;
			}
#endif
		}
	}
}

void FSkeletalMeshObjectStatic::ReleaseResources()
{
	for( int32 LODIndex=0;LODIndex < LODs.Num();LODIndex++ )
	{
		FSkeletalMeshObjectLOD& SkelLOD = LODs[LODIndex];
		
		// Skip LODs that have their render data stripped
		if (SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].GetNumVertices() > 0)
		{
#if RHI_RAYTRACING
			if (IsRayTracingEnabled())
			{
				if (SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].NumReferencingStaticSkeletalMeshObjects > 0)
				{
					SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].NumReferencingStaticSkeletalMeshObjects--;

					if (SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].NumReferencingStaticSkeletalMeshObjects == 0)
					{
						ENQUEUE_RENDER_COMMAND(ResetStaticRayTracingGeometryFlag)(
							[&bReferencedByStaticSkeletalMeshObjects_RenderThread = SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].bReferencedByStaticSkeletalMeshObjects_RenderThread](FRHICommandListImmediate& RHICmdList)
						{
							bReferencedByStaticSkeletalMeshObjects_RenderThread = false;
						}
						);

						BeginReleaseResource(&SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].StaticRayTracingGeometry);
					}
				}
			}
#endif

			SkelLOD.ReleaseResources();
		}
	}
}

const FVertexFactory* FSkeletalMeshObjectStatic::GetSkinVertexFactory(const FSceneView* View, int32 LODIndex, int32 ChunkIdx) const
{
	check(LODs.IsValidIndex(LODIndex));
	return &LODs[LODIndex].VertexFactory; 
}

TArray<FTransform>* FSkeletalMeshObjectStatic::GetComponentSpaceTransforms() const
{
	return nullptr;
}

const TArray<FMatrix>& FSkeletalMeshObjectStatic::GetReferenceToLocalMatrices() const
{
	static TArray<FMatrix> ReferenceToLocalMatrices;
	return ReferenceToLocalMatrices;
}

void FSkeletalMeshObjectStatic::FSkeletalMeshObjectLOD::InitResources(FSkelMeshComponentLODInfo* CompLODInfo)
{
	check(SkelMeshRenderData);
	check(SkelMeshRenderData->LODRenderData.IsValidIndex(LODIndex));

	FSkeletalMeshLODRenderData& LODData = SkelMeshRenderData->LODRenderData[LODIndex];
	
	FPositionVertexBuffer* PositionVertexBufferPtr = &LODData.StaticVertexBuffers.PositionVertexBuffer;
	FStaticMeshVertexBuffer* StaticMeshVertexBufferPtr = &LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
	
	// If we have a vertex color override buffer (and it's the right size) use it
	if (CompLODInfo &&
		CompLODInfo->OverrideVertexColors &&
		CompLODInfo->OverrideVertexColors->GetNumVertices() == PositionVertexBufferPtr->GetNumVertices())
	{
		ColorVertexBuffer = CompLODInfo->OverrideVertexColors;
	}
	else
	{
		ColorVertexBuffer = &LODData.StaticVertexBuffers.ColorVertexBuffer;
	}

	FLocalVertexFactory* VertexFactoryPtr = &VertexFactory;
	FColorVertexBuffer* ColorVertexBufferPtr = ColorVertexBuffer;

	ENQUEUE_RENDER_COMMAND(InitSkeletalMeshStaticSkinVertexFactory)(
		[VertexFactoryPtr, PositionVertexBufferPtr, StaticMeshVertexBufferPtr, ColorVertexBufferPtr](FRHICommandListImmediate& RHICmdList)
		{
			FLocalVertexFactory::FDataType Data;
			PositionVertexBufferPtr->InitResource();
			StaticMeshVertexBufferPtr->InitResource();
			ColorVertexBufferPtr->InitResource();

			PositionVertexBufferPtr->BindPositionVertexBuffer(VertexFactoryPtr, Data);
			StaticMeshVertexBufferPtr->BindTangentVertexBuffer(VertexFactoryPtr, Data);
			StaticMeshVertexBufferPtr->BindTexCoordVertexBuffer(VertexFactoryPtr, Data);
			ColorVertexBufferPtr->BindColorVertexBuffer(VertexFactoryPtr, Data);

			VertexFactoryPtr->SetData(Data);
			VertexFactoryPtr->InitResource();
		});

	bResourcesInitialized = true;
}

/** 
 * Release rendering resources for this LOD 
 */
void FSkeletalMeshObjectStatic::FSkeletalMeshObjectLOD::ReleaseResources()
{	
	BeginReleaseResource(&VertexFactory);

#if RHI_RAYTRACING
	// BeginReleaseResource(&RayTracingGeometry);
	// Workaround for UE-106993:
	// Destroy ray tracing geometry on the render thread, as it may hold references to render resources.
	// These references should be cleared in FRayTracingGeometry::ReleaseResource(), however FRayTracingGeometry does
	// not implement this method and it can't be added due to 4.26 hotfix rules.
	ENQUEUE_RENDER_COMMAND(ReleaseRayTracingGeometry)([Ptr = &RayTracingGeometry](FRHICommandListImmediate&)
	{
		Ptr->ReleaseResource();
		*Ptr = FRayTracingGeometry(); // Explicitly reset all contents, including any resource references.
	});
#endif // RHI_RAYTRACING

	bResourcesInitialized = false;
}

