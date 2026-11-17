#include "AqueductTextureWriter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AqueductShader.h"
#include "AqueductLiveTextureResource.h"
#include "Engine/Texture2D.h"
#include "AqueductFfi.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "DataDrivenShaderPlatformInfo.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductWriter, Log, All);

namespace AqueductTextureWriter
{

// ---------------------------------------------------------------------------
// Delivery classification
// ---------------------------------------------------------------------------

static const TCHAR* GOrmComponents[3] = { TEXT("ao"), TEXT("roughness"), TEXT("metallic") };

const TCHAR* const* OrmComponentOutputs()
{
	return GOrmComponents;
}

bool IsOrmComponentOutput(const FString& OutputName)
{
	return OutputName == GOrmComponents[0]
		|| OutputName == GOrmComponents[1]
		|| OutputName == GOrmComponents[2];
}

const TCHAR* OrmDeliveryName()
{
	return TEXT("orm");
}

static const TCHAR* GAnisoComponents[2] = { TEXT("anisotropy"), TEXT("tangent") };

const TCHAR* const* AnisotropyComponentOutputs()
{
	return GAnisoComponents;
}

bool IsAnisotropyComponentOutput(const FString& OutputName)
{
	return OutputName == GAnisoComponents[0] || OutputName == GAnisoComponents[1];
}

const TCHAR* AnisotropyDeliveryName()
{
	return TEXT("anisotropy");
}

FAnisotropyConstants ResolveAnisotropyConstantsFromBindingsJson(const FString& BindingsJson)
{
	FAnisotropyConstants Result;
	TSharedPtr<FJsonObject> Outputs;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BindingsJson);
	if (!FJsonSerializer::Deserialize(Reader, Outputs) || !Outputs)
	{
		return Result;
	}
	if (const TSharedPtr<FJsonValue>* AVal = Outputs->Values.Find(TEXT("anisotropy")))
	{
		if (AVal->IsValid())
		{
			float Scalar = 0.0f;
			if ((*AVal)->Type == EJson::Number)
			{
				Scalar = (float)(*AVal)->AsNumber();
			}
			else if ((*AVal)->Type == EJson::Array && (*AVal)->AsArray().Num() > 0)
			{
				Scalar = (float)(*AVal)->AsArray()[0]->AsNumber();
			}
			if (FMath::IsFinite(Scalar))
			{
				Result.Anisotropy = FMath::Clamp(Scalar, 0.0f, 1.0f);
			}
		}
	}
	if (const TSharedPtr<FJsonValue>* TVal = Outputs->Values.Find(TEXT("tangent")))
	{
		if (TVal->IsValid())
		{
			float R = 1.0f;
			float G = 0.5f;
			bool bNumeric = false;
			if ((*TVal)->Type == EJson::Number)
			{
				R = (float)(*TVal)->AsNumber();
				G = R;
				bNumeric = true;
			}
			else if ((*TVal)->Type == EJson::Array && (*TVal)->AsArray().Num() >= 2)
			{
				const auto& Arr = (*TVal)->AsArray();
				R = (float)Arr[0]->AsNumber();
				G = (float)Arr[1]->AsNumber();
				bNumeric = true;
			}
			if (bNumeric)
			{
				const FVector2f Raw(R * 2.0f - 1.0f, G * 2.0f - 1.0f);
				const float Len = Raw.Size();
				if (FMath::IsFinite(Len) && Len > 1e-5f)
				{
					Result.TangentDir = Raw / Len;
				}
			}
		}
	}
	return Result;
}

EAqueductMapKind ClassifyOutput(const FString& OutputName, EPixelFormat SrcPF)
{
	if (OutputName == TEXT("base_color")) return EAqueductMapKind::Color;
	if (OutputName == TEXT("albedo")) return EAqueductMapKind::Color; // legacy name for base_color
	if (OutputName == TEXT("emissive")) return EAqueductMapKind::Color;
	if (OutputName == TEXT("fuzz_color")) return EAqueductMapKind::Color;
	if (OutputName == TEXT("normal")) return EAqueductMapKind::Normal;
	if (OutputName == TEXT("height") || OutputName == TEXT("opacity") || OutputName == TEXT("fuzz_weight"))
		return EAqueductMapKind::Gray;
	if (OutputName == OrmDeliveryName()) return EAqueductMapKind::Orm;
	if (OutputName == AnisotropyDeliveryName()) return EAqueductMapKind::Anisotropy;
	// Generic outputs: single-channel data is grayscale by core contract.
	return SrcPF == PF_R32_FLOAT ? EAqueductMapKind::Gray : EAqueductMapKind::Color;
}

