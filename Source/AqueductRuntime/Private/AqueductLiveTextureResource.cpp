#include "AqueductLiveTextureResource.h"

#include "Engine/Texture2D.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

FAqueductLiveTextureResource::FAqueductLiveTextureResource(
	UTexture2D* InOwner,
	uint32 InSizeX,
	uint32 InSizeY,
	uint8 InNumMips,
	EPixelFormat InFormat,
	bool bInSRGB)
	: OwnerTextureReferenceRHI(InOwner ? InOwner->TextureReference.TextureReferenceRHI : nullptr)
	, OwnerName(InOwner ? InOwner->GetFName() : NAME_None)
	, SizeX(InSizeX)
	, SizeY(InSizeY)
	, NumMips(InNumMips)
	, Format(InFormat)
	, bWantSRGB(bInSRGB)
{
	// FTexture sampling hints (used by some readback/preview shaders).
	bSRGB = bInSRGB;
	bGreyScaleFormat = (InFormat == PF_G8 || InFormat == PF_R8);
}

void FAqueductLiveTextureResource::InitRHI(FRHICommandListBase& RHICmdList)
{
	// Trilinear + wrap: graphs are tileable and carry a full mip chain, exactly
	// like the cooked asset's sampler.
	FSamplerStateInitializerRHI SamplerStateInitializer(SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap);
	SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);

	ETextureCreateFlags Flags = ETextureCreateFlags::ShaderResource;
	if (bWantSRGB)
	{
		// Color bytes are sRGB-encoded values; the flag makes sampling decode
		// them, matching the cooked BC7+sRGB asset. (The delivery copy is a raw
		// byte copy, so the source's linear-ness does not matter.)
		Flags |= ETextureCreateFlags::SRGB;
	}

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("AqueductLiveResource"), SizeX, SizeY, Format)
		.SetNumMips(NumMips)
		.SetFlags(Flags)
		.SetInitialState(ERHIAccess::SRVMask);
	LiveTextureRHI = RHICmdList.CreateTexture(Desc);
	TextureRHI = LiveTextureRHI;
	TextureRHI->SetName(OwnerName);

	if (OwnerTextureReferenceRHI.IsValid())
	{
		// Repoint what materials sample (level viewport + material-editor
		// preview) at this texture — the same mechanism the streamer uses.
		RHICmdList.UpdateTextureReference(OwnerTextureReferenceRHI, TextureRHI);
	}
}

void FAqueductLiveTextureResource::ReleaseRHI()
{
	if (OwnerTextureReferenceRHI.IsValid())
	{
		// Stop pointing the reference at our (about-to-die) texture; the engine
		// resource's InitRHI repoints it when the asset reverts.
		RHIClearTextureReference(OwnerTextureReferenceRHI);
	}
	FTextureResource::ReleaseRHI();
	LiveTextureRHI.SafeRelease();
}

namespace AqueductLiveTexture
{

void EnsureLiveResource(
	UTexture2D* Tex, EPixelFormat Format, uint32 Size, uint8 NumMips,
	bool bSRGB, FTextureResource*& InstalledPtr)
{
	if (!Tex || Size == 0) return;

#if WITH_EDITOR
	// Wait for the engine's own build to finish before we take over: only then
	// is the texture reference valid, and replacing a resource mid-async-build
	// would fight the texture compiler. The async texture compiler is
	// editor-only (IsAsyncCacheComplete lives behind WITH_EDITOR); in a cooked
	// game the texture is already fully built at load, so there is nothing to
	// wait for.
	if (!Tex->IsAsyncCacheComplete()) return;
#endif
	if (!Tex->TextureReference.TextureReferenceRHI.IsValid()) return;

	// Already our resource at the right size? Identity-compare against the ptr
	// we installed — if the engine's async build later reverted it,
	// GetResource() differs and we reinstall.
	FTextureResource* Cur = Tex->GetResource();
	if (Cur && Cur == InstalledPtr && Cur->GetSizeX() == Size)
	{
		return;
	}

	// Release the asset's current resource the engine's own way, then install
	// ours and init it on the render thread. NOT UpdateResource() — that would
	// re-create the engine (BC) resource instead of taking ours. The release →
	// set → init commands run in order on the render thread, so the texture
	// reference transitions old→ours within one flush (no frame samples a
	// released texture).
	Tex->ReleaseResource();
	FAqueductLiveTextureResource* New =
		new FAqueductLiveTextureResource(Tex, Size, Size, NumMips, Format, bSRGB);
	Tex->SetResource(New);
	BeginInitResource(New);

	// Make the install SYNCHRONOUS: drain the render thread so InitRHI has run
	// and GetResource()->TextureRHI is valid before we return. Without this the
	// install is two-phase — the caller reads a null TextureRHI on this tick and
	// the delivery is skipped, leaving the freshly-installed resource UNFILLED
	// (uninitialized garbage) until some later tick re-delivers. That later
	// re-delivery only happens while a graph has unsaved edits (the maintenance
	// tick), so a reconnect/reload on a SAVED graph would otherwise install and
	// never fill. Flushing here makes a single CopyOutputsToTextures call
	// self-sufficient. Only runs on an actual (re)install — the identity check
	// above no-ops in steady state, so scrubbing pays nothing.
	FlushRenderingCommands();

	InstalledPtr = New;
}

void RevertLiveResource(UTexture2D* Tex, FTextureResource*& InstalledPtr)
{
	// Only revert what is still ours; a bake's PostEditChange may already have
	// rebuilt the engine resource. UpdateResource() releases our resource and
	// rebuilds the normal (BC) one from Source.
	if (Tex && InstalledPtr && Tex->GetResource() == InstalledPtr)
	{
		Tex->UpdateResource();
	}
	InstalledPtr = nullptr;
}

} // namespace AqueductLiveTexture
