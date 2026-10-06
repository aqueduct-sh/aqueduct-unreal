// Compiled graph — pre-resolved flat dispatch list for zero-allocation replay.
//
// Mirrors the wgpu engine's CompiledGraph architecture:
//   compile_graph():  builds flat dispatch list from graph topology
//   replay():         walks dispatches, skips clean nodes, records GPU commands
//   set_param():      marks node dirty for next replay
//
// Expression evaluation is NOT done here. Expressions are evaluated in Rust
// (the single source of truth) via aqueduct_evaluate_time_expressions(), and
// the results are applied as Named param overrides before each replay.

#pragma once

#include "RHIResources.h"
#include "RHICommandList.h"

namespace UE::Aqueduct
{

// ---------------------------------------------------------------------------
// Parameter source — how a shader param gets its value each frame
// ---------------------------------------------------------------------------

enum class EParamSource : uint8
{
	Static,       // Constant value, never changes
	Named,        // Slider-controlled or expression-evaluated, changes via SetParam
	PassIndex,    // Current multi-pass iteration
	TotalPasses,  // Total iterations for multi-pass node
};

struct FCompiledParam
{
	EParamSource Source = EParamSource::Static;
	float Value = 0.0f;        // Current value (static or named)
	FString Name;              // Param name (for Named source)
	bool bIsInt = false;       // Int/enum params stored as float(int)
};

// ---------------------------------------------------------------------------
// Per-node metadata
// ---------------------------------------------------------------------------

struct FCompiledNode
{
	FString NodeId;
	FString NodeKind;
	TArray<FCompiledParam> Params;
	uint32 BufferSize = 0;     // Uniform buffer size (16-byte aligned)
};

// ---------------------------------------------------------------------------
// Single dispatch command (one per node per pass)
// ---------------------------------------------------------------------------

struct FCompiledDispatch
{
	// Shader
	int32 PermutationIndex = -1;
	uint32 PipelineId = 0;     // Sequential ID for batching set_pipeline calls

	// Bind state (pre-resolved, reused every frame)
	FShaderResourceViewRHIRef InputSRVs[16];
	FUnorderedAccessViewRHIRef OutputUAVs[4];
	FUnorderedAccessViewRHIRef ScratchUAVs[8];
	// Count-sized UAVs over the SHARED scratch slots, zero-filled at the
	// start of this node's execution (only populated on the PassIndex==0
	// dispatch of a multipass node). Shared slots make the placement strict:
	// the clear must run immediately before this node's first pass, never
	// batched up front.
	TArray<FUnorderedAccessViewRHIRef> ScratchUAVsToZero;
	bool bClearOutputs = false;
	FSamplerStateRHIRef Sampler;

	// Uniform data
	uint32 ParamDataOffset = 0;  // Offset into uniform scratch buffer
	uint32 ParamDataSize = 0;

	// Dispatch dimensions
	uint32 WorkgroupsX = 0;
	uint32 WorkgroupsY = 0;

	// Node tracking
	uint32 NodeIndex = 0;      // Index into nodes array
	uint32 PassIndex = 0;
	uint32 TotalPasses = 1;

	// Output textures to transition after dispatch
	TArray<FTextureRHIRef> OutputTextures;
};

// In-place mip regeneration for producer outputs required by registry inputs.
struct FCompiledMipChain
{
	FTextureRHIRef OutputTexture;
	uint32 Resolution = 0;
	uint32 SourceNodeIndex = 0;
};

/** One semantic output conversion for a multi-pass node. */
struct FCompiledFormatConvert
{
	bool bClampOutput = true;
	FTextureRHIRef SrcTexture;   // primary ping/pong or secondary staging (rgba32float)
	FTextureRHIRef DstTexture;   // semantic's actual output texture
	uint32 Resolution = 0;
	uint32 NodeIndex = 0;
};

// ---------------------------------------------------------------------------
// The compiled graph — flat dispatch list with dirty tracking
// ---------------------------------------------------------------------------

struct FCompiledGraph
{
	// Nodes and dispatches
	TArray<FCompiledNode> Nodes;
	TArray<FCompiledDispatch> Dispatches;
	TArray<FCompiledMipChain> MipChains;
	TArray<FCompiledFormatConvert> FormatConverts;

	// Node ID → index lookup
	TMap<FString, uint32> NodeIdToIndex;

	// Downstream dependencies (flattened)
	TArray<uint32> DownstreamFlat;
	TArray<uint32> DownstreamOffsets;  // Offsets[i]..Offsets[i+1] = deps of node i

	// Dispatch ranges per node
	TArray<TPair<uint32, uint32>> NodeDispatchRanges;  // [start, end) in Dispatches

	// Time-dependent node tracking (indices of nodes with expression params)
	TArray<uint32> TimeDepNodes;
	TArray<bool> IsTimeDep;           // O(1) lookup parallel to Nodes

	// Dirty flags (parallel to Nodes)
	TArray<bool> NodeDirty;
	TArray<bool> NodeUniformDirty;
	TArray<uint32> PendingDirtyNodes;
	bool bHasPendingDirty = false;

	// Scratch buffer for uniform data (reused every frame)
	TArray<uint8> UniformScratch;

	// Resolution
	uint32 Resolution = 0;

	// ---------------------------------------------------------------------------
	// Methods
	// ---------------------------------------------------------------------------

	/** Mark a node dirty (param changed via slider). */
	void MarkNodeDirty(const FString& NodeId)
	{
		if (const uint32* Idx = NodeIdToIndex.Find(NodeId))
		{
			PendingDirtyNodes.Add(*Idx);
			bHasPendingDirty = true;
		}
	}

	/** Update a named parameter value. */
	bool SetParam(const FString& NodeId, const FString& ParamName, float Value)
	{
		if (const uint32* NodeIdx = NodeIdToIndex.Find(NodeId))
		{
			FCompiledNode& Node = Nodes[*NodeIdx];
			for (FCompiledParam& P : Node.Params)
			{
				if ((P.Source == EParamSource::Named || P.Source == EParamSource::Static)
					&& P.Name == ParamName)
				{
					P.Value = Value;
					MarkNodeDirty(NodeId);
					return true;
				}
			}
		}
		return false;
	}

	/** Write uniform data for a node into the scratch buffer. Returns slice. */
	TArrayView<const uint8> WriteUniformData(
		uint32 NodeIndex, uint32 PassIndex, uint32 TotalPasses)
	{
		const FCompiledNode& Node = Nodes[NodeIndex];
		if (Node.BufferSize == 0)
		{
			return {};
		}

		UniformScratch.SetNumZeroed(Node.BufferSize);

		for (int32 i = 0; i < Node.Params.Num(); i++)
		{
			const FCompiledParam& P = Node.Params[i];
			float Val = P.Value;

			switch (P.Source)
			{
			case EParamSource::Static:
			case EParamSource::Named:
				// Use P.Value as-is (set at compile time or by SetParam/EvaluateExpressions)
				break;
			case EParamSource::PassIndex:
				Val = (float)PassIndex;
				break;
			case EParamSource::TotalPasses:
				Val = (float)TotalPasses;
				break;
			}

			if (P.bIsInt)
			{
				int32 IntVal = (int32)Val;
				FMemory::Memcpy(&UniformScratch[i * 4], &IntVal, 4);
			}
			else
			{
				FMemory::Memcpy(&UniformScratch[i * 4], &Val, 4);
			}
		}

		return TArrayView<const uint8>(UniformScratch.GetData(), Node.BufferSize);
	}
};

} // namespace UE::Aqueduct
