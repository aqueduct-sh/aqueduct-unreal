#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Engine/TextureDefines.h"
#include "AqueductTextureWriter.h"
#include "AqueductTexture.generated.h"

class UTexture2D;
class UAqueductGraph;

/**
 * Aqueduct metadata attached to the plain UTexture2D assets that hold graph
 * output pixels. (UTexture2D is MinimalAPI and cannot be subclassed from a
 * plugin module, so the metadata travels as asset user data — the engine's
 * designed extension point — and the textures themselves stay maximally
 * stock: serialization, DDC, cooking, and thumbnails are untouched engine
 * paths.)
 *
 * The textures are simultaneously saved assets and live preview targets:
 * - Baked pixels persist through the standard Source → PlatformData
 *   pipeline; teammates without the plugin see the last bake.
 * - During live editing, animation, and (for animated graphs) game runtime,
 *   the Aqueduct executor renders into a plugin-owned UNCOMPRESSED live-texture
 *   resource installed on the asset (FAqueductLiveTextureResource), reverting
 *   to the engine-built resource at rest. Material Instance Constants point at
 *   these textures permanently.
 *
 * Format policy: the cooked/baked asset is exactly what a UE artist's imported
 * textures are — 8/16-bit Source, standard compression per map, mips. See
 * AqueductTexture::ApplyDeliverySettings / AqueductTextureWriter::EAqueductMapKind:
 *   Color  → TSF_BGRA8  source, TC_BC7,       sRGB on   (builds BC7)
 *   Orm    → TSF_BGRA8  source, TC_BC7,       sRGB off  (builds BC7)
 *   Normal → TSF_RGBA16 source, TC_Normalmap, sRGB off  (builds BC5)
 *   Gray   → TSF_G8     source, TC_Grayscale, sRGB off  (builds BC4)
 */
UCLASS()
class AQUEDUCTRUNTIME_API UAqueductTextureUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Which graph output this texture holds (base_color, normal, roughness, ...). */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	FString OutputName;

	/**
	 * Core-computed dependency-closure hash of THIS texture's output at bake
	 * time: it covers exactly the upstream subgraph that can affect this
	 * output's pixels (canonical structure — formatting/positions never
	 * matter). Mismatch vs the graph's current per-output hash ⇒ this
	 * texture, and only this texture, is stale.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	FString GraphContentHash;

	/** True for animated graphs: cooked writable, overwritten at game runtime. */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	bool bDynamicWritable = false;

	/** The graph asset this texture derives from (cook dependency anchor). */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	TObjectPtr<UAqueductGraph> SourceGraph;
};

namespace AqueductTexture
{
	/** Get the Aqueduct user data on a texture, optionally creating it. */
	AQUEDUCTRUNTIME_API UAqueductTextureUserData* GetUserData(UTexture2D* Texture, bool bCreate);

#if WITH_EDITOR
	/** The Source layout each map kind persists. */
	AQUEDUCTRUNTIME_API ETextureSourceFormat SourceFormatForKind(
		AqueductTextureWriter::EAqueductMapKind Kind);

	/**
	 * Apply the standard per-kind asset settings (compression class, sRGB,
	 * LOD group, mips). Idempotent. bDynamicWritable (animated graphs) keeps
	 * the texture fully resident — its mips are rewritten at runtime, which
	 * streaming would invalidate.
	 */
	AQUEDUCTRUNTIME_API void ApplyDeliverySettings(
		UTexture2D* Texture,
		AqueductTextureWriter::EAqueductMapKind Kind,
		bool bDynamicWritable);

	/**
	 * Split pixel-write API so the pixel copy can run on a worker thread —
	 * UObject bookkeeping is game-thread law, but it is all constant-time;
	 * only the copy scales with resolution and it never belongs on the game
	 * thread.
	 *
	 * Game thread:  uint8* Dst = BeginWritePixels(Tex, Size, Kind);
	 * Any thread:   write Size*Size*BytesPerPixel(SourceFormatForKind(Kind))
	 * Game thread:  EndWritePixels(Tex);   // unlock + async rebuild kick-off
	 *
	 * BeginWritePixels applies the per-kind asset settings. EndWritePixels
	 * triggers the engine's ASYNC texture build; callers needing the GPU
	 * resource poll IsAsyncCacheComplete rather than blocking.
	 */
	AQUEDUCTRUNTIME_API uint8* BeginWritePixels(
		UTexture2D* Texture,
		int32 InSize,
		AqueductTextureWriter::EAqueductMapKind Kind,
		bool bDynamicWritable);

	AQUEDUCTRUNTIME_API void EndWritePixels(UTexture2D* Texture);

	/** One-shot convenience (Begin + copy + End on the calling thread).
	 *  Game thread only; for blocking/unattended paths and zero-fills. */
	AQUEDUCTRUNTIME_API void InitFromPixels(
		UTexture2D* Texture,
		int32 InSize,
		AqueductTextureWriter::EAqueductMapKind Kind,
		bool bDynamicWritable,
		const uint8* PixelData);

	/** Bytes per pixel of a supported texture-source format. */
	AQUEDUCTRUNTIME_API int64 BytesPerPixel(ETextureSourceFormat Format);
#endif
}
