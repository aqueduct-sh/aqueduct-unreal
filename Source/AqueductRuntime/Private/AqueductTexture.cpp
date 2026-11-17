#include "AqueductTexture.h"
#include "AqueductGraphAsset.h"
#include "Engine/Texture2D.h"

namespace AqueductTexture
{

using AqueductTextureWriter::EAqueductMapKind;

UAqueductTextureUserData* GetUserData(UTexture2D* Texture, bool bCreate)
{
	if (!Texture) return nullptr;

	UAqueductTextureUserData* Data = Cast<UAqueductTextureUserData>(
		Texture->GetAssetUserDataOfClass(UAqueductTextureUserData::StaticClass()));
	if (!Data && bCreate)
	{
		Data = NewObject<UAqueductTextureUserData>(Texture);
		Texture->AddAssetUserData(Data);
	}
	return Data;
}

#if WITH_EDITOR

ETextureSourceFormat SourceFormatForKind(EAqueductMapKind Kind)
{
	switch (Kind)
	{
	case EAqueductMapKind::Normal:     return TSF_RGBA16;
	case EAqueductMapKind::Gray:       return TSF_G8;
	case EAqueductMapKind::Anisotropy: return TSF_BGRA8;
	default:                           return TSF_BGRA8;  // Color, Orm
	}
}

int64 BytesPerPixel(ETextureSourceFormat Format)
{
	switch (Format)
	{
	case TSF_G8:     return 1;
	case TSF_BGRA8:  return 4;
	case TSF_RGBA16: return 8;
	default:
		checkf(false, TEXT("unsupported texture source format %d"), (int32)Format);
		return 0;
	}
}

void ApplyDeliverySettings(UTexture2D* Texture, EAqueductMapKind Kind, bool bDynamicWritable)
{
	switch (Kind)
	{
	case EAqueductMapKind::Color:
		// BC7 (sRGB): near-lossless color. BC1 (TC_Default) bands and blocks
		// on gradients; BC7 is the same shipped size as BC3 and the quality
		// standard professionals expect for base color.
		Texture->CompressionSettings = TC_BC7;
		Texture->SRGB = true;
		Texture->LODGroup = TEXTUREGROUP_World;
		break;
	case EAqueductMapKind::Orm:
		// BC7 (linear): AO/Roughness/Metallic are three independent channels —
		// the worst case for BC1's single per-block RGB endpoint line (the
		// source of the blocky ORM). BC7's partitioning keeps them near-
		// independent.
		Texture->CompressionSettings = TC_BC7;
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_World;
		break;
	case EAqueductMapKind::Anisotropy:
		// BC7 (linear): coupled double-angle vector uw in RG (0.5 + 0.5 * uw).
		// Never renormalize mips so shortened vector length encodes variance.
		Texture->CompressionSettings = TC_BC7;
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_World;
#if WITH_EDITORONLY_DATA
		Texture->bNormalizeNormals = false;
#endif
		break;
	case EAqueductMapKind::Normal:
		// BC5: two independently-interpolated channels (X/Y); the sampler
		// reconstructs Z. The standard, near-lossless normal-map format.
		Texture->CompressionSettings = TC_Normalmap;
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
#if WITH_EDITORONLY_DATA
		// Unreal's modern convention for normal maps (see UTexture::
		// SetModernSettingsForNewOrChangedTexture): mips renormalize; the
		// variance the shortening carried reaches shading through the ORM
		// roughness composite instead.
		Texture->bNormalizeNormals = true;
#endif
		break;
	case EAqueductMapKind::Gray:
		// BC4: dedicated single-channel codec (height, generic single-channel).
		// 8-bit-class precision; high-precision displacement that bands would
		// need an uncompressed R16 source instead.
		Texture->CompressionSettings = TC_Grayscale;
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_World;
		break;
	}
	Texture->CompressionNone = false;
	// Our packed sources carry an opaque alpha byte; CompressionNoAlpha tells
	// the builder to ignore it so the encoder spends all its bits on RGB and
	// never widens to an alpha-carrying mode.
	Texture->CompressionNoAlpha = true;
#if WITH_EDITORONLY_DATA
	// Modern processing path: Unreal opts every imported texture in
	// (SetModernSettingsForNewOrChangedTexture); generated textures must opt
	// in explicitly, and the live delivery chain matches its kernels.
	Texture->bUseNewMipFilter = true;
#endif
	Texture->MipGenSettings = TMGS_FromTextureGroup;
	// Animated graphs rewrite every mip at runtime; streamed-out mips would
	// drop those writes. Static graphs stream like any other texture.
	Texture->NeverStream = bDynamicWritable;
}

uint8* BeginWritePixels(UTexture2D* Texture, int32 InSize, EAqueductMapKind Kind, bool bDynamicWritable)
{
	check(IsInGameThread());

	Texture->PreEditChange(nullptr);

	// Allocate uninitialized source storage; the caller fills it (typically
	// from a worker thread) before EndWritePixels.
	Texture->Source.Init(InSize, InSize, /*NewNumSlices=*/1, /*NewNumMips=*/1,
		SourceFormatForKind(Kind), nullptr);

	ApplyDeliverySettings(Texture, Kind, bDynamicWritable);

	return Texture->Source.LockMip(0);
}

void EndWritePixels(UTexture2D* Texture)
{
	check(IsInGameThread());

	Texture->Source.UnlockMip(0);

	// Async build: DDC processing runs off the game thread; the engine swaps
	// the GPU resource in when ready. Consumers poll IsAsyncCacheComplete.
	Texture->PostEditChange();
}

void InitFromPixels(
	UTexture2D* Texture,
	int32 InSize,
	EAqueductMapKind Kind,
	bool bDynamicWritable,
	const uint8* PixelData)
{
	uint8* Dst = BeginWritePixels(Texture, InSize, Kind, bDynamicWritable);
	const int64 Bytes = (int64)InSize * InSize * BytesPerPixel(SourceFormatForKind(Kind));
	if (Dst && PixelData)
	{
		FMemory::Memcpy(Dst, PixelData, Bytes);
	}
	else if (Dst)
	{
		FMemory::Memzero(Dst, Bytes);
	}
	EndWritePixels(Texture);
}

#endif // WITH_EDITOR

} // namespace AqueductTexture
