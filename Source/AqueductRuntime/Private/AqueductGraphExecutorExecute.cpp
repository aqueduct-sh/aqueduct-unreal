// AqueductGraphExecutor execution: the full dispatch path (first frame and
// topology changes), node dispatch, and the GPU resources it binds
// (placeholders, defaults, scratch pool, dither, mips).

#include "AqueductGraphExecutor.h"
#include "AqueductGraphExecutorShared.h"
#include "AqueductFfi.h"
#include "AqueductAssets.h"
#include "AqueductShader.h"
#include "AqueductDynamicShader.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIStaticStates.h"
#include "RHIDefinitions.h"
#include "DynamicRHI.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "Misc/ScopeLock.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderCompiler.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace UE::Aqueduct
{

const TCHAR* const GSharedPingKey = TEXT("_multipass.ping");
const TCHAR* const GSharedPongKey = TEXT("_multipass.pong");

// ---------------------------------------------------------------------------
// Shared helpers for shader compilation
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::DispatchFormatConvert(
	FRHICommandListImmediate& RHICmdList,
	FTextureRHIRef SrcTexture,
	FTextureRHIRef DstTexture,
	uint32 Resolution) const
{
	TShaderMapRef<FAqueductBlitCS> BlitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ensure(BlitShader.IsValid())) return;

	FShaderResourceViewRHIRef SrcSRV = RHICmdList.CreateShaderResourceView(
		SrcTexture, FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));

	FUnorderedAccessViewRHIRef DstUAV = RHICmdList.CreateUnorderedAccessView(
		DstTexture, FRHIViewDesc::CreateTextureUAV()
			.SetDimension(ETextureDimension::Texture2D)
			.SetMipLevel(0));

	RHICmdList.Transition(FRHITransitionInfo(SrcTexture, ERHIAccess::Unknown, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(DstTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	// FComputeShaderUtils::Dispatch — the canonical out-of-RenderGraph
	// dispatch (binds AND unbinds). The raw scratch-batched path leaves UAV
	// bindings set after dispatch; a later differently-shaped utility
	// dispatch then silently mis-binds on some RHI states — measured twice
	// (interactive-editor live blits; batch parity format converts).
	FAqueductBlitCS::FParameters PassParameters;
	PassParameters.InputTexture = SrcSRV;
	PassParameters.OutputTexture = DstUAV;
	PassParameters.bReplicateR = 0; // executor-internal converts are format moves, not display expansion

	const uint32 GroupCount = FMath::DivideAndRoundUp(Resolution, (uint32)WORKGROUP_SIZE);
	FComputeShaderUtils::Dispatch(RHICmdList, BlitShader, PassParameters,
		FIntVector(GroupCount, GroupCount, 1));

	RHICmdList.Transition(FRHITransitionInfo(DstTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

// ---------------------------------------------------------------------------
// Execution (render thread)
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::CheckBitmapSourcesResolvable(FString& OutReason)
{
	// GAME THREAD. Existence checks only (no file loads, no decode): a graph
	// whose bitmap sources cannot all resolve must not be executed at all
	// (bitmap correctness contract). Populates MissingAssetUrls
	// for the editor module's downloader. The render-thread pre-pass in
	// ExecuteFull independently enforces the same contract (and catches
	// decode failures, which need the bytes).
	MissingAssetUrls.Reset();
	OutReason.Reset();
	if (!Graph)
	{
		return true;
	}

	bool bAllResolvable = true;
	for (const FString& NodeId : PlanOrder)
	{
		if (PlanNodes[NodeId].Kind != TEXT("bitmap")) continue;

		FTCHARToUTF8 NodeIdUtf8(*NodeId);
		const TSharedPtr<FJsonObject> NObj =
			FfiJsonObject(aqueduct_graph_get_node_json(Graph, NodeIdUtf8.Get()));
		if (!NObj.IsValid()) continue;

		FString Source;
		const TSharedPtr<FJsonObject>* ParamsObj;
		if (NObj->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			(*ParamsObj)->TryGetStringField(TEXT("source"), Source);
		}
		Source.TrimStartAndEndInline();
		if (Source.IsEmpty())
		{
			continue; // empty source is a defined input state (transparent)
		}

		if (AqueductAssets::IsAssetUrl(Source))
		{
			FString Hash;
			FString Extension;
			if (!AqueductAssets::ParseAssetUrl(Source, Hash, Extension))
			{
				bAllResolvable = false;
				if (OutReason.IsEmpty())
				{
					OutReason = FString::Printf(
						TEXT("bitmap node %s: '%s' is not a whitelisted asset URL"),
						*NodeId, *Source);
				}
				continue;
			}
			if (!IFileManager::Get().FileExists(
					*AqueductAssets::AssetFilePath(Hash, Extension)))
			{
				bAllResolvable = false;
				MissingAssetUrls.AddUnique(Source);
				if (OutReason.IsEmpty())
				{
					OutReason = FString::Printf(
						TEXT("bitmap node %s: asset %s.%s is not in the project store yet"),
						*NodeId, *Hash, *Extension);
				}
			}
			continue;
		}

		FString FilePath = Source;
		if (FPaths::IsRelative(FilePath) && !GraphDirectory.IsEmpty())
		{
			FilePath = FPaths::Combine(GraphDirectory, FilePath);
		}
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		if (!IFileManager::Get().FileExists(*FilePath))
		{
			bAllResolvable = false;
			if (OutReason.IsEmpty())
			{
				OutReason = FString::Printf(
					TEXT("bitmap node %s: failed to read '%s'"), *NodeId, *FilePath);
			}
		}
	}
	return bAllResolvable;
}

void FAqueductGraphExecutor::Execute(FRHICommandListImmediate& RHICmdList)
{
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Warning, TEXT("Execute: no graph loaded"));
		return;
	}

	// Structural change (rewire, resolution, formats, outputs) invalidates
	// the compiled graph; the plan hash never covers parameter values.
	const uint32 NewHash = PlanHash;
	if (CompiledGraph.IsValid() && NewHash != TopologyHash)
	{
		CompiledGraph.Reset();
		InvalidateProfileCache();
		// VRAM peak follows the same invalidation discipline as node
		// timings: meaningful only for the current topology + resolution.
		PeakVramBytes = 0;
	}
	TopologyHash = NewHash;

	// Fast path: replay compiled graph if available
	if (CompiledGraph.IsValid())
	{
		ReplayCompiledGraph(RHICmdList);
		UpdateVramPeak();
		return;
	}

	// First execution (or after topology change): full path + build compiled graph.
	ExecuteFull(RHICmdList);
	BuildCompiledGraph(RHICmdList);
	UpdateVramPeak();
}

bool FAqueductGraphExecutor::CanSkipNode(
	const FString& NodeId, const FNodeShaderData* ShaderData) const
{
	const FString* Current = NodeSigs.Find(NodeId);
	const FString* Rendered = LastRenderedSigs.Find(NodeId);
	if (!Current || !Rendered || *Current != *Rendered)
	{
		return false;
	}
	if (TimeCone.Contains(NodeId))
	{
		const float* RenderedT = LastRenderedTimes.Find(NodeId);
		if (!RenderedT || *RenderedT != CurrentTime)
		{
			return false;
		}
	}
	if (ShaderData && ShaderData->OutputSemantics.Num() > 1)
	{
		for (const FString& OutSem : ShaderData->OutputSemantics)
		{
			if (!NodeTextures.Contains(NodeId + TEXT(".") + OutSem))
			{
				return false;
			}
		}
	}
	else if (!NodeTextures.Contains(NodeId))
	{
		return false;
	}
	const FExecutionPlanNode* Plan = PlanNodes.Find(NodeId);
	if (Plan && Plan->Kind == TEXT("bitmap") &&
		!NodeTextures.Contains(NodeId + TEXT(".__host_source")))
	{
		return false;
	}
	return true;
}

void FAqueductGraphExecutor::ExecuteFull(FRHICommandListImmediate& RHICmdList)
{
	if (!ensure(Graph)) return;

	if (!bShadersCompiled)
	{
		UE_LOG(LogAqueduct, Warning, TEXT("Execute: shaders not compiled, falling back to test shader"));
	}

	bExecutionBlocked = false;
	ExecutionBlockReason.Reset();

	const uint32 Resolution = aqueduct_graph_resolution(Graph);
	// Bounded one-shot mode:
	// over budget, execute in the plan's memory-aware order, aim
	// intermediate writes at the reuse pool, and release each texture the
	// step after its last consumer (RHI defers actual destruction past
	// in-flight command lists). Below budget nothing changes.
	const bool bBounded = ShouldEngageBounded() && OneShotPlan.IsValid();
	if (bBounded)
	{
		PoolRemap.Reset();
	}
	UE_LOG(LogAqueduct, Log, TEXT("AqueductBounded engaged=%d predicted=%llu pool=%d"),
		bBounded ? 1 : 0, PredictedResidentBytes, OneShotPlan.Pool.Num());
	if (bBounded)
	{
		CreateMippedPoolSlots(OneShotPlan, TEXT("_pool_"), GetResolution());
		PoolRemap.Reset();
		for (const auto& A : OneShotPlan.Assign)
		{
			// Pool only identity-stable keys. Multipass finalization
			// REPLACES its output's NodeTextures entry (the write-copy
			// dance), which is incompatible with a shared pool slot; those
			// intermediates stay real textures and free after their last
			// consumer instead.
			const FString ProducerId = A.Key.Contains(TEXT("."))
				? A.Key.Left(A.Key.Find(TEXT(".")))
				: A.Key;
			const FExecutionPlanNode* Producer = PlanNodes.Find(ProducerId);
			const bool bIdentityStable = Producer && Producer->Passes <= 1;
			if (bIdentityStable)
			{
				const FString PoolKey = FString::Printf(TEXT("_pool_%d"), A.Value);
				PoolRemap.Add(A.Key, PoolKey);
				PoolRemap.Add(RuntimeTextureKey(A.Key), PoolKey);
			}
		}
	}
	const TArray<FString>& ExecutionOrder = bBounded ? OneShotPlan.Order : PlanOrder;

	// Formats and compute-texture allocation come straight from the plan.
	ResolvedFormats.Empty();
	for (const FString& NodeId : ExecutionOrder)
	{
		const FExecutionPlanNode& Plan = PlanNodes[NodeId];
		ResolvedFormats.Add(NodeId, Plan.Format);

		const FNodeShaderData* ShaderData = bShadersCompiled ? NodeShaders.Find(NodeId) : nullptr;
		if (ShaderData && ShaderData->OutputSemantics.Num() > 1)
		{
			for (const FString& OutSem : ShaderData->OutputSemantics)
			{
				const FString TextureKey = NodeId + TEXT(".") + OutSem;
				GetOrCreateNodeTexture(TextureKey, Plan.ComputeFormat, Resolution,
					MipRequiredTextureKeys.Contains(TextureKey));
			}
		}
		else
		{
			GetOrCreateNodeTexture(NodeId, Plan.ComputeFormat, Resolution,
				MipRequiredTextureKeys.Contains(NodeId));
		}

		// The shared multi-pass ping/pong pair holds rgba32float intermediates;
		// a format-convert blit lands each node's final format afterwards.
		if (ShaderData && ShaderData->Passes > 1)
		{
			GetOrCreateNodeTexture(GSharedPingKey, TEXT("rgba32float"), Resolution);
			GetOrCreateNodeTexture(GSharedPongKey, TEXT("rgba32float"), Resolution);
		}
	}

	// Bitmap source pre-pass (correctness contract):
	// a graph render either uses the exact bytes its sources name or it
	// produces NO output. Every bitmap source is resolved and uploaded as
	// the node's host texture ({NodeId}.__host_source) BEFORE any dispatch;
	// if any source is unresolvable (asset not in the project store yet,
	// unreadable file, decode failure), execution BLOCKS: nothing is
	// dispatched, nothing is delivered, nothing can bake. The editor module
	// downloads missing assets and reloads. The single defined exception:
	// an EMPTY source is a defined input state that renders transparent.
	for (const FString& NodeId : ExecutionOrder)
	{
		if (PlanNodes[NodeId].Kind != TEXT("bitmap"))
		{
			continue;
		}
		FTCHARToUTF8 NodeIdUtf8(*NodeId);
		const TSharedPtr<FJsonObject> NObj =
			FfiJsonObject(aqueduct_graph_get_node_json(Graph, NodeIdUtf8.Get()));
		if (!NObj.IsValid())
		{
			continue;
		}

		// User content: non-throwing reads.
		FString Source;
		const TSharedPtr<FJsonObject>* ParamsObj;
		if (NObj->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			(*ParamsObj)->TryGetStringField(TEXT("source"), Source);
		}
		Source.TrimStartAndEndInline();

		const FString HostKey = NodeId + TEXT(".__host_source");
		const FString* CachedSource = BitmapSourceCache.Find(NodeId);
		if (CachedSource && *CachedSource == Source && NodeTextures.Contains(HostKey))
		{
			continue;
		}

		AqueductBitmapData* BmpData = nullptr;
		if (!Source.IsEmpty())
		{
			TArray<uint8> SourceBytes;
			FString LoadError;
			// Local list: missing-URL bookkeeping is the game thread's job
			// (CheckBitmapSourcesResolvable); this render-thread pre-pass is
			// the enforcement backstop and must not touch GT state.
			TArray<FString> LocalMissing;
			if (!AqueductAssets::LoadSourceBytes(
					Source, GraphDirectory, SourceBytes, LoadError, LocalMissing))
			{
				UE_LOG(LogAqueduct, Warning,
					TEXT("Bitmap node %s: %s — graph execution blocked"), *NodeId, *LoadError);
				bExecutionBlocked = true;
				if (ExecutionBlockReason.IsEmpty()) ExecutionBlockReason = LoadError;
				continue; // keep scanning: collect ALL missing URLs for parallel download
			}
			BmpData = aqueduct_bitmap_decode(SourceBytes.GetData(), SourceBytes.Num());
			if (!BmpData)
			{
				const FString DecodeError = FString::Printf(
					TEXT("bitmap decode failed for '%s': %s"), *Source, *GetLastError());
				UE_LOG(LogAqueduct, Error,
					TEXT("Bitmap node %s: %s — graph execution blocked"), *NodeId, *DecodeError);
				bExecutionBlocked = true;
				if (ExecutionBlockReason.IsEmpty()) ExecutionBlockReason = DecodeError;
				continue;
			}
		}

		// Empty source: defined transparent value. Resolved source: exact pixels.
		const uint8 TransparentPixel[4] = { 0, 0, 0, 0 };
		uint32 SrcWidth = 1;
		uint32 SrcHeight = 1;
		uint32 SrcBytesPerRow = 4;
		const uint8* SrcPixels = TransparentPixel;
		FString FormatStr = TEXT("rgba8unorm");
		if (BmpData)
		{
			SrcWidth = BmpData->width;
			SrcHeight = BmpData->height;
			SrcBytesPerRow = BmpData->bytes_per_row;
			SrcPixels = BmpData->pixels;
			FormatStr = UTF8_TO_TCHAR(BmpData->format);
		}

		EPixelFormat PixelFormat = AqueductFormatToPixelFormat(FormatStr);
		const FRHITextureCreateDesc TexDesc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductBitmapSource"))
			.SetExtent(SrcWidth, SrcHeight)
			.SetFormat(PixelFormat)
			.SetFlags(ETextureCreateFlags::ShaderResource)
			.SetInitialState(ERHIAccess::SRVCompute);

		FTextureRHIRef Tex = RHICreateTexture(TexDesc);
		if (Tex.IsValid())
		{
			uint32 Stride = 0;
			void* MappedData = RHICmdList.LockTexture2D(Tex, 0, RLM_WriteOnly, Stride, false);
			if (MappedData)
			{
				// Clamp to the mapped stride: bytes_per_row comes from a
				// user-supplied image file via FFI — never trust it for
				// the destination write size.
				const uint32 CopyBytes = FMath::Min(SrcBytesPerRow, Stride);
				const uint8* SrcRow = SrcPixels;
				uint8* DstRow = static_cast<uint8*>(MappedData);
				for (uint32 Row = 0; Row < SrcHeight; Row++)
				{
					FMemory::Memcpy(DstRow, SrcRow, CopyBytes);
					SrcRow += SrcBytesPerRow;
					DstRow += Stride;
				}
				RHICmdList.UnlockTexture2D(Tex, 0, false);
			}

			FAqueductTexture AqTex;
			AqTex.Texture = Tex;
			AqTex.FormatStr = FormatStr;
			AqTex.Resolution = SrcWidth;
			NodeTextures.Add(HostKey, MoveTemp(AqTex));
			BitmapSourceCache.Add(NodeId, Source);
			UE_LOG(LogAqueduct, Log, TEXT("Bitmap node %s: source '%s' (%dx%d %s)"),
				*NodeId, *Source, SrcWidth, SrcHeight, *FormatStr);
		}

		if (BmpData) aqueduct_bitmap_free(BmpData);
	}

	if (bExecutionBlocked)
	{
		UE_LOG(LogAqueduct, Warning,
			TEXT("Graph execution blocked, nothing dispatched: %s"), *ExecutionBlockReason);
		return;
	}

	UE_LOG(LogAqueduct, Log, TEXT("Dispatching %d nodes..."), ExecutionOrder.Num());
	int32 SkippedNodes = 0;
	for (int32 Idx = 0; Idx < ExecutionOrder.Num(); Idx++)
	{
		const FString& NodeId = ExecutionOrder[Idx];

	const FNodeShaderData* ShaderData = bShadersCompiled ? NodeShaders.Find(NodeId) : nullptr;
		// Signature skip: same content, valid time, textures resident means
		// the dispatch cannot change any pixel. Never while bounded (pool
		// slots are not the node's persistent outputs).
		if (!bBounded && CanSkipNode(NodeId, ShaderData))
		{
			SkippedNodes++;
			continue;
		}
		UE_LOG(LogAqueduct, Log, TEXT("Dispatching node %d/%d: %s"), Idx + 1, ExecutionOrder.Num(), *NodeId);
		if (ShaderData)
		{
			if (ShaderData->Passes > 1)
			{
				DispatchMultiPassNode(RHICmdList, NodeId, *ShaderData, Resolution);
			}
			else
			{
				DispatchDynamicNode(RHICmdList, NodeId, *ShaderData, Resolution);
			}
		}
		else
		{
			DispatchTestNode(RHICmdList, NodeId, Resolution);
		}
		GenerateRequiredMipmapsForNode(RHICmdList, NodeId, Resolution);
		if (!bBounded)
		{
			if (const FString* Sig = NodeSigs.Find(NodeId))
			{
				LastRenderedSigs.Add(NodeId, *Sig);
				LastRenderedTimes.Add(NodeId, CurrentTime);
			}
		}
	
		// Bounded: unpooled textures (host sources) whose last consumer
		// just ran are released. Pooled keys stay: their physical slot is
		// reused by later steps and the whole pool releases at the end.
		if (bBounded && OneShotPlan.FreeAfter.IsValidIndex(Idx))
		{
			for (const FString& DeadKey : OneShotPlan.FreeAfter[Idx])
			{
				// Unpooled keys (multipass intermediates, host sources)
				// free for real; pooled keys keep their slot for reuse.
				if (!PoolRemap.Contains(DeadKey))
				{
					NodeTextures.Remove(DeadKey);
				}
			}
		}
	}
	LastFullDispatchedNodes = ExecutionOrder.Num() - SkippedNodes;
	LastFullSkippedNodes = SkippedNodes;
	UE_LOG(LogAqueduct, Log, TEXT("AqueductExecuted dispatched=%d skipped=%d"),
		LastFullDispatchedNodes, LastFullSkippedNodes);

	// Graph outputs come pre-resolved to texture keys in the plan. Outputs
	// whose node resolves to rgba8unorm deliver the blue-noise-dithered
	// conversion of the (wide) compute texture instead.
	OutputTextures.Empty();
	for (const auto& Output : PlanOutputs)
	{
		const FString& TextureKey = Output.Value;
		FString NodeId = TextureKey;
		int32 DotIdx;
		if (NodeId.FindChar(TEXT('.'), DotIdx))
		{
			NodeId = NodeId.Left(DotIdx);
		}
		const FExecutionPlanNode* Plan = PlanNodes.Find(NodeId);
		if (Plan && Plan->Format == TEXT("rgba8unorm"))
		{
			const FString ConvKey = TEXT("_dither_") + TextureKey;
			GetOrCreateNodeTexture(ConvKey, TEXT("rgba8unorm"), Resolution);
			DispatchDitherConversion(RHICmdList, TextureKey, ConvKey, Resolution);
			OutputTextures.Add(Output.Key, GetNodeTexture(ConvKey));
			// Bounded: the wide compute texture is dead once the dithered
			// delivery copy exists (nothing downstream reads targets).
			if (bBounded)
			{
				NodeTextures.Remove(ResolvePoolKey(TextureKey));
			}
			continue;
		}
		if (FTextureRHIRef Tex = GetNodeTexture(TextureKey))
		{
			OutputTextures.Add(Output.Key, Tex);
		}
	}

	// Bounded teardown: the delivered outputs hold their own RHI refs in
	// OutputTextures; the pool and remap end with the render.
	if (bBounded)
	{
		PoolRemap.Reset();
		for (int32 Slot = 0; Slot < OneShotPlan.Pool.Num(); Slot++)
		{
			NodeTextures.Remove(FString::Printf(TEXT("_pool_%d"), Slot));
		}
	}

	UE_LOG(LogAqueduct, Verbose, TEXT("Graph execution complete: %d textures, %d outputs"),
		NodeTextures.Num(), OutputTextures.Num());
}

// ---------------------------------------------------------------------------
// Placeholder texture for SRV slots the shader never reads
// ---------------------------------------------------------------------------

FShaderResourceViewRHIRef FAqueductGraphExecutor::GetPlaceholderSRV(FRHICommandListImmediate& RHICmdList)
{
	if (PlaceholderSRV.IsValid())
	{
		return PlaceholderSRV;
	}

	// 1x1 R8G8B8A8 white texture, bound only to SRV slots the shader never
	// reads (unconnected declared inputs bind GetDefaultColorSRV instead).
	// Use initial data instead of UAV clear, because ClearUAVFloat
	// would set a different pipeline state and break any in-flight compute dispatch.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("AqueductPlaceholder"))
		.SetExtent(1, 1)
		.SetFormat(PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVCompute);

	PlaceholderTexture = RHICreateTexture(Desc);

	// Upload white pixel data
	FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
	uint8 WhitePixel[4] = { 255, 255, 255, 255 };
	RHIUpdateTexture2D(PlaceholderTexture, 0, Region, 4, WhitePixel);

	RHICmdList.Transition(FRHITransitionInfo(PlaceholderTexture, ERHIAccess::Unknown, ERHIAccess::SRVCompute));

	PlaceholderSRV = RHICmdList.CreateShaderResourceView(
		PlaceholderTexture, FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));

	return PlaceholderSRV;
}

FShaderResourceViewRHIRef FAqueductGraphExecutor::GetDefaultColorSRV(
	FRHICommandListImmediate& RHICmdList, const FLinearColor& Color, uint32 Resolution)
{
	// Node shaders Load() texels at output-pixel coordinates, so a non-zero
	// default color needs real resolution-sized texels; a 1x1 texture would
	// read out-of-bounds (zero) everywhere except (0,0). The zero color can
	// stay 1x1 because out-of-bounds loads read as zero.
	const bool bZeroColor = Color.R == 0.f && Color.G == 0.f && Color.B == 0.f && Color.A == 0.f;
	const uint32 Size = bZeroColor ? 1 : FMath::Max<uint32>(Resolution, 1);

	const FString Key = FString::Printf(TEXT("%g_%g_%g_%g_%u"), Color.R, Color.G, Color.B, Color.A, Size);
	if (const TPair<FTextureRHIRef, FShaderResourceViewRHIRef>* Found = DefaultColorSRVs.Find(Key))
	{
		return Found->Value;
	}

	// RGBA16F initialized with the color: matches the Rust engine's
	// unconnected-input default textures (create_default_texture /
	// create_solid_texture). Uploaded as initial data like the placeholder
	// above, for the same reason: ClearUAVFloat would disturb in-flight
	// compute pipeline state.
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("AqueductInputDefault"))
		.SetExtent(Size, Size)
		.SetFormat(PF_FloatRGBA)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVCompute);

	FTextureRHIRef Texture = RHICreateTexture(Desc);

	const FFloat16 Pixel[4] = {
		FFloat16(Color.R), FFloat16(Color.G), FFloat16(Color.B), FFloat16(Color.A)
	};
	TArray<FFloat16> Data;
	Data.SetNumUninitialized((int32)(Size * Size * 4));
	for (uint32 i = 0; i < Size * Size; i++)
	{
		FMemory::Memcpy(Data.GetData() + i * 4, Pixel, sizeof(Pixel));
	}
	FUpdateTextureRegion2D Region(0, 0, 0, 0, Size, Size);
	RHIUpdateTexture2D(Texture, 0, Region, Size * sizeof(Pixel), reinterpret_cast<const uint8*>(Data.GetData()));

	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::SRVCompute));

	FShaderResourceViewRHIRef SRV = RHICmdList.CreateShaderResourceView(
		Texture, FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));
	DefaultColorSRVs.Add(Key, TPair<FTextureRHIRef, FShaderResourceViewRHIRef>(Texture, SRV));
	return SRV;
}

