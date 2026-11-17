#pragma once

#include "CoreMinimal.h"
#include "RHI.h"

class FTextureResource;
class UTexture2D;

/**
 * GPU-side delivery of executor outputs to what materials sample. Float graph
 * outputs are an internal stage; the delivery chain quantizes them to the exact
 * bytes the baked asset's Source holds, builds the mip chain, and hands
 * materials a texture to sample.
 *
 * Dynamic content — the editor live session AND cooked-animated graphs — is
 * sampled UNCOMPRESSED: it is recomputed every frame, so there is nothing to
 * gain from block-compressing it, and uncompressed is lossless and encoder-free.
 * The persisted/cooked STATIC asset carries a block format, produced by the
 * engine's Oodle build from the same Source bytes (see
 * AqueductTexture::ApplyDeliverySettings):
 *
 *   base_color / generic color → BC7, sRGB        (bytes = core quantization)
 *   orm (AO/rough/metal)   → BC7, linear      (R=AO G=Rough B=Metal)
 *   normal                 → BC5              (16-bit packed source)
 *   height / generic gray  → BC4, linear
 *
 * Chain per delivery: pack (quantize to Source bytes) → mip downsample → copy
 * the uncompressed packed chain into the delivery texture. The packed stage IS
 * the bake's Source data — one quantization code path for live, baked, and
 * CLI-exported pixels.
 *
 * One delivery mechanism: the asset is backed by a plugin-owned uncompressed
 * FAqueductLiveTextureResource (installed by the editor session / cooked
 * runtime; see EnsureLiveDeliveryResource). This chain copies into that
 * resource's texture (Target.DstTexture) and repoints the asset's RHI texture
 * reference at it (RHIUpdateTextureReference — the same mechanism texture
 * streaming uses). Materials (level viewport AND material-editor preview) sample
 * through the reference; the texture-editor image and thumbnails sample
 * GetResource()->TextureRHI — both resolve to the same live texture.
 */
namespace AqueductTextureWriter
{
	/** Delivery format class of one texture asset. */
	enum class EAqueductMapKind : uint8
	{
		Color,      // baked BC7 + sRGB (base_color, fuzz_color, generic color outputs)
		Orm,        // baked BC7 linear, R=AO G=Roughness B=Metallic
		Anisotropy, // baked BC7 linear, RG = 0.5 + 0.5 * a * (cos 2theta, sin 2theta), B=0, A=1
		Normal,     // baked BC5 (X/Y; sampler reconstructs Z)
		Gray,       // baked BC4 linear (height, opacity, fuzz_weight, generic single-channel outputs)
	};

	/** Output names packed into the ORM texture, in channel order R,G,B. */
	AQUEDUCTRUNTIME_API const TCHAR* const* OrmComponentOutputs();  // {"ao","roughness","metallic"}
	AQUEDUCTRUNTIME_API bool IsOrmComponentOutput(const FString& OutputName);

	/** Name of the packed ORM delivery texture ("orm"). */
	AQUEDUCTRUNTIME_API const TCHAR* OrmDeliveryName();

	/** Output names packed into the Anisotropy texture: {"anisotropy","tangent"}. */
	AQUEDUCTRUNTIME_API const TCHAR* const* AnisotropyComponentOutputs();
	AQUEDUCTRUNTIME_API bool IsAnisotropyComponentOutput(const FString& OutputName);

	/** Name of the packed Anisotropy delivery texture ("anisotropy"). */
	AQUEDUCTRUNTIME_API const TCHAR* AnisotropyDeliveryName();

	/** Resolved graph-level fallback constants when only one of [anisotropy, tangent] is texture-bound. */
	struct FAnisotropyConstants
	{
		float Anisotropy = 0.0f;
		FVector2f TangentDir = FVector2f(1.0f, 0.0f);
	};

	AQUEDUCTRUNTIME_API FAnisotropyConstants ResolveAnisotropyConstantsFromBindingsJson(
		const FString& BindingsJson);

