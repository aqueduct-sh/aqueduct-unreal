#pragma once

#include "CoreMinimal.h"
#include "TextureResource.h"
#include "RHI.h"

class UTexture2D;

/**
 * An FTextureResource that backs a UTexture2D with a plugin-owned UNCOMPRESSED
 * RHI texture for the duration of a live Aqueduct session — instead of the
 * engine's streamable resource built from the asset's (stale) Source.
 *
 * Why this exists: materials sample the texture REFERENCE, and InitRHI repoints
 * it at this texture, so the level viewport and the material-editor preview go
 * live. The texture-editor image and content-browser thumbnails sample
 * GetResource()->TextureRHI directly; because this IS what GetResource()
 * returns during a session, those go live too. The delivery chain writes fresh
 * pixels into this resource's texture (via the asset's GetResource()->TextureRHI)
 * every frame — the texture object is stable, so there is no per-frame resource
 * churn and nothing for the engine's async build to clobber.
 *
 * The asset keeps its real CompressionSettings (BC7/BC5/BC4) and Source, so the
 * Details panel, the bake, and the cook are unaffected — this only overrides
 * the DISPLAYED RHI while a session is live. Reverting the asset to its normal
 * engine-built resource (UpdateResource) restores the cooked BC view.
 *
 * Lifecycle: SetResource(new ...) + BeginInitResource(...) to install (NOT
 * UpdateResource, which would re-create the engine resource); the editor's
 * resource maintenance re-installs it if the engine's async build reverts it,
 * and BeginReleaseResource + SetResource(nullptr)/UpdateResource removes it.
 */
class AQUEDUCTRUNTIME_API FAqueductLiveTextureResource : public FTextureResource
{
public:
	FAqueductLiveTextureResource(
		UTexture2D* InOwner,
		uint32 InSizeX,
		uint32 InSizeY,
		uint8 InNumMips,
		EPixelFormat InFormat,
		bool bInSRGB);

	virtual uint32 GetSizeX() const override { return SizeX; }
	virtual uint32 GetSizeY() const override { return SizeY; }

	/** Render thread: create the uncompressed texture and repoint the owner's
	 *  texture reference at it. */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

	/** Render thread: clear the reference and release the texture. */
	virtual void ReleaseRHI() override;

private:
	// Captured on the game thread at construction so InitRHI/ReleaseRHI never
	// dereference the owning UObject on the render thread (it could be GC'd
	// before a deferred release runs).
	FTextureReferenceRHIRef OwnerTextureReferenceRHI;
	FName OwnerName;
	uint32 SizeX;
	uint32 SizeY;
	uint8 NumMips;
	EPixelFormat Format;
	bool bWantSRGB;
	FTextureRHIRef LiveTextureRHI;
};

namespace AqueductLiveTexture
{
	/**
	 * Install (idempotently) an FAqueductLiveTextureResource of the given
	 * uncompressed Format/Size on Tex, so every surface that samples it shows
	 * the live pixels. InstalledPtr tracks what we installed: if
	 * Tex->GetResource() no longer equals it, the engine's async build reverted
	 * the asset and we reinstall. No-op while the engine is still building Tex
	 * (the texture reference isn't valid until then). Game thread.
	 *
	 * The install is SYNCHRONOUS — it drains the render thread so the texture is
	 * created (not just enqueued) before returning, so the caller's delivery in
	 * the same tick fills it. The texture starts empty; the delivery fills it.
	 *
	 * Shared by the editor session path and the cooked-animated runtime — the
	 * one place that decides how a live Aqueduct texture is backed.
	 */
	AQUEDUCTRUNTIME_API void EnsureLiveResource(
		UTexture2D* Tex, EPixelFormat Format, uint32 Size, uint8 NumMips,
		bool bSRGB, FTextureResource*& InstalledPtr);

	/** Hand Tex back to its engine-built (BC) resource if it still holds the
	 *  resource we installed, and clear InstalledPtr. Game thread. */
	AQUEDUCTRUNTIME_API void RevertLiveResource(UTexture2D* Tex, FTextureResource*& InstalledPtr);
}