// ---------------------------------------------------------------------------
// Blue noise texture for dithered rgba8 conversion
// ---------------------------------------------------------------------------

FShaderResourceViewRHIRef FAqueductGraphExecutor::GetBlueNoiseSRV(FRHICommandListImmediate& RHICmdList)
{
	if (BlueNoiseSRV.IsValid()) return BlueNoiseSRV;

	// 64x64 RGBA8 blue noise embedded in the Rust library, so the plugin
	// works identically wherever it is installed and in packaged games.
	uint32 DataLen = 0;
	const uint8* Data = aqueduct_blue_noise_64(&DataLen);
	check(Data != nullptr && DataLen == 64 * 64 * 4);

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("AqueductBlueNoise"))
		.SetExtent(64, 64)
		.SetFormat(PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVCompute);
	BlueNoiseTexture = RHICreateTexture(Desc);

	FUpdateTextureRegion2D Region(0, 0, 0, 0, 64, 64);
	RHIUpdateTexture2D(BlueNoiseTexture, 0, Region, 64 * 4, Data);

	BlueNoiseSRV = RHICmdList.CreateShaderResourceView(
		BlueNoiseTexture, FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));

	return BlueNoiseSRV;
}

// ---------------------------------------------------------------------------
// Scratch storage buffers
// ---------------------------------------------------------------------------
//
// Node-internal GPU buffers for atomics, histograms, sorted lists, etc.
// Allocated on first dispatch that binds them; zero-filled at the start of
// each node execution; persist across passes of a single node run.
//
// Every compiled FAqueductDynamicCS permutation has 8 AQ_ScratchN slots;
// unused slots bind the 1-element placeholder UAV to satisfy the RHI.