	/** Classify a non-ORM output by name (base_color/normal/height), falling
	 *  back to the source pixel format for generic outputs (single-channel
	 *  → Gray, else Color). */
	AQUEDUCTRUNTIME_API EAqueductMapKind ClassifyOutput(const FString& OutputName, EPixelFormat SrcPF);

	/** The uncompressed pixel format every live/dynamic texture is delivered and
	 *  sampled in — also the bake readback format. Color/Orm → PF_R8G8B8A8,
	 *  Normal → PF_A16B16G16R16, Gray → PF_G8. The cooked STATIC asset is a
	 *  block format (BC7/BC5/BC4), built from these bytes by Oodle. */
	AQUEDUCTRUNTIME_API EPixelFormat PackedPixelFormat(EAqueductMapKind Kind);

	/** Install/maintain the uncompressed live-texture resource for a delivery of
	 *  this Kind — the single place the format/sRGB/mip mapping lives, shared by
	 *  the editor session and the cooked-animated runtime. Size is the
	 *  graph/executor resolution — the same source the delivery chain uses, so
	 *  the live resource and the delivery never disagree on size. Game thread. */
	AQUEDUCTRUNTIME_API void EnsureLiveDeliveryResource(
		UTexture2D* Tex, EAqueductMapKind Kind, uint32 Size, FTextureResource*& InstalledPtr);

	struct FDeliveryTarget
	{
		FString Name;                              // delivery/asset key
		EAqueductMapKind Kind = EAqueductMapKind::Color;
		FTextureRHIRef Src;                        // single-source kinds
		FTextureRHIRef OrmSrc[3];                  // ao, roughness, metallic (any may be null)
		FTextureRHIRef AnisoSrc[2];                // anisotropy, tangent (either may be null)
		float AnisotropyDefault = 0.0f;            // constant fallback when AnisoSrc[0] is null
		FVector2f TangentDefault = FVector2f(1.0f, 0.0f); // unit (tx, ty) fallback when AnisoSrc[1] is null
		// Normal kind only: the graph declares an OpenGL (Y-up) normal format
		// but UE materials sample DirectX (Y-down), so the pack stage inverts
		// the green channel. Set from the graph's normal_format at enqueue.
		uint32 bFlipNormalGreen = 0;
		// The live-resource texture this delivery copies into. A REF, resolved on
		// the game thread at enqueue: it pins the texture for the command's
		// lifetime (a raw FTextureResource* could dangle if the engine released
		// the resource between enqueue and execution). If the resource is swapped
		// meanwhile, this writes the old texture — harmless and repaired next tick.
		FTextureRHIRef DstTexture;
		// The asset's texture reference, repointed at DstTexture so materials
		// sample the live texture (the same mechanism texture streaming uses).
		FTextureReferenceRHIRef TextureReference;
	};

	/** Render-thread-owned scratch per delivery (created/reused there; the
	 *  game thread only keeps the shell alive and may Empty() it only at
	 *  quiescence). */
	struct FDeliveryScratch
	{
		FTextureRHIRef Packed;     // quantized data, full mip chain (= Source bytes at mip 0)
		FTextureRHIRef MipScratch; // half-size ping-pong for mip downsampling
		// ORM-with-normal only: the composite normal chain ping-pongs through
		// two float textures; the delivered top stages in CompositeTop because
		// the live resource has no UAV access.
		FTextureRHIRef CompositeA;
		FTextureRHIRef CompositeB;
		FTextureRHIRef CompositeTop;
		                           // (same-texture SRV+UAV would need per-mip
		                           // barriers, which tracked transitions forbid)
	};

	/** Enqueue the delivery chain for a set of targets. */
	AQUEDUCTRUNTIME_API void EnqueueDeliverOutputs(
		TArray<FDeliveryTarget>&& Targets,
		TMap<FString, FDeliveryScratch>* RenderThreadScratch);

	/** Render-thread body of the above, for callers already inside a render
	 *  command (e.g. the cooked runtime's execute+deliver command). */
	AQUEDUCTRUNTIME_API void DeliverOutputsRT(
		FRHICommandListImmediate& RHICmdList,
		const TArray<FDeliveryTarget>& Targets,
		TMap<FString, FDeliveryScratch>* RenderThreadScratch);

}
