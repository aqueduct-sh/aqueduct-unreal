#include "AqueductDynamicShader.h"
#include "GlobalShader.h"
#include "RHIDefinitions.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

namespace UE::Aqueduct
{

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

TMap<int32, FAqueductNodeShaderStorage::FPermSlot> FAqueductNodeShaderStorage::Slots;
FCriticalSection FAqueductNodeShaderStorage::Lock;
int32 FAqueductNodeShaderStorage::NextIndex = 0;
TMap<uint32, int32> FAqueductNodeShaderStorage::HLSLHashToPermIndex;
TMap<const void*, TSet<int32>> FAqueductNodeShaderStorage::OwnerIndexSets;

static uint64 GAqueductUseTick = 0;
static int32 GAqueductRegistrationEpoch = 0;

int32 FAqueductNodeShaderStorage::RegistrationEpoch()
{
	FScopeLock ScopeLock(&Lock);
	return GAqueductRegistrationEpoch;
}

int32 FAqueductNodeShaderStorage::EffectiveCap()
{
	return (int32)AQUEDUCT_MAX_NODE_SHADERS;
}

// Evict the least-recently-used UNREFERENCED permutation so its index can be
// reused. Caller holds Lock. Returns the freed index, or INDEX_NONE if every
// slot is still referenced by a loaded graph (genuinely can't recycle).
// EDITOR-ONLY (the cook never recycles).
static int32 EvictLeastRecentlyUsed_Locked()
{
	int32 Victim = INDEX_NONE;
	uint64 Oldest = MAX_uint64;
	for (const TPair<int32, FAqueductNodeShaderStorage::FPermSlot>& Pair : FAqueductNodeShaderStorage::Slots)
	{
		// Only recycle indices no live executor references — evicting a
		// shared, still-live shader would corrupt another graph's render.
		if (Pair.Value.RefCount > 0) continue;
		if (Pair.Value.LastUsed < Oldest)
		{
			Oldest = Pair.Value.LastUsed;
			Victim = Pair.Key;
		}
	}
	if (Victim == INDEX_NONE)
	{
		return INDEX_NONE;
	}

#if WITH_EDITOR
	// Drop the compiled shader for this index from the global shader map. The
	// caller's VerifyGlobalShaders then recompiles it from the new HLSL — the
	// shader compile cache is keyed on preprocessed source, so this is a true
	// recompile, never a stale-shader reuse. Game thread (we are).
	if (FGlobalShaderMap* GSM = GetGlobalShaderMap(GMaxRHIFeatureLevel))
	{
		GSM->RemoveShaderTypePermutaion(&FAqueductDynamicCS::GetStaticType(), Victim);
	}
#endif

	if (const FAqueductNodeShaderStorage::FPermSlot* Slot = FAqueductNodeShaderStorage::Slots.Find(Victim))
	{
		FAqueductNodeShaderStorage::HLSLHashToPermIndex.Remove(Slot->Hash);
	}
	FAqueductNodeShaderStorage::Slots.Remove(Victim);
	return Victim;
}

void FAqueductNodeShaderStorage::SetOwnerIndices(const void* Owner, const TSet<int32>& Indices)
{
	FScopeLock ScopeLock(&Lock);
	TSet<int32>& Prev = OwnerIndexSets.FindOrAdd(Owner);
	// New references this owner gained.
	for (int32 Idx : Indices)
	{
		if (FPermSlot* Slot = Slots.Find(Idx))
		{
			if (!Prev.Contains(Idx)) Slot->RefCount++;
			Slot->LastUsed = ++GAqueductUseTick; // live → most-recently-used
		}
	}
	// References this owner dropped.
	for (int32 Idx : Prev)
	{
		if (!Indices.Contains(Idx))
		{
			if (FPermSlot* Slot = Slots.Find(Idx))
			{
				Slot->RefCount = FMath::Max(0, Slot->RefCount - 1);
			}
		}
	}
	Prev = Indices;
}

void FAqueductNodeShaderStorage::ReleaseOwner(const void* Owner)
{
	FScopeLock ScopeLock(&Lock);
	if (TSet<int32>* Prev = OwnerIndexSets.Find(Owner))
	{
		for (int32 Idx : *Prev)
		{
			if (FPermSlot* Slot = Slots.Find(Idx))
			{
				Slot->RefCount = FMath::Max(0, Slot->RefCount - 1);
			}
		}
		OwnerIndexSets.Remove(Owner);
	}
}

// Allocate an index for fresh HLSL: a new slot while the pool has room,
// otherwise recycle the LRU slot. Caller holds Lock.
static int32 AllocateIndex_Locked()
{
	if (FAqueductNodeShaderStorage::NextIndex < FAqueductNodeShaderStorage::EffectiveCap())
	{
		return FAqueductNodeShaderStorage::NextIndex++;
	}
	int32 Recycled = EvictLeastRecentlyUsed_Locked();
	if (Recycled == INDEX_NONE)
	{
		// Every slot is in use by the current compile pass — only possible if
		// one graph alone exceeds the cap (node count >> 4096), which no real
		// graph does. Per-graph compile failure, never a crash.
		UE_LOG(LogTemp, Error,
			TEXT("Aqueduct node-shader permutations exhausted (cap %d) with no ")
			TEXT("evictable slot — graph exceeds the permutation pool."),
			FAqueductNodeShaderStorage::EffectiveCap());
	}
	return Recycled;
}

int32 FAqueductNodeShaderStorage::RegisterOrReuse(const FString& HLSL)
{
	FScopeLock ScopeLock(&Lock);
	uint32 Hash = GetTypeHash(HLSL);
	if (int32* Existing = HLSLHashToPermIndex.Find(Hash))
	{
		// Verify not a hash collision
		if (FPermSlot* Slot = Slots.Find(*Existing))
		{
			if (Slot->HLSL == HLSL)
			{
				Slot->LastUsed = ++GAqueductUseTick; // touch (MRU)
				return *Existing;
			}
		}
	}
	// New content — allocate a fresh permutation (recycling the LRU if full).
	int32 Index = AllocateIndex_Locked();
	if (Index == INDEX_NONE) return INDEX_NONE;
	Slots.Add(Index, FPermSlot{ HLSL, Hash, ++GAqueductUseTick, 0 });
	HLSLHashToPermIndex.Add(Hash, Index);
	++GAqueductRegistrationEpoch;  // new content → caller must (re)compile
	return Index;
}

void FAqueductNodeShaderStorage::Reset()
{
	FScopeLock ScopeLock(&Lock);
	Slots.Empty();
	HLSLHashToPermIndex.Empty();
	OwnerIndexSets.Empty();
	// Do NOT reset NextIndex — UE caches compiled global shaders by permutation
	// index for the lifetime of the process. Reusing indices would dispatch
	// stale shaders from a previous graph.
}

void FAqueductNodeShaderStorage::ClearForReload()
{
	// Intentionally empty — preserve compiled permutations + dedup cache across
	// reloads. Shaders compiled in previous loads remain valid and reusable.
}

// ---------------------------------------------------------------------------
// Shader registration
// ---------------------------------------------------------------------------

IMPLEMENT_GLOBAL_SHADER(FAqueductDynamicCS,
	"/Plugin/Aqueduct/Private/AqueductNode.usf",
	"computeMain",
	SF_Compute);

} // namespace UE::Aqueduct