EPixelFormat PackedPixelFormat(EAqueductMapKind Kind)
{
	switch (Kind)
	{
	case EAqueductMapKind::Normal: return PF_A16B16G16R16;
	case EAqueductMapKind::Gray:   return PF_G8;
	default:                       return PF_R8G8B8A8;  // Color, Orm, Anisotropy
	}
}

void EnsureLiveDeliveryResource(
	UTexture2D* Tex, EAqueductMapKind Kind, uint32 Size, FTextureResource*& InstalledPtr)
{
	if (Size == 0) return;
	const EPixelFormat PackPF = PackedPixelFormat(Kind);
	const uint8 Mips = (uint8)(FMath::FloorLog2(Size) + 1);
	const bool bSRGB = (Kind == EAqueductMapKind::Color);
	AqueductLiveTexture::EnsureLiveResource(Tex, PackPF, Size, Mips, bSRGB, InstalledPtr);
}

/**
 * Does the core's expansion-swizzle convention say this source format
 * replicates its channel when presented as RGBA? ("rrr1": single-channel
 * data is grayscale BY CONTRACT — never (r,0,0,1).) Queried from the core
 * once per format so this backend cannot drift from the others.
 */
static bool ShouldReplicateR(EPixelFormat SrcPF)
{
	if (SrcPF != PF_R32_FLOAT) return false;
	static const bool bReplicate = []()
	{
		return UE::Aqueduct::FfiToString(aqueduct_expansion_swizzle("r32float")) == TEXT("rrr1");
	}();
	return bReplicate;
}

// ---------------------------------------------------------------------------
// Render-thread delivery chain
// ---------------------------------------------------------------------------

static FShaderResourceViewRHIRef MakeMipSRV(
	FRHICommandListImmediate& RHICmdList, const FTextureRHIRef& Tex, uint8 Mip)
{
	return RHICmdList.CreateShaderResourceView(Tex,
		FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D)
			.SetMipRange(Mip, 1));
}

static FUnorderedAccessViewRHIRef MakeMipUAV(
	FRHICommandListImmediate& RHICmdList, const FTextureRHIRef& Tex, uint8 Mip)
{
	return RHICmdList.CreateUnorderedAccessView(Tex,
		FRHIViewDesc::CreateTextureUAV()
			.SetDimension(ETextureDimension::Texture2D)
			.SetMipLevel(Mip));
}

/**
 * Run pack → mips → BC encode for one target, (re)creating scratch as
 * needed. On success, Scratch.Packed holds the quantized mip chain (the
 * bake's Source bytes at mip 0) and Scratch.Delivery (live mode) or
 * Target.DstTexture (resource-write mode) holds the final
 * BC/G8 mips. Returns the delivered texture, or null on failure.
 */

