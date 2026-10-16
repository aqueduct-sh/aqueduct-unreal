#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Maximum simultaneous node shaders (one permutation per node in a graph)
#define AQUEDUCT_MAX_NODE_SHADERS 4096

namespace UE::Aqueduct
{

// Shared constants matching the shader parameter struct layout
static constexpr int32 MAX_PARAMS = 64;   // 16 float4 = 64 scalars
static constexpr int32 WORKGROUP_SIZE = 16;
// Fixed AQ_* FParameters ABI widths (AQ_Input0..15, AQ_Output0..3,
// AQ_Scratch0..7): every generated shader binds against these slots.
static constexpr int32 MAX_NODE_INPUTS = 16;
static constexpr int32 MAX_NODE_OUTPUTS = 4;
static constexpr int32 MAX_SCRATCH_SLOTS = 8;
static constexpr int32 MAX_PARAM_VECTORS = MAX_PARAMS / 4; // AQ_Params float4 count

// ---------------------------------------------------------------------------
// Static storage for generated HLSL, keyed by permutation index.
// Populated on the game thread before triggering shader compilation.
// ---------------------------------------------------------------------------

struct AQUEDUCTRUNTIME_API FAqueductNodeShaderStorage
{
	/** Everything tracked per permutation index, in one record (one map, not
	 *  several parallel ones — so an index's state can't get half-updated). */
	struct FPermSlot
	{
		FString HLSL;          // generated source ModifyCompilationEnvironment injects
		uint32  Hash = 0;      // content hash (for dedup + reverse lookup on evict)
		uint64  LastUsed = 0;  // monotonic tick → LRU eviction order
		int32   RefCount = 0;  // # live executors using it; only 0 is recyclable
	};

	/** Permutation index → its slot. The authoritative per-index state. */
	static TMap<int32, FPermSlot> Slots;

	static FCriticalSection Lock;
	static int32 NextIndex;

	/** HLSL content hash → permutation index. Enables dedup across nodes/reloads. */
	static TMap<uint32, int32> HLSLHashToPermIndex;

	/** Per-executor set of indices it currently uses, for refcount diffing. */
	static TMap<const void*, TSet<int32>> OwnerIndexSets;

	/** An executor reports the permutation indices its current NodeShaders
	 *  reference; the storage adjusts refcounts by diffing against the
	 *  executor's previous set. Call after every (re)compile. */
	static void SetOwnerIndices(const void* Owner, const TSet<int32>& Indices);

	/** Release all of an executor's index references (on destroy/reset). */
	static void ReleaseOwner(const void* Owner);

	/**
	 * Register HLSL, reusing an existing permutation if content is identical.
	 * When the index pool is full, RECYCLES the least-recently-used index:
	 * evicts its compiled shader from the global shader map and repoints the
	 * index at the new HLSL — the caller's VerifyGlobalShaders then recompiles
	 * it (the compile cache is content-addressed, so this is a real recompile,
	 * never a stale shader). EDITOR-ONLY: the cook registers a finite set up
	 * front and never recycles. Returns INDEX_NONE only if nothing is
	 * evictable (every slot in use by the current compile pass — impossible
	 * for any single graph, whose node count is far below the cap).
	 */
	static int32 RegisterOrReuse(const FString& HLSL);

	/** Effective permutation cap (the permutation-domain maximum the shader
	 *  type was registered with). */
	static int32 EffectiveCap();

	/** Monotonic counter bumped whenever NEW HLSL is assigned to an index —
	 *  a fresh allocation OR a recycle. Callers snapshot it around a compile
	 *  pass to decide whether VerifyGlobalShaders is needed: recycling reuses
	 *  an index without growing NextIndex, so index growth is NOT a valid
	 *  "needs compile" signal; this is. */
	static int32 RegistrationEpoch();

	/** Full reset — only call on editor shutdown. Clears all state. */
	static void Reset();

	/** Lightweight clear for graph reloads — preserves compiled permutations
	 *  (Slots) and the dedup cache. */
	static void ClearForReload();
};

// ---------------------------------------------------------------------------
// Dynamic compute shader — one permutation per node.
//
// Uses SHADER_USE_PARAMETER_STRUCT with generic slot names.
// Slang HLSL is post-processed to rename variables to these slots.
// ---------------------------------------------------------------------------

class AQUEDUCTRUNTIME_API FAqueductDynamicCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductDynamicCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductDynamicCS, FGlobalShader);

	// Generic parameter struct — superset of all possible node resources.
	// Unused slots are skipped by SetShaderParameters automatically.
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Output textures (up to 4 for nodes like curvature, rgba_split)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, AQ_Output0)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, AQ_Output1)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, AQ_Output2)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, AQ_Output3)
		// Input textures (up to 16 for nodes like scatter)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input0)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input1)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input2)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input3)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input4)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input5)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input6)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input7)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input8)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input9)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input10)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input11)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input12)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input13)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input14)
		SHADER_PARAMETER_SRV(Texture2D, AQ_Input15)
		// Shared linear sampler for all inputs
		SHADER_PARAMETER_SAMPLER(SamplerState, AQ_Sampler)
		// Scalar parameters packed as float4 array (std140, 64 floats max)
		SHADER_PARAMETER_ARRAY(FVector4f, AQ_Params, [16])
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve0)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve1)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve2)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve3)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve4)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve5)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve6)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, AQ_Curve7)

		// GPU-internal scratch storage buffers. Each slot has typed aliases so
		// Slang-generated RWStructuredBuffer<float4>/uint4/etc. declarations can
		// compile without forcing every node through uint scratch.
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch0_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch0_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch0_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch0_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch0_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch1_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch1_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch1_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch1_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch1_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch2_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch2_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch2_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch2_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch2_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch3_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch3_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch3_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch3_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch3_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch4_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch4_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch4_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch4_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch4_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch5_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch5_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch5_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch5_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch5_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch6_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch6_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch6_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch6_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch6_Float4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, AQ_Scratch7_Uint)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<int>, AQ_Scratch7_Int)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, AQ_Scratch7_Float)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint4>, AQ_Scratch7_Uint4)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, AQ_Scratch7_Float4)
	END_SHADER_PARAMETER_STRUCT()

	// Permutation dimension — each node in the graph gets a unique index
	class FNodeIndex : SHADER_PERMUTATION_INT("AQUEDUCT_NODE_INDEX", AQUEDUCT_MAX_NODE_SHADERS);
	using FPermutationDomain = TShaderPermutationDomain<FNodeIndex>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
		{
			return false;
		}
		FPermutationDomain PD(Parameters.PermutationId);
		int32 NodeIndex = PD.Get<FNodeIndex>();
		FScopeLock ScopeLock(&FAqueductNodeShaderStorage::Lock);
		return FAqueductNodeShaderStorage::Slots.Contains(NodeIndex);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		FPermutationDomain PD(Parameters.PermutationId);
		int32 NodeIndex = PD.Get<FNodeIndex>();

		FScopeLock ScopeLock(&FAqueductNodeShaderStorage::Lock);
		if (const FAqueductNodeShaderStorage::FPermSlot* Slot = FAqueductNodeShaderStorage::Slots.Find(NodeIndex))
		{
			OutEnvironment.IncludeVirtualPathToContentsMap.Add(
				TEXT("/Plugin/Aqueduct/Generated/NodeBody.ush"),
				Slot->HLSL);
			// Include content hash in the compilation environment so the
			// shader cache key changes when the HLSL content changes.
			OutEnvironment.SetDefine(TEXT("AQUEDUCT_CONTENT_HASH"), Slot->Hash);
		}
	}
};

} // namespace UE::Aqueduct
