// Aqueduct GPU shaders — compiled through UE's standard shader pipeline.

#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Proof-of-life compute shader: writes solid red to a UAV texture.
class AQUEDUCTRUNTIME_API FAqueductTestCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductTestCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductTestCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Blit shader: copies any texture to a UAV via textureLoad.
// Used for format conversion and future display pipeline needs.
class AQUEDUCTRUNTIME_API FAqueductBlitCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductBlitCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductBlitCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(uint32, bReplicateR)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Mipmap generation: 2x2 box-filter downsample from one mip level to the next.
class AQUEDUCTRUNTIME_API FAqueductMipGenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductMipGenCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductMipGenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D, SrcMip)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, DstMip)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// R32Float mipmap generation: same filter as FAqueductMipGenCS with typed scalar output.
class AQUEDUCTRUNTIME_API FAqueductMipGenR32CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductMipGenR32CS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductMipGenR32CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float>, SrcMipR32)
		SHADER_PARAMETER_UAV(RWTexture2D<float>, DstMipR32)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ---------------------------------------------------------------------------
// Delivery chain (AqueductDeliver.usf): pack → mips → BC encode.
// See the .usf header for the contract; these classes are 1:1 entry points.
// ---------------------------------------------------------------------------

// Pack stage. One class per map kind (distinct bindings/quantization).
class AQUEDUCTRUNTIME_API FAqueductPackColorCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductPackColorCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductPackColorCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputA)
		SHADER_PARAMETER(uint32, bReplicateR)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductPackOrmCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductPackOrmCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductPackOrmCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputA)
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputB)
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputC)
		SHADER_PARAMETER(uint32, PresenceMask)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductPackNormalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductPackNormalCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductPackNormalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputA)
		SHADER_PARAMETER(uint32, bReplicateR)
		SHADER_PARAMETER(uint32, bFlipGreen)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductPackGrayCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductPackGrayCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductPackGrayCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, InputA)
		SHADER_PARAMETER_UAV(RWTexture2D<float>, OutPackedGray)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Mip downsample on quantized data (re-quantized per level).
class AQUEDUCTRUNTIME_API FAqueductDownsampleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, SrcMip)
		SHADER_PARAMETER(float, QuantSteps)
		SHADER_PARAMETER(FUintVector2, OutDims)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutPacked)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Normal mip renormalization: post-pass over the clean quantized chain,
// mirroring the cooked build's bNormalizeNormals NormalizeMip pass.
class AQUEDUCTRUNTIME_API FAqueductNormalizeNormalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductNormalizeNormalCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductNormalizeNormalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, NormalizeSrc)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, NormalizeOut)
		SHADER_PARAMETER(FUintVector2, NormalizeDims)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Composite normal source chain (mirrors the cooked build's GenerateTopMip
// blur + unnormalized box chain): separable Gaussian passes over renormalized
// signed normals, and a float box downsample for the chain levels.
class AQUEDUCTRUNTIME_API FAqueductCompositeBlurHCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductCompositeBlurHCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductCompositeBlurHCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, BlurSrcPacked)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, BlurOutSigned)
		SHADER_PARAMETER(FUintVector2, BlurDims)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductCompositeBlurVCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductCompositeBlurVCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductCompositeBlurVCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, BlurSrcSigned)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, BlurOutSigned)
		SHADER_PARAMETER(FUintVector2, BlurDims)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductCompositeChainDownCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductCompositeChainDownCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductCompositeChainDownCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, BlurSrcSigned)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, BlurOutSigned)
		SHADER_PARAMETER(FUintVector2, BlurDims)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ORM roughness composite: Epic's CompositeNormalLengthToRoughness applied
// once per mip of a clean roughness chain, reading the matching level of the
// composite normal source chain, so live deliveries match the cooked
// CompositeTexture build.
class AQUEDUCTRUNTIME_API FAqueductCompositeOrmRoughnessCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductCompositeOrmRoughnessCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductCompositeOrmRoughnessCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float4>, CompositeOrmSrc)
		SHADER_PARAMETER_SRV(Texture2D<float4>, CompositeNormalMip)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, CompositeOrm)
		SHADER_PARAMETER(FUintVector2, CompositeDims)
		SHADER_PARAMETER(float, CompositePower)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class AQUEDUCTRUNTIME_API FAqueductDownsampleGrayCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductDownsampleGrayCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductDownsampleGrayCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D<float>, SrcMipGray)
		SHADER_PARAMETER(FUintVector2, OutDims)
		SHADER_PARAMETER_UAV(RWTexture2D<float>, OutPackedGray)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Blue-noise dithered float16 → rgba8 conversion.
class AQUEDUCTRUNTIME_API FAqueductDitherCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAqueductDitherCS);
	SHADER_USE_PARAMETER_STRUCT(FAqueductDitherCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER_SRV(Texture2D, BlueNoiseTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