// Tracked barriers are whole-resource only (per-mip transitions trip
// IsWholeResource ensures), so mip writes stage through a single-mip scratch
// and copy in.
static void LandScratchInMip(
	FRHICommandListImmediate& RHICmdList,
	const FTextureRHIRef& Staging,
	const FTextureRHIRef& Dest,
	uint32 MipSize,
	uint8 DestMip)
{
	RHICmdList.Transition(FRHITransitionInfo(Staging, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Dest, ERHIAccess::SRVCompute, ERHIAccess::CopyDest));
	FRHICopyTextureInfo CI;
	CI.Size = FIntVector(MipSize, MipSize, 1);
	CI.DestMipIndex = DestMip;
	RHICmdList.CopyTexture(Staging, Dest, CI);
	RHICmdList.Transition(FRHITransitionInfo(Staging, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(Dest, ERHIAccess::CopyDest, ERHIAccess::SRVCompute));
}

static FTextureRHIRef RunDeliveryChainRT(
	FRHICommandListImmediate& RHICmdList,
	const FDeliveryTarget& Target,
	FDeliveryScratch& Scratch,
	const FTextureRHIRef& NormalPackedChain)
{
	// Source set + resolution.
	FTextureRHIRef Primary = Target.Src;
	if (Target.Kind == EAqueductMapKind::Orm)
	{
		for (int32 i = 0; i < 3 && !Primary.IsValid(); ++i) Primary = Target.OrmSrc[i];
	}
	else if (Target.Kind == EAqueductMapKind::Anisotropy)
	{
		Primary = Target.AnisoSrc[0].IsValid() ? Target.AnisoSrc[0] : Target.AnisoSrc[1];
	}
	if (!Primary.IsValid()) return nullptr;
	const uint32 Size = Primary->GetSizeX();
	const uint8 MipCount = (uint8)(FMath::FloorLog2(Size) + 1);

	const EPixelFormat PackPF = PackedPixelFormat(Target.Kind);
	const float QuantSteps = (Target.Kind == EAqueductMapKind::Normal) ? 65535.0f : 255.0f;

	// --- Scratch (re)creation -------------------------------------------------
	if (!Scratch.Packed.IsValid()
		|| Scratch.Packed->GetFormat() != PackPF
		|| Scratch.Packed->GetSizeX() != Size)
	{
		const FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductPacked"))
			.SetExtent(Size, Size)
			.SetFormat(PackPF)
			.SetNumMips(MipCount)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::UAVCompute);
		Scratch.Packed = RHICreateTexture(Desc);

		const uint32 ScratchSize = FMath::Max(1u, Size / 2);
		const FRHITextureCreateDesc ScratchDesc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductMipScratch"))
			.SetExtent(ScratchSize, ScratchSize)
			.SetFormat(PackPF)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::UAVCompute);
		Scratch.MipScratch = RHICreateTexture(ScratchDesc);
	}

	// ORM roughness composite (cooked-build parity) needs its normal chain in
	// float precision; the Toksvig lengths sit near 1.0 where 8-bit steps
	// would swamp the variance signal.
	const bool bComposite = Target.Kind == EAqueductMapKind::Orm
		&& NormalPackedChain.IsValid()
		&& NormalPackedChain->GetSizeX() == Size;
	if (bComposite
		&& (!Scratch.CompositeA.IsValid()
			|| Scratch.CompositeA->GetSizeX() != Size))
	{
		const FRHITextureCreateDesc PingDesc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductCompositePing"))
			.SetExtent(Size, Size)
			.SetFormat(PF_FloatRGBA)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::UAVCompute);
		Scratch.CompositeA = RHICreateTexture(PingDesc);
		Scratch.CompositeB = RHICreateTexture(PingDesc);

		const FRHITextureCreateDesc TopDesc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductCompositeTop"))
			.SetExtent(Size, Size)
			.SetFormat(PackPF)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::UAVCompute);
		Scratch.CompositeTop = RHICreateTexture(TopDesc);
	}

	// The delivered texture is the asset's own FAqueductLiveTextureResource — the
	// UNCOMPRESSED (PackPF) texture this chain copies into. Both the editor
	// session and the cooked-animated runtime install it; materials, the texture
	// editor, and thumbnails all sample it. Its bytes are identical to what the
	// bake turns into BC7, so the live image matches the cooked asset.
	FTextureRHIRef Delivery = Target.DstTexture;
	if (!Delivery.IsValid()) return nullptr;
	if (Delivery->GetFormat() != PackPF || Delivery->GetSizeX() != Size)
	{
		// The live resource isn't installed yet (engine still building the
		// asset, or a resolution change is mid-flight). Skip; the install
		// maintenance puts it in place and the next tick delivers.
		return nullptr;
	}

	const auto& ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// --- Stage 1: pack mip 0 ---------------------------------------------------
	RHICmdList.Transition(FRHITransitionInfo(Scratch.Packed, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	const uint32 PackGroups = FMath::DivideAndRoundUp(Size, 16u);

	auto SrcSRV = [&](const FTextureRHIRef& T) -> FShaderResourceViewRHIRef
	{
		RHICmdList.Transition(FRHITransitionInfo(T, ERHIAccess::Unknown, ERHIAccess::SRVCompute));
		return RHICmdList.CreateShaderResourceView(T,
			FRHIViewDesc::CreateTextureSRV().SetDimension(ETextureDimension::Texture2D));
	};

	switch (Target.Kind)
	{
	case EAqueductMapKind::Orm:
	{
		TShaderMapRef<FAqueductPackOrmCS> Shader(ShaderMap);
		if (!ensure(Shader.IsValid())) return nullptr;
		FAqueductPackOrmCS::FParameters P;
		uint32 Mask = 0;
		FShaderResourceViewRHIRef Fallback = SrcSRV(Primary);
		FShaderResourceViewRHIRef Views[3];
		for (int32 i = 0; i < 3; ++i)
		{
			if (Target.OrmSrc[i].IsValid())
			{
				Mask |= (1u << i);
				Views[i] = (Target.OrmSrc[i] == Primary) ? Fallback : SrcSRV(Target.OrmSrc[i]);
			}
			else
			{
				Views[i] = Fallback;  // bound but masked out in the shader
			}
		}
		P.InputA = Views[0];
		P.InputB = Views[1];
		P.InputC = Views[2];
		P.PresenceMask = Mask;
		P.OutPacked = MakeMipUAV(RHICmdList, Scratch.Packed, 0);
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(PackGroups, PackGroups, 1));
		break;
	}
	case EAqueductMapKind::Anisotropy:
	{
		TShaderMapRef<FAqueductPackAnisotropyCS> Shader(ShaderMap);
		if (!ensure(Shader.IsValid())) return nullptr;
		FAqueductPackAnisotropyCS::FParameters P;
		uint32 Mask = 0;
		FShaderResourceViewRHIRef Fallback = SrcSRV(Primary);
		if (Target.AnisoSrc[0].IsValid())
		{
			Mask |= 1u;
			P.InputA = (Target.AnisoSrc[0] == Primary) ? Fallback : SrcSRV(Target.AnisoSrc[0]);
		}
		else
		{
			P.InputA = Fallback;
		}
		if (Target.AnisoSrc[1].IsValid())
		{
			Mask |= 2u;
			P.InputB = (Target.AnisoSrc[1] == Primary) ? Fallback : SrcSRV(Target.AnisoSrc[1]);
			P.bReplicateR = ShouldReplicateR(Target.AnisoSrc[1]->GetFormat()) ? 1 : 0;
		}
		else
		{
			P.InputB = Fallback;
			P.bReplicateR = 0;
		}
		P.PresenceMask = Mask;
		P.DefaultAnisotropy = Target.AnisotropyDefault;
		P.DefaultTangentDir = Target.TangentDefault;
		P.OutPacked = MakeMipUAV(RHICmdList, Scratch.Packed, 0);
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(PackGroups, PackGroups, 1));
		break;
	}
	case EAqueductMapKind::Normal:
	{
		TShaderMapRef<FAqueductPackNormalCS> Shader(ShaderMap);
		if (!ensure(Shader.IsValid())) return nullptr;
		FAqueductPackNormalCS::FParameters P;
		P.InputA = SrcSRV(Primary);
		P.bReplicateR = ShouldReplicateR(Primary->GetFormat()) ? 1 : 0;
		P.bFlipGreen = Target.bFlipNormalGreen;
		P.OutPacked = MakeMipUAV(RHICmdList, Scratch.Packed, 0);
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(PackGroups, PackGroups, 1));
		break;
	}
	case EAqueductMapKind::Gray:
	{
		TShaderMapRef<FAqueductPackGrayCS> Shader(ShaderMap);
		if (!ensure(Shader.IsValid())) return nullptr;
		FAqueductPackGrayCS::FParameters P;
		P.InputA = SrcSRV(Primary);
		P.OutPackedGray = MakeMipUAV(RHICmdList, Scratch.Packed, 0);
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(PackGroups, PackGroups, 1));
		break;
	}
	default:  // Color
	{
		TShaderMapRef<FAqueductPackColorCS> Shader(ShaderMap);
		if (!ensure(Shader.IsValid())) return nullptr;
		FAqueductPackColorCS::FParameters P;
		P.InputA = SrcSRV(Primary);
		P.bReplicateR = ShouldReplicateR(Primary->GetFormat()) ? 1 : 0;
		P.OutPacked = MakeMipUAV(RHICmdList, Scratch.Packed, 0);
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(PackGroups, PackGroups, 1));
		break;
	}
	}
	RHICmdList.Transition(FRHITransitionInfo(Scratch.Packed, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	// --- Stage 2: mip chain on the quantized data -------------------------------
	for (uint8 Mip = 1; Mip < MipCount; ++Mip)
	{
		const uint32 MipSize = FMath::Max(1u, Size >> Mip);
		const uint32 Groups = FMath::DivideAndRoundUp(MipSize, 16u);
		if (Target.Kind == EAqueductMapKind::Gray)
		{
			TShaderMapRef<FAqueductDownsampleGrayCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductDownsampleGrayCS::FParameters P;
			P.SrcMipGray = MakeMipSRV(RHICmdList, Scratch.Packed, Mip - 1);
			P.OutDims = FUintVector2(MipSize, MipSize);
			P.OutPackedGray = MakeMipUAV(RHICmdList, Scratch.MipScratch, 0);
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(Groups, Groups, 1));
		}
		else
		{
			TShaderMapRef<FAqueductDownsampleCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductDownsampleCS::FParameters P;
			P.SrcMip = MakeMipSRV(RHICmdList, Scratch.Packed, Mip - 1);
			P.QuantSteps = QuantSteps;
			P.OutDims = FUintVector2(MipSize, MipSize);
			P.OutPacked = MakeMipUAV(RHICmdList, Scratch.MipScratch, 0);
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(Groups, Groups, 1));
		}
		LandScratchInMip(RHICmdList, Scratch.MipScratch, Scratch.Packed, MipSize, Mip);
	}

	// --- Stage 2b: normal mip renormalization (cooked-build parity) -------------
	// The cooked build normalizes every generated mip after building the
	// unnormalized chain; same order here. Mip 0 is already unit length.
	if (Target.Kind == EAqueductMapKind::Normal)
	{
		for (uint8 Mip = 1; Mip < MipCount; ++Mip)
		{
			const uint32 MipSize = FMath::Max(1u, Size >> Mip);
			const uint32 Groups = FMath::DivideAndRoundUp(MipSize, 16u);
			TShaderMapRef<FAqueductNormalizeNormalCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductNormalizeNormalCS::FParameters P;
			P.NormalizeSrc = MakeMipSRV(RHICmdList, Scratch.Packed, Mip);
			P.NormalizeOut = MakeMipUAV(RHICmdList, Scratch.MipScratch, 0);
			P.NormalizeDims = FUintVector2(MipSize, MipSize);
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(Groups, Groups, 1));
			LandScratchInMip(RHICmdList, Scratch.MipScratch, Scratch.Packed, MipSize, Mip);
		}
	}

	// --- Stage 2c: ORM roughness composite (cooked-build parity) ----------------
	// Epic's CompositeTexture build, replicated: a normal chain (renormalized
	// + Gaussian top, unnormalized Gaussian mips) folded once into each level
	// of the clean roughness chain, top included. The chain ping-pongs A<->B;
	// Packed mip 0 stays clean (it is the saved Source, which the cooked
	// build composites itself), so the delivered top stages in CompositeTop.
	if (bComposite)
	{
		const uint32 TopGroups = FMath::DivideAndRoundUp(Size, 16u);
		{
			TShaderMapRef<FAqueductCompositeBlurHCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductCompositeBlurHCS::FParameters P;
			P.BlurSrcPacked = MakeMipSRV(RHICmdList, NormalPackedChain, 0);
			P.BlurOutSigned = MakeMipUAV(RHICmdList, Scratch.CompositeA, 0);
			P.BlurDims = FUintVector2(Size, Size);
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(TopGroups, TopGroups, 1));
		}
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeA, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		{
			TShaderMapRef<FAqueductCompositeBlurVCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductCompositeBlurVCS::FParameters P;
			P.BlurSrcSigned = MakeMipSRV(RHICmdList, Scratch.CompositeA, 0);
			P.BlurOutSigned = MakeMipUAV(RHICmdList, Scratch.CompositeB, 0);
			P.BlurDims = FUintVector2(Size, Size);
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(TopGroups, TopGroups, 1));
		}
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeB, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		{
			TShaderMapRef<FAqueductCompositeOrmRoughnessCS> Shader(ShaderMap);
			if (!ensure(Shader.IsValid())) return nullptr;
			FAqueductCompositeOrmRoughnessCS::FParameters P;
			P.CompositeOrmSrc = MakeMipSRV(RHICmdList, Scratch.Packed, 0);
			P.CompositeNormalMip = MakeMipSRV(RHICmdList, Scratch.CompositeB, 0);
			P.CompositeOrm = MakeMipUAV(RHICmdList, Scratch.CompositeTop, 0);
			P.CompositeDims = FUintVector2(Size, Size);
			P.CompositePower = 1.0f;
			FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(TopGroups, TopGroups, 1));
		}
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeTop, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));

		FTextureRHIRef Cur = Scratch.CompositeB;
		FTextureRHIRef Next = Scratch.CompositeA;
		for (uint8 Mip = 1; Mip < MipCount; ++Mip)
		{
			const uint32 MipSize = FMath::Max(1u, Size >> Mip);
			const uint32 Groups = FMath::DivideAndRoundUp(MipSize, 16u);
			RHICmdList.Transition(FRHITransitionInfo(Next, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute));
			{
				TShaderMapRef<FAqueductCompositeChainDownCS> Shader(ShaderMap);
				if (!ensure(Shader.IsValid())) return nullptr;
				FAqueductCompositeChainDownCS::FParameters P;
				P.BlurSrcSigned = MakeMipSRV(RHICmdList, Cur, 0);
				P.BlurOutSigned = MakeMipUAV(RHICmdList, Next, 0);
				P.BlurDims = FUintVector2(MipSize, MipSize);
				FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(Groups, Groups, 1));
			}
			RHICmdList.Transition(FRHITransitionInfo(Next, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
			{
				TShaderMapRef<FAqueductCompositeOrmRoughnessCS> Shader(ShaderMap);
				if (!ensure(Shader.IsValid())) return nullptr;
				FAqueductCompositeOrmRoughnessCS::FParameters P;
				P.CompositeOrmSrc = MakeMipSRV(RHICmdList, Scratch.Packed, Mip);
				P.CompositeNormalMip = MakeMipSRV(RHICmdList, Next, 0);
				P.CompositeOrm = MakeMipUAV(RHICmdList, Scratch.MipScratch, 0);
				P.CompositeDims = FUintVector2(MipSize, MipSize);
				P.CompositePower = 1.0f;
				FComputeShaderUtils::Dispatch(RHICmdList, Shader, P, FIntVector(Groups, Groups, 1));
			}
			LandScratchInMip(RHICmdList, Scratch.MipScratch, Scratch.Packed, MipSize, Mip);
			Swap(Cur, Next);
		}
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeA, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeB, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute));
	}

	// --- Stage 3: deliver -------------------------------------------------------
	// The uncompressed packed chain IS what every surface samples — copy it into
	// the asset's live texture. No BC encode: dynamic content is recomputed every
	// frame, so block-compressing it would only cost a pass and quality (and
	// there is no realtime BC7). The cooked STATIC asset is BC, built by Oodle.
	RHICmdList.Transition(FRHITransitionInfo(Scratch.Packed, ERHIAccess::SRVCompute, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Delivery, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	{
		FRHICopyTextureInfo CI;
		CI.NumMips = FMath::Min(MipCount, (uint8)Delivery->GetNumMips());
		RHICmdList.CopyTexture(Scratch.Packed, Delivery, CI);
	}
	if (bComposite)
	{
		FRHICopyTextureInfo CI;
		CI.Size = FIntVector(Size, Size, 1);
		RHICmdList.CopyTexture(Scratch.CompositeTop, Delivery, CI);
		RHICmdList.Transition(FRHITransitionInfo(Scratch.CompositeTop, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
	}
	RHICmdList.Transition(FRHITransitionInfo(Scratch.Packed, ERHIAccess::CopySrc, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(Delivery, ERHIAccess::CopyDest, ERHIAccess::SRVMask));

	return Delivery;
}

void EnqueueDeliverOutputs(
	TArray<FDeliveryTarget>&& Targets,
	TMap<FString, FDeliveryScratch>* RenderThreadScratch)
{
	if (Targets.Num() == 0) return;

	ENQUEUE_RENDER_COMMAND(AqueductDeliverOutputs)(
		[Targets = MoveTemp(Targets), ScratchMap = RenderThreadScratch](
			FRHICommandListImmediate& RHICmdList)
	{
		DeliverOutputsRT(RHICmdList, Targets, ScratchMap);
	});
}

void DeliverOutputsRT(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FDeliveryTarget>& Targets,
	TMap<FString, FDeliveryScratch>* ScratchMap)
{
	// Scratch is always caller-owned (the editor session / cooked runtime keep
	// it alive across deliveries and free it under a render fence). A null map
	// would leak render-thread textures into an unmanaged static, so require it.
	if (!ensure(ScratchMap)) return;
	TMap<FString, FDeliveryScratch>& Map = *ScratchMap;

	// A batch is one graph's targets. The ORM chain consumes the normal
	// chain's mips (roughness composite), so order normals first and remember
	// the packed chain; the scratch map keeps it alive across the loop.
	TArray<const FDeliveryTarget*> Ordered;
	Ordered.Reserve(Targets.Num());
	for (const FDeliveryTarget& Target : Targets)
	{
		if (Target.Kind == EAqueductMapKind::Normal) Ordered.Insert(&Target, 0);
		else Ordered.Add(&Target);
	}

	FTextureRHIRef NormalPackedChain;
	// A partial redelivery (roughness branch edited, normal untouched) has no
	// normal target in the batch; its packed chain persists in the scratch
	// map, so the ORM composite keeps its variance source instead of silently
	// delivering uncomposited roughness mips.
	if (const FDeliveryScratch* NormalScratch = Map.Find(TEXT("normal")))
	{
		NormalPackedChain = NormalScratch->Packed;
	}
	for (const FDeliveryTarget* TargetPtr : Ordered)
	{
		const FDeliveryTarget& Target = *TargetPtr;
		FDeliveryScratch& Scratch = Map.FindOrAdd(Target.Name);
		FTextureRHIRef Delivery =
			RunDeliveryChainRT(RHICmdList, Target, Scratch, NormalPackedChain);
		if (!Delivery.IsValid()) continue;
		if (Target.Kind == EAqueductMapKind::Normal) NormalPackedChain = Scratch.Packed;

		if (Target.TextureReference.IsValid())
		{
			// Repoint what materials sample at the uncompressed delivery.
			// Idempotent; re-asserted every copy, so any engine resource
			// init that repointed the reference is corrected next copy. Both
			// the level viewport and the material-editor preview sample
			// through this reference, so both go live. The texture-editor
			// image and thumbnails sample GetResource()->TextureRHI instead;
			// those are covered by the live texture resource set on the asset
			// (see the editor's live-resource override), not from here.
			RHICmdList.UpdateTextureReference(Target.TextureReference, Delivery);
		}

		UE_LOG(LogAqueductWriter, Verbose, TEXT("deliver %s: kind=%d %dx%d mips=%d → pf=%d"),
			*Target.Name, (int32)Target.Kind, Delivery->GetSizeX(), Delivery->GetSizeY(),
			Delivery->GetNumMips(), (int32)Delivery->GetFormat());
	}
}


} // namespace AqueductTextureWriter
