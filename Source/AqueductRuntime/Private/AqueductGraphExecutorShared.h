// Internals shared by the AqueductGraphExecutor translation units.

#pragma once

#include "CoreMinimal.h"
#include "AqueductDynamicShader.h"
#include "RHIResources.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAqueduct, Log, All);

namespace UE::Aqueduct
{

// Multipass ping/pong textures are SHARED by all multipass nodes: node
// execution is strictly serial and the pair's contents are dead the moment a
// node's format-convert lands in its output texture. Keys keep the
// `.ping`/`.pong` suffix (the VRAM classifier keys off it). Mirrors the wgpu
// engine's SHARED_PING_KEY/SHARED_PONG_KEY.
extern const TCHAR* const GSharedPingKey;
extern const TCHAR* const GSharedPongKey;

/** Owning node of a NodeTextures key; empty for engine-internal keys. */
FString AqueductVramNodeOf(const FString& Key);

/** Classify a NodeTextures key into the category contract shared with the
 *  Rust engine (aqueduct/crates/engine/src/vram.rs). */
const TCHAR* ClassifyVramTextureKey(const FString& Key, FString& OutNodeId);

inline EPixelFormat AqueductFormatToPixelFormat(const FString& Format)
{
	if (Format == TEXT("r32float"))      return PF_R32_FLOAT;
	if (Format == TEXT("rgba8unorm"))    return PF_R8G8B8A8;
	if (Format == TEXT("rgba16float"))   return PF_FloatRGBA;
	if (Format == TEXT("rgba32float"))   return PF_A32B32G32R32F;
	return PF_FloatRGBA;
}

inline void BindCurveSlot(FAqueductDynamicCS::FParameters& Params, int32 Slot, const FShaderResourceViewRHIRef& SRV)
{
    switch (Slot) {
    case 0: Params.AQ_Curve0 = SRV; break;
    case 1: Params.AQ_Curve1 = SRV; break;
    case 2: Params.AQ_Curve2 = SRV; break;
    case 3: Params.AQ_Curve3 = SRV; break;
    case 4: Params.AQ_Curve4 = SRV; break;
    case 5: Params.AQ_Curve5 = SRV; break;
    case 6: Params.AQ_Curve6 = SRV; break;
    case 7: Params.AQ_Curve7 = SRV; break;
    default: ensure(false); break;
    }
}

inline void BindScratchSlot(FAqueductDynamicCS::FParameters& Params, int32 Slot, FUnorderedAccessViewRHIRef UAV)
{
	switch (Slot)
	{
	case 0:
		Params.AQ_Scratch0_Uint = UAV; Params.AQ_Scratch0_Int = UAV; Params.AQ_Scratch0_Float = UAV; Params.AQ_Scratch0_Uint4 = UAV; Params.AQ_Scratch0_Float4 = UAV; break;
	case 1:
		Params.AQ_Scratch1_Uint = UAV; Params.AQ_Scratch1_Int = UAV; Params.AQ_Scratch1_Float = UAV; Params.AQ_Scratch1_Uint4 = UAV; Params.AQ_Scratch1_Float4 = UAV; break;
	case 2:
		Params.AQ_Scratch2_Uint = UAV; Params.AQ_Scratch2_Int = UAV; Params.AQ_Scratch2_Float = UAV; Params.AQ_Scratch2_Uint4 = UAV; Params.AQ_Scratch2_Float4 = UAV; break;
	case 3:
		Params.AQ_Scratch3_Uint = UAV; Params.AQ_Scratch3_Int = UAV; Params.AQ_Scratch3_Float = UAV; Params.AQ_Scratch3_Uint4 = UAV; Params.AQ_Scratch3_Float4 = UAV; break;
	case 4:
		Params.AQ_Scratch4_Uint = UAV; Params.AQ_Scratch4_Int = UAV; Params.AQ_Scratch4_Float = UAV; Params.AQ_Scratch4_Uint4 = UAV; Params.AQ_Scratch4_Float4 = UAV; break;
	case 5:
		Params.AQ_Scratch5_Uint = UAV; Params.AQ_Scratch5_Int = UAV; Params.AQ_Scratch5_Float = UAV; Params.AQ_Scratch5_Uint4 = UAV; Params.AQ_Scratch5_Float4 = UAV; break;
	case 6:
		Params.AQ_Scratch6_Uint = UAV; Params.AQ_Scratch6_Int = UAV; Params.AQ_Scratch6_Float = UAV; Params.AQ_Scratch6_Uint4 = UAV; Params.AQ_Scratch6_Float4 = UAV; break;
	case 7:
		Params.AQ_Scratch7_Uint = UAV; Params.AQ_Scratch7_Int = UAV; Params.AQ_Scratch7_Float = UAV; Params.AQ_Scratch7_Uint4 = UAV; Params.AQ_Scratch7_Float4 = UAV; break;
	default:
		break;
	}
}

} // namespace UE::Aqueduct