FUnorderedAccessViewRHIRef FAqueductGraphExecutor::GetScratchPlaceholderUAV(FRHICommandListImmediate& RHICmdList)
{
	if (ScratchPlaceholderUAV.IsValid())
	{
		return ScratchPlaceholderUAV;
	}

	FRHIBufferCreateDesc BufDesc =
		FRHIBufferCreateDesc::CreateStructured<uint32>(TEXT("AqueductScratchPlaceholder"), 1)
		.AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
		.SetInitialState(ERHIAccess::UAVCompute);

	ScratchPlaceholderBuffer = RHICmdList.CreateBuffer(BufDesc);
	ScratchPlaceholderUAV = RHICmdList.CreateUnorderedAccessView(
		ScratchPlaceholderBuffer,
		FRHIViewDesc::CreateBufferUAV()
			.SetType(FRHIViewDesc::EBufferType::Structured)
			.SetStride(sizeof(uint32))
			.SetNumElements(1));
	return ScratchPlaceholderUAV;
}

TArray<FAqueductGraphExecutor::FScratchDeclInfo> FAqueductGraphExecutor::ResolveScratchDecls(
	const FString& NodeId, const FNodeShaderData& ShaderData) const
{
	TArray<FScratchDeclInfo> Out;
	Out.SetNum(FMath::Min(ShaderData.ScratchSemantics.Num(), MAX_SCRATCH_SLOTS));
	const FExecutionPlanNode* Plan = PlanNodes.Find(NodeId);
	if (!Plan)
	{
		return Out;
	}
	for (int32 Slot = 0; Slot < Out.Num(); Slot++)
	{
		if (const FScratchDeclInfo* Decl = Plan->Scratch.Find(ShaderData.ScratchSemantics[Slot]))
		{
			Out[Slot] = *Decl;
		}
	}
	return Out;
}

FUnorderedAccessViewRHIRef FAqueductGraphExecutor::GetSharedScratchUAV(
	FRHICommandListImmediate& RHICmdList, int32 Slot, uint32 Stride, uint32 Count)
{
	Stride = FMath::Max<uint32>(4u, Stride);
	Count = FMath::Max<uint32>(1u, Count);
	const uint32 PoolKey = (uint32(Slot) << 8) | Stride;
	FSharedScratchSlot& Entry = SharedScratch.FindOrAdd(PoolKey);

	const uint32 NeededBytes = Count * Stride;
	if (!Entry.Buffer.IsValid() || Entry.SizeBytes < NeededBytes || Entry.Stride != Stride)
	{
		// Grow-only: a recreate orphans every cached view, so this only
		// happens before any compiled snapshot exists for the new topology
		// (the full execute that precedes BuildCompiledGraph walks every
		// node and grows each slot to its graph-wide maximum).
		const FString DebugName = FString::Printf(TEXT("_shared.scratch.%d.s%u"), Slot, Stride);
		FRHIBufferCreateDesc BufDesc =
			FRHIBufferCreateDesc::Create(*DebugName, NeededBytes, Stride, EBufferUsageFlags::StructuredBuffer)
			.AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
			.SetInitialState(ERHIAccess::UAVCompute);
		Entry.Buffer = RHICmdList.CreateBuffer(BufDesc);
		Entry.SizeBytes = NeededBytes;
		Entry.Stride = Stride;
		Entry.Views.Empty();
	}

	if (const FUnorderedAccessViewRHIRef* Existing = Entry.Views.Find(Count))
	{
		return *Existing;
	}
	FUnorderedAccessViewRHIRef View = RHICmdList.CreateUnorderedAccessView(
		Entry.Buffer,
		FRHIViewDesc::CreateBufferUAV()
			.SetType(FRHIViewDesc::EBufferType::Structured)
			.SetStride(Stride)
			.SetNumElements(Count));
	Entry.Views.Add(Count, View);
	return View;
}

void FAqueductGraphExecutor::ZeroFillScratchUAV(
	FRHICommandListImmediate& RHICmdList, const FUnorderedAccessViewRHIRef& UAV)
{
	if (!UAV.IsValid())
	{
		return;
	}
	RHICmdList.Transition(FRHITransitionInfo(UAV, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.ClearUAVUint(UAV, FUintVector4(0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// Dithered rgba8 conversion
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::DispatchDitherConversion(
	FRHICommandListImmediate& RHICmdList,
	const FString& SourceKey,
	const FString& DestKey,
	uint32 Resolution)
{
	FAqueductTexture* SrcEntry = NodeTextures.Find(ResolvePoolKey(SourceKey));
	FAqueductTexture* DstEntry = NodeTextures.Find(ResolvePoolKey(DestKey));
	if (!ensure(SrcEntry) || !ensure(DstEntry)) return;

	TShaderMapRef<FAqueductDitherCS> DitherShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ensure(DitherShader.IsValid())) return;

	FShaderResourceViewRHIRef SrcSRV = RHICmdList.CreateShaderResourceView(
		SrcEntry->Texture, FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));

	FUnorderedAccessViewRHIRef DstUAV = RHICmdList.CreateUnorderedAccessView(
		DstEntry->Texture, FRHIViewDesc::CreateTextureUAV()
			.SetDimension(ETextureDimension::Texture2D));

	FShaderResourceViewRHIRef NoiseSRV = GetBlueNoiseSRV(RHICmdList);

	RHICmdList.Transition(FRHITransitionInfo(DstEntry->Texture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FAqueductDitherCS::FParameters PassParameters;
	PassParameters.InputTexture = SrcSRV;
	PassParameters.OutputTexture = DstUAV;
	PassParameters.BlueNoiseTexture = NoiseSRV;

	const uint32 GroupCount = FMath::DivideAndRoundUp(Resolution, (uint32)WORKGROUP_SIZE);
	FComputeShaderUtils::Dispatch(RHICmdList, DitherShader, PassParameters,
		FIntVector(GroupCount, GroupCount, 1));

	RHICmdList.Transition(FRHITransitionInfo(DstEntry->Texture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FAqueductGraphExecutor::GenerateMipmapsInPlace(
	FRHICommandListImmediate& RHICmdList,
	const FTextureRHIRef& Texture,
	uint32 Resolution) const
{
	if (!Texture.IsValid() || Texture->GetNumMips() <= 1)
	{
		return;
	}

	const EPixelFormat Format = Texture->GetFormat();
	const uint32 MipCount = Texture->GetNumMips();

	uint32 MipW = Resolution;
	uint32 MipH = Resolution;
	for (uint32 Level = 1; Level < MipCount; Level++)
	{
		MipW = FMath::Max(MipW / 2u, 1u);
		MipH = FMath::Max(MipH / 2u, 1u);

		RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

		if (Format == PF_R32_FLOAT)
		{
			TShaderMapRef<FAqueductMipGenR32CS> MipShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FShaderResourceViewRHIRef SrcSRV = RHICmdList.CreateShaderResourceView(
				Texture, FRHIViewDesc::CreateTextureSRV()
					.SetDimension(ETextureDimension::Texture2D)
					.SetMipRange(Level - 1, 1));
			FUnorderedAccessViewRHIRef DstUAV = RHICmdList.CreateUnorderedAccessView(
				Texture, FRHIViewDesc::CreateTextureUAV()
					.SetDimension(ETextureDimension::Texture2D)
					.SetMipLevel(Level));

			FAqueductMipGenR32CS::FParameters MipParams;
			MipParams.SrcMipR32 = SrcSRV;
			MipParams.DstMipR32 = DstUAV;

			FComputeShaderUtils::Dispatch(RHICmdList, MipShader, MipParams,
				FIntVector(FMath::DivideAndRoundUp(MipW, 16u),
					FMath::DivideAndRoundUp(MipH, 16u), 1));
		}
		else
		{
			TShaderMapRef<FAqueductMipGenCS> MipShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FShaderResourceViewRHIRef SrcSRV = RHICmdList.CreateShaderResourceView(
				Texture, FRHIViewDesc::CreateTextureSRV()
					.SetDimension(ETextureDimension::Texture2D)
					.SetMipRange(Level - 1, 1));
			FUnorderedAccessViewRHIRef DstUAV = RHICmdList.CreateUnorderedAccessView(
				Texture, FRHIViewDesc::CreateTextureUAV()
					.SetDimension(ETextureDimension::Texture2D)
					.SetMipLevel(Level));

			FAqueductMipGenCS::FParameters MipParams;
			MipParams.SrcMip = SrcSRV;
			MipParams.DstMip = DstUAV;

			FComputeShaderUtils::Dispatch(RHICmdList, MipShader, MipParams,
				FIntVector(FMath::DivideAndRoundUp(MipW, 16u),
					FMath::DivideAndRoundUp(MipH, 16u), 1));
		}
	}

	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::SRVCompute));
}

void FAqueductGraphExecutor::GenerateRequiredMipmapsForNode(
	FRHICommandListImmediate& RHICmdList,
	const FString& NodeId,
	uint32 Resolution) const
{
	for (const FString& TextureKey : MipRequiredTextureKeys)
	{
		const bool bOwnedByNode = TextureKey == NodeId || TextureKey.StartsWith(NodeId + TEXT("."));
		if (bOwnedByNode)
		{
			GenerateMipmapsInPlace(RHICmdList, GetNodeTexture(TextureKey), Resolution);
		}
	}
}


// ---------------------------------------------------------------------------
// Dynamic node dispatch
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::DispatchDynamicNode(
	FRHICommandListImmediate& RHICmdList,
	const FString& NodeId,
	const FNodeShaderData& ShaderData,
	uint32 Resolution)
{
	FAqueductDynamicCS::FPermutationDomain PermDomain;
	PermDomain.Set<FAqueductDynamicCS::FNodeIndex>(ShaderData.PermutationIndex);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!GlobalShaderMap->HasShader(&FAqueductDynamicCS::GetStaticType(), PermDomain.ToDimensionValueId()))
	{
		UE_LOG(LogAqueduct, Error, TEXT("Shader not compiled for node %s (permutation %d)"),
			*NodeId, ShaderData.PermutationIndex);
		DispatchTestNode(RHICmdList, NodeId, Resolution);
		return;
	}

	TShaderRef<FAqueductDynamicCS> ComputeShader =
		GlobalShaderMap->GetShader<FAqueductDynamicCS>(PermDomain);

	FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();

	bool bMultiOutput = ShaderData.OutputSemantics.Num() > 1;
	FUnorderedAccessViewRHIRef OutputUAVs[MAX_NODE_OUTPUTS];

	for (int32 i = 0; i < ShaderData.OutputSemantics.Num() && i < MAX_NODE_OUTPUTS; i++)
	{
		FString TexKey = bMultiOutput
			? (NodeId + TEXT(".") + ShaderData.OutputSemantics[i])
			: NodeId;

		FAqueductTexture* Entry = NodeTextures.Find(ResolvePoolKey(TexKey));
		if (!Entry || !Entry->Texture.IsValid())
		{
			UE_LOG(LogAqueduct, Error, TEXT("No texture for node output %s"), *TexKey);
			return;
		}

		OutputUAVs[i] = RHICmdList.CreateUnorderedAccessView(
			Entry->Texture, FRHIViewDesc::CreateTextureUAV()
				.SetDimension(ETextureDimension::Texture2D)
				.SetMipLevel(0));
		RHICmdList.Transition(FRHITransitionInfo(Entry->Texture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	}

	// Resolve input connections and create SRVs BEFORE setting the main
	// pipeline, because R32Float expansion dispatches its own compute shader.
	FTCHARToUTF8 NodeIdUtf8(*NodeId);
	const FExecutionPlanNode* Plan = PlanNodes.Find(NodeId);

	FShaderResourceViewRHIRef InputSRVs[MAX_NODE_INPUTS];
	for (int32 i = 0; i < ShaderData.InputSemantics.Num() && i < MAX_NODE_INPUTS; i++)
	{
		const FString& Semantic = ShaderData.InputSemantics[i];

		// Multi-pass feedback: "ping" input reads from the ping-pong buffer
		if (Semantic == TEXT("ping"))
		{
			FTextureRHIRef FeedbackTex = GetNodeTexture(NodeId + TEXT("._ping_feedback"));
			if (FeedbackTex.IsValid())
			{
				InputSRVs[i] = RHICmdList.CreateShaderResourceView(
					FeedbackTex, FRHIViewDesc::CreateTextureSRV()
						.SetDimension(ETextureDimension::Texture2D));
			}
			continue;
		}

		// Host-provided textures (registry host_textures, e.g. the bitmap
		// node's decoded source image): bound from the executor-uploaded
		// texture, not graph wiring. Keyed exactly like the Rust engine.
		FTextureRHIRef HostTex = GetNodeTexture(NodeId + TEXT(".__host_") + Semantic);
		if (HostTex.IsValid())
		{
			InputSRVs[i] = RHICmdList.CreateShaderResourceView(
				HostTex, FRHIViewDesc::CreateTextureSRV()
					.SetDimension(ETextureDimension::Texture2D));
			continue;
		}

		const FString* SourceKey = Plan ? Plan->Inputs.Find(Semantic) : nullptr;
		if (!SourceKey)
		{
			continue;
		}
		FTextureRHIRef InputTex = GetNodeTexture(*SourceKey);
		if (!InputTex.IsValid())
		{
			continue;
		}
		// Semantic mip consumers bind the same producer texture as every
		// other consumer. Its full pyramid was generated in place immediately
		// after the producer dispatch.
		InputSRVs[i] = RHICmdList.CreateShaderResourceView(
			InputTex, FRHIViewDesc::CreateTextureSRV()
				.SetDimension(ETextureDimension::Texture2D));
	}

	// Now set the main node's compute pipeline (after all expansion dispatches).
	SetComputePipelineState(RHICmdList, ShaderRHI);

	FAqueductDynamicCS::FParameters PassParameters;
	if (OutputUAVs[0]) PassParameters.AQ_Output0 = OutputUAVs[0];
	if (OutputUAVs[1]) PassParameters.AQ_Output1 = OutputUAVs[1];
	if (OutputUAVs[2]) PassParameters.AQ_Output2 = OutputUAVs[2];
	if (OutputUAVs[3]) PassParameters.AQ_Output3 = OutputUAVs[3];

	// Unconnected/missing declared inputs bind a 1x1 default color: the node
	// definition's input_defaults entry, else transparent black (matches the
	// Rust engine's unconnected-input default).
	for (int32 i = 0; i < ShaderData.InputSemantics.Num() && i < MAX_NODE_INPUTS; i++)
	{
		if (InputSRVs[i]) continue;
		const FLinearColor* Override = ShaderData.InputDefaults.Find(ShaderData.InputSemantics[i]);
		InputSRVs[i] = GetDefaultColorSRV(RHICmdList, Override ? *Override : FLinearColor(0.f, 0.f, 0.f, 0.f), Resolution);
	}

	// SRV slots the shader never reads still need a bound descriptor to
	// prevent GPU hangs; fill them with the white placeholder.
	FShaderResourceViewRHIRef Placeholder = GetPlaceholderSRV(RHICmdList);
	for (int32 i = 0; i < MAX_NODE_INPUTS; i++)
	{
		if (!InputSRVs[i])
		{
			InputSRVs[i] = Placeholder;
		}
	}
	PassParameters.AQ_Input0  = InputSRVs[0];
	PassParameters.AQ_Input1  = InputSRVs[1];
	PassParameters.AQ_Input2  = InputSRVs[2];
	PassParameters.AQ_Input3  = InputSRVs[3];
	PassParameters.AQ_Input4  = InputSRVs[4];
	PassParameters.AQ_Input5  = InputSRVs[5];
	PassParameters.AQ_Input6  = InputSRVs[6];
	PassParameters.AQ_Input7  = InputSRVs[7];
	PassParameters.AQ_Input8  = InputSRVs[8];
	PassParameters.AQ_Input9  = InputSRVs[9];
	PassParameters.AQ_Input10 = InputSRVs[10];
	PassParameters.AQ_Input11 = InputSRVs[11];
	PassParameters.AQ_Input12 = InputSRVs[12];
	PassParameters.AQ_Input13 = InputSRVs[13];
	PassParameters.AQ_Input14 = InputSRVs[14];
	PassParameters.AQ_Input15 = InputSRVs[15];

	PassParameters.AQ_Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

	// Bind scratch buffers. Node's scratch declarations (from registry) tell us
	// name + element type + count formula. Each AQ_ScratchN slot binds the
	// SHARED pool buffer for that slot via a count-sized view, in the order
	// ScratchSemantics lists them. Unused slots get a placeholder.
	FUnorderedAccessViewRHIRef ScratchUAVs[MAX_SCRATCH_SLOTS];
	{
		const TArray<FScratchDeclInfo> Decls = ResolveScratchDecls(NodeId, ShaderData);
		for (int32 i = 0; i < Decls.Num(); i++)
		{
			ScratchUAVs[i] = GetSharedScratchUAV(RHICmdList, i, Decls[i].Stride, Decls[i].Count);
		}
	}
	FUnorderedAccessViewRHIRef ScratchPlaceholder = GetScratchPlaceholderUAV(RHICmdList);
	for (int32 i = 0; i < MAX_SCRATCH_SLOTS; i++)
	{
		if (!ScratchUAVs[i]) ScratchUAVs[i] = ScratchPlaceholder;
	}
	for (int32 i = 0; i < MAX_SCRATCH_SLOTS; i++)
	{
		BindScratchSlot(PassParameters, i, ScratchUAVs[i]);
	}

	// Pack scalar parameters into float4 array
	// Layout: AQ_Params[0].x = param0, [0].y = param1, ... — float4[16] = 64
	// scalars, enough for any node's params. Resolution is NOT stored here;
	// node shaders read it at runtime via GetDimensions, matching native/web.
	// Zero unused slots so stale values never leak in.
	for (int32 i = 0; i < MAX_PARAM_VECTORS; i++)
	{
		PassParameters.AQ_Params[i] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	}

	// Resolved params: graph values merged with definition defaults (custom
	// kinds resolve through their inline definitions inside Rust).
	const TSharedPtr<FJsonObject> ResolvedParams =
		FfiJsonObject(aqueduct_graph_get_node_resolved_params_json(Graph, Registry, NodeIdUtf8.Get()));

	{
		for (int32 i = 0; i < ShaderData.Params.Num() && i < MAX_PARAMS; i++)
		{
			const FString& ParamName = ShaderData.Params[i].Key;
			const FString& ParamType = ShaderData.Params[i].Value;
			int32 ArrayIdx = i / 4;
			int32 CompIdx = i % 4;

			float FloatVal = 0.0f;

			// Priority: override → expression → resolved param → 0
			FString OverrideKey = NodeId + TEXT(".") + ParamName;

			// Expression values are already in ParamValuesFloat (from EvaluateExpressions).
			// Numeric overrides from SetNodeParam are also there. Just check overrides → resolved.
			if (ParamType == TEXT("int"))
			{
				int32 IntVal = 0;
				if (const int32* Override = ParamValuesInt.Find(OverrideKey))
				{
					IntVal = *Override;
				}
				else if (const float* FloatOverride = ParamValuesFloat.Find(OverrideKey))
				{
					IntVal = (int32)*FloatOverride;
				}
				else if (ResolvedParams.IsValid())
				{
					double Num = 0;
					if (ResolvedParams->TryGetNumberField(ParamName, Num))
					{
						IntVal = (int32)Num;
					}
				}
				FloatVal = (float)IntVal;
			}
			else
			{
				if (const float* Override = ParamValuesFloat.Find(OverrideKey))
				{
					FloatVal = *Override;
				}
				else if (ResolvedParams.IsValid())
				{
					double Num = 0;
					if (ResolvedParams->TryGetNumberField(ParamName, Num))
					{
						FloatVal = (float)Num;
					}
				}
			}

			reinterpret_cast<float*>(&PassParameters.AQ_Params[ArrayIdx])[CompIdx] = FloatVal;
		}
	}

	// Bind and dispatch (canonical helper: binds AND unbinds — see the
	// format-convert site for why the raw batched path is forbidden).
	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Binding parameters..."), *NodeId);

	uint32 GroupCount = FMath::DivideAndRoundUp(Resolution, (uint32)WORKGROUP_SIZE);
	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Dispatching %dx%d groups..."), *NodeId, GroupCount, GroupCount);
	FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, PassParameters,
		FIntVector(GroupCount, GroupCount, 1));

	// No GPU flush here: the UAV→SRV transition below is the hazard barrier that
	// orders this node's writes before any downstream node samples it (UE's
	// tracked transitions insert the GPU-side barrier). A per-node
	// SubmitCommandsAndFlushGPU + BlockUntilGPUIdle stalls the GPU for no
	// correctness gain — the compiled-replay path relies on the same transition
	// alone, and CPU readback paths issue their own flush where they need one.

	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Transitioning to SRV..."), *NodeId);
	for (int32 i = 0; i < ShaderData.OutputSemantics.Num() && i < MAX_NODE_OUTPUTS; i++)
	{
		FString TexKey = bMultiOutput
			? (NodeId + TEXT(".") + ShaderData.OutputSemantics[i])
			: NodeId;
		FAqueductTexture* OutEntry = NodeTextures.Find(ResolvePoolKey(TexKey));
		if (OutEntry && OutEntry->Texture.IsValid())
		{
			RHICmdList.Transition(FRHITransitionInfo(OutEntry->Texture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		}
	}

	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Dispatch complete (perm %d, %d inputs, %d outputs, %d params)"),
		*NodeId, ShaderData.PermutationIndex,
		ShaderData.InputSemantics.Num(), ShaderData.OutputSemantics.Num(), ShaderData.Params.Num());
}

// ---------------------------------------------------------------------------
// Multi-pass node dispatch with ping-pong buffers
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::DispatchMultiPassNode(
	FRHICommandListImmediate& RHICmdList,
	const FString& NodeId,
	const FNodeShaderData& ShaderData,
	uint32 Resolution)
{
	uint32 TotalPasses = ShaderData.Passes;
	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Multi-pass: %d passes"), *NodeId, TotalPasses);

	FString PingKey = GSharedPingKey;
	FString PongKey = GSharedPongKey;
	FString PrimaryOutputKey = NodeId;
	if (ShaderData.OutputSemantics.Num() > 1 && ShaderData.OutputSemantics.Num() > 0)
	{
		PrimaryOutputKey = NodeId + TEXT(".") + ShaderData.OutputSemantics[0];
	}

	// Zero-fill all shared scratch slots this node declares before the first
	// pass. Slots persist across passes within this node's execution but each
	// node-execution starts from a clean zero state (the slots are shared, so
	// they hold the previous node's garbage otherwise).
	{
		const TArray<FScratchDeclInfo> Decls = ResolveScratchDecls(NodeId, ShaderData);
		for (int32 i = 0; i < Decls.Num(); i++)
		{
			ZeroFillScratchUAV(RHICmdList,
				GetSharedScratchUAV(RHICmdList, i, Decls[i].Stride, Decls[i].Count));
		}
	}

	// Set pass_index and total_passes as param overrides (read by shader via AQ_Params)
	FString PassIndexOverride = NodeId + TEXT(".pass_index");
	FString TotalPassesOverride = NodeId + TEXT(".total_passes");
	ParamValuesInt.Add(TotalPassesOverride, (int32)TotalPasses);

	// All passes run in rgba32float ping-pong buffers. Multi-pass algorithms (JFA,
	// separable blur, etc.) need 4 channels and full precision for intermediate data.
	// Node implementers don't need to know about this — the engine appends a transparent
	// format conversion copy after all passes complete.
	for (uint32 Pass = 0; Pass < TotalPasses; Pass++)
	{
		// All passes write to ping/pong — NOT to the output texture.
		FString ReadKey;   // "ping" input reads from this
		FString WriteKey;  // "output" writes to this

		if (Pass == 0)
		{
				ReadKey = FString();  // empty = use placeholder
			WriteKey = PingKey;
		}
		else if (Pass % 2 == 1)
		{
			ReadKey = PingKey;
			WriteKey = PongKey;
		}
		else
		{
			ReadKey = PongKey;
			WriteKey = PingKey;
		}

		ParamValuesInt.Add(PassIndexOverride, (int32)Pass);

		// Temporarily swap the node's output texture to the write target
		// and set up the ping input to read from the correct buffer.
		// VALUE COPIES before every Add: inserting `*PointerIntoThisMap`
		// into the same map is a use-after-free — the insert can rehash and
		// move the pointee before the value is copied.
		FAqueductTexture WriteCopy;
		if (const FAqueductTexture* WriteEntry = NodeTextures.Find(ResolvePoolKey(WriteKey)))
		{
			WriteCopy = *WriteEntry;
		}
		else
		{
			UE_LOG(LogAqueduct, Error, TEXT("  [%s] Missing write texture %s for pass %d"), *NodeId, *WriteKey, Pass);
			break;
		}

		// Save original primary output texture and swap write target into that
		// slot. Multi-output nodes store their primary output as "node.output",
		// not "node", so routing must use the first output semantic.
		FAqueductTexture SavedOutput;
		bool bHadOriginal = false;
		if (const FAqueductTexture* OrigPtr = NodeTextures.Find(ResolvePoolKey(PrimaryOutputKey)))
		{
			SavedOutput = *OrigPtr;
			bHadOriginal = true;
		}
		NodeTextures.Add(ResolvePoolKey(PrimaryOutputKey), WriteCopy);

		// Set up the "ping" feedback input (read from previous pass)
		FString PingInputKey = NodeId + TEXT("._ping_feedback");
		if (!ReadKey.IsEmpty())
		{
			FAqueductTexture ReadCopy;
			bool bHasRead = false;
			if (const FAqueductTexture* ReadEntry = NodeTextures.Find(ReadKey))
			{
				ReadCopy = *ReadEntry;
				bHasRead = true;
			}
			if (bHasRead)
			{
				NodeTextures.Add(PingInputKey, ReadCopy);
			}
		}

		// Dispatch the shader for this pass.
		// DispatchDynamicNode handles all transitions (UAVCompute→SRVCompute).
		DispatchDynamicNode(RHICmdList, NodeId, ShaderData, Resolution);

		// Restore original output texture
		if (bHadOriginal)
		{
			NodeTextures.Add(PrimaryOutputKey, SavedOutput);
		}

		NodeTextures.Remove(PingInputKey);
	}

	ParamValuesInt.Remove(PassIndexOverride);
	ParamValuesInt.Remove(TotalPassesOverride);

	// Format conversion: compute-shader blit from rgba32float ping/pong to the node's
	// declared output format. RHICmdList.CopyTexture is a hardware copy that requires
	// matching formats — using FAqueductBlitCS makes the hardware do the float conversion
	// on write.
	FString LastWrittenKey = (TotalPasses % 2 == 0) ? PongKey : PingKey;
	FAqueductTexture* SrcEntry = NodeTextures.Find(LastWrittenKey);
	FAqueductTexture* DstEntry = NodeTextures.Find(PrimaryOutputKey);
	if (SrcEntry && DstEntry && SrcEntry->Texture.IsValid() && DstEntry->Texture.IsValid())
	{
		DispatchFormatConvert(RHICmdList, SrcEntry->Texture, DstEntry->Texture, Resolution);
	}

	UE_LOG(LogAqueduct, Log, TEXT("  [%s] Multi-pass complete (%d passes + format convert)"), *NodeId, TotalPasses);
}

// ---------------------------------------------------------------------------
// Fallback: static test shader dispatch
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::DispatchTestNode(
	FRHICommandListImmediate& RHICmdList,
	const FString& NodeId,
	uint32 Resolution)
{
	FAqueductTexture* Entry = NodeTextures.Find(NodeId);
	if (!Entry || !Entry->Texture.IsValid())
	{
		UE_LOG(LogAqueduct, Error, TEXT("No texture for node %s"), *NodeId);
		return;
	}

	TShaderMapRef<FAqueductTestCS> TestShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!TestShader.IsValid())
	{
		UE_LOG(LogAqueduct, Error, TEXT("FAqueductTestCS not compiled"));
		return;
	}

	FUnorderedAccessViewRHIRef UAV = RHICmdList.CreateUnorderedAccessView(
		Entry->Texture, FRHIViewDesc::CreateTextureUAV()
			.SetDimension(ETextureDimension::Texture2D)
			.SetMipLevel(0));

	RHICmdList.Transition(FRHITransitionInfo(Entry->Texture, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FAqueductTestCS::FParameters PassParameters;
	PassParameters.OutputTexture = UAV;

	const uint32 GroupCount = FMath::DivideAndRoundUp(Resolution, (uint32)WORKGROUP_SIZE);
	FComputeShaderUtils::Dispatch(RHICmdList, TestShader, PassParameters,
		FIntVector(GroupCount, GroupCount, 1));

	RHICmdList.Transition(FRHITransitionInfo(Entry->Texture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	UE_LOG(LogAqueduct, Verbose, TEXT("Dispatched test shader for %s: %dx%d"),
		*NodeId, Resolution, Resolution);
}


} // namespace UE::Aqueduct
