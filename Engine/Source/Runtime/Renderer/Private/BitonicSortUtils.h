#pragma once

#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "Shader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterMacros.h"
#include "RHIGPUReadback.h"

// Bitonic Sort is a highly parallel sorting algorithm well-suited to the
// GPU.  It has a complexity of O( N*(log N)^2 ), which is inferior to most
// traditional sorting algorithms, but because GPUs have so many threads,
// and because each thread can be utilized, the algorithm can fully load
// the GPU, taking advantage of its high ALU and bandwidth capabilities.
// 
// Another reason why sorting on the GPU is useful is when the GPU is creating
// work on its own timeline and needs to sort the work without CPU intervention.
// One example in MiniEngine is with GPU simulated particle systems.  Before
// rendering the particles, it is beneficial to sort the particles and render
// them either front-to-back or back-to-front.
//
// The way a bitonic sort works is by iteratively sorting groups of increasing
// size and then blending the sorted groups together to form larger sorted
// groups.  The core of the algorithm can be expressed like so:
//
// for (k = 2; k < NumItems; k *= 2)      // k = group size
//     for (j = k / 2; j > 0; j /= 2)     // j = compare distance
//         for (i = 0; i < NumItems; ++i) // i = element index
//             if (ShouldSwap(i, i ^ j))  // Are the two in proper order?
//                 Swap(i, i ^ j)         // If not, swap them
//
// In this modified form of bitonic sort, all groups of size k are sorted in
// the same direction.  This facilitates sorting lists with non-power-of-two
// lengths.  So the ShouldSwap() test is informed only by the intent to have
// an ascending or descending list.  If sorting ascending, null items should
// have a sort key of 0xffffffff to guarantee sorting to the end.  Likewise,
// sorting descending has a null item of 0x00000000.
//
// The value of the null item is also useful in the key comparison.  Notice
// that with ascending lists, we want A < B, and with descending lists, we
// want A > B.  So if they are reversed, we must swap them.  By using the
// null item value, we can automatically reverse the test like so:
//
// Descending:  Swap if (A < B) == (A ^ 0x00000000 < B ^ 0x00000000)
// Ascending:   Swap if (A > B) == (~A < ~B) == (A ^ 0xffffffff < B ^ 0xffffffff)
// Generalized: Swap if (A ^ NullItem) < (B ^ NullItem)
//
// As an optimization, you can pre-sort the list for k values up to 2048 in
// LDS before writing to memory.  (You do not have to write null items at the
// end of your list to memory.)  It is always better for the caller of this
// system to pre-sort their list as they create it.
//
// The expected usage of this API is that you will have an array of data of
// unspecified stride.  You will generate a list of sort keys and array index
// pairs to pass to the system to be sorted.  The sorted key/index pairs can
// then be used to reorder your data array (with a double buffer) or you can
// read your data array in place using indirection.
//
// We also expect that consumers of this system have a GPU-visible count of
// items.  This probably came from an AppendBuffer (structured buffer with an
// atomic counter).  So you will need to provide the counter buffer as well as
// the offset to the counter.
//
// Also note that key/index pairs may be packed into 32-bit words with the key
// in the most significant bits.  Sorting 32-bit elements is faster than sorting
// 64-bit elements because it uses less bandwidth.

// The UE Implementation Reference To Microsoft MiniGameEngine, The Implement is not very good, use AMD ParallelSort Lib Might be faster sorter: GPUOpen-Effects/FidelityFX-ParallelSort: FidelityFX Parallel Sort 

struct FBitonicSortUtils
{
	static void	Sort(FRDGBuilder& GraphBuilder, 
		FRDGBufferRef& KeyIndexList,
		FRDGBufferRef& CounterBuffer,
		uint32_t CounterOffset,
		bool IsPartiallyPreSorted,
		bool SortAscending);
};