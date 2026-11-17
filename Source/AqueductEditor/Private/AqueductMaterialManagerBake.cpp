// AqueductMaterialManager bake pipeline: GPU readback and package persistence.

#include "AqueductMaterialManager.h"
#include "AqueductMaterialManagerShared.h"
#include "AqueductTexture.h"
#include "AqueductAssets.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "AqueductGraphAsset.h"
#include "AqueductShader.h"
#include "AqueductPaths.h"
#include "AqueductTextureWriter.h"
#include "AqueductLiveTextureResource.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"
#include "Misc/App.h"
#include "Misc/SecureHash.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "MaterialDomain.h"
#include "Engine/Texture2D.h"
#include "TextureCompiler.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Containers/Ticker.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Editor.h"
#include "UObject/ObjectSaveContext.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductMaterial, Log, All);

// ---------------------------------------------------------------------------
// Bake: persist GPU outputs into the texture assets (the only readback path)
//
// All pixel conversion happens on the GPU (blit to the texture-source layout
// before readback); the CPU only row-compacts. The interactive path is fully
// staged across ticks — async readback polled, async texture builds, one
// package save per tick — so the game thread never waits on anything.
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::EnqueueBakeCopiesRT(
	FRHICommandListImmediate& RHICmdList, TArray<FPendingBakeOutput>& Targets,
	TMap<FString, AqueductTextureWriter::FDeliveryScratch>* Scratch)
{
	for (FPendingBakeOutput& Target : Targets)
	{
		// The delivery chain already produced the quantized bytes on the
		// GPU; the bake just reads its packed mip 0. No conversion, no
		// second quantization path.
		const AqueductTextureWriter::FDeliveryScratch* S =
			Scratch ? Scratch->Find(Target.Name) : nullptr;
		if (!S || !S->Packed.IsValid()
			|| S->Packed->GetSizeX() != Target.Resolution
			|| S->Packed->GetFormat() != Target.ReadbackPF)
		{
			UE_LOG(LogAqueductMaterial, Warning,
				TEXT("Bake readback: no packed delivery for %s (scratch %s)"),
				*Target.Name, S ? TEXT("format/size mismatch") : TEXT("missing"));
			continue;
		}

		RHICmdList.Transition(FRHITransitionInfo(S->Packed, ERHIAccess::Unknown, ERHIAccess::CopySrc));
		Target.Readback->EnqueueCopy(RHICmdList, S->Packed, FIntVector(0, 0, 0), 0,
			FIntVector(Target.Resolution, Target.Resolution, 1));
		RHICmdList.Transition(FRHITransitionInfo(S->Packed, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
	}
}

void FAqueductMaterialManager::EnqueueBakeReadbackCommands(
	TSharedRef<TArray<FPendingBakeOutput>> Targets,
	TMap<FString, AqueductTextureWriter::FDeliveryScratch>* Scratch, bool bBlocking)
{
	ENQUEUE_RENDER_COMMAND(AqueductBakeReadback)(
		[Targets, Scratch, bBlocking](FRHICommandListImmediate& RHICmdList)
	{
		EnqueueBakeCopiesRT(RHICmdList, Targets.Get(), Scratch);
		if (bBlocking)
		{
			RHICmdList.SubmitCommandsAndFlushGPU();
			RHICmdList.BlockUntilGPUIdle();
		}
	});
}

void FAqueductMaterialManager::CompactRowsInto(const FPendingBakeOutput& Target, uint8* Dest)
{
	const uint32 BytesPerPixel = GPixelFormats[Target.ReadbackPF].BlockBytes;
	const uint32 Res = Target.Resolution;
	const int64 RowBytes = (int64)Res * BytesPerPixel;
	const int64 SrcPitchBytes = (int64)Target.RowPitchInPixels * BytesPerPixel;

	if (Target.ReadbackPF == PF_R8G8B8A8)
	{
		// Packed RGBA bytes → TSF_BGRA8 source layout (R↔B swizzle).
		for (uint32 Y = 0; Y < Res; Y++)
		{
			const uint8* Src = static_cast<const uint8*>(Target.MappedRows) + (int64)Y * SrcPitchBytes;
			uint8* Dst = Dest + (int64)Y * RowBytes;
			for (uint32 X = 0; X < Res; X++)
			{
				Dst[X * 4 + 0] = Src[X * 4 + 2];
				Dst[X * 4 + 1] = Src[X * 4 + 1];
				Dst[X * 4 + 2] = Src[X * 4 + 0];
				Dst[X * 4 + 3] = Src[X * 4 + 3];
			}
		}
		return;
	}

	// PF_A16B16G16R16 → TSF_RGBA16 and PF_G8 → TSF_G8 are layout-identical.
	for (uint32 Y = 0; Y < Res; Y++)
	{
		FMemory::Memcpy(
			Dest + (int64)Y * RowBytes,
			static_cast<const uint8*>(Target.MappedRows) + (int64)Y * SrcPitchBytes,
			RowBytes);
	}
}

bool FAqueductMaterialManager::BakeGraph(const FString& GraphFilePath, bool bAllowUnattendedSave)
{
	check(IsInGameThread());

	const FString FullPath = FPaths::ConvertRelativePathToFull(GraphFilePath);

	// A live numeric edit updates the compiled uniforms and exact recipe file
	// without re-parsing on every slider frame. Reconcile once here so hashes,
	// parsed parameters, and the pixels being baked all describe that file.
	if (const FGraphState* Existing = GraphStates.Find(FullPath);
		Existing && Existing->CurrentHash.IsEmpty())
	{
		if (!ReloadGraphFromFile(FullPath)) return false;
	}

	if (!FindExecutor(FullPath).IsValid())
	{
		if (!ReloadGraphFromFile(FullPath)) return false;
	}

	FGraphState* State = GraphStates.Find(FullPath);
	if (!State || !State->Executor) return false;
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor = State->Executor;

	const UE::Aqueduct::FCompiledGraph* CG = Executor->GetCompiledGraph();
	const bool bAnimated = CG && CG->TimeDepNodes.Num() > 0;

	// Per-texture granularity: bake (read back + serialize) ONLY the outputs
	// whose dependency-closure hash differs from the hash recorded on their
	// texture — an edit to one branch costs exactly that branch's textures.
	const FString WholeHash = Executor->GetGraphContentHash();
	const TMap<FString, FString> OutputHashes = Executor->GetOutputContentHashes();
	State->CurrentHash = WholeHash;
	State->OutputContentHashes = OutputHashes;

	// Animated graphs bake their t=0 frame.
	if (bAnimated)
	{
		Executor->SetTime(0.0f);
		Executor->EvaluateTimeExpressions();
	}

	ENQUEUE_RENDER_COMMAND(AqueductBakeExec)([Exec = Executor](FRHICommandListImmediate& RHICmdList)
	{
		Exec->Execute(RHICmdList);
		RHICmdList.SubmitCommandsAndFlushGPU();
		RHICmdList.BlockUntilGPUIdle();
	});
	FlushRenderingCommands();

	// Never bake a blocked execute: blocked = nothing was dispatched, the
	// textures hold stale pixels that are NOT this graph's output.
	if (Executor->IsExecutionBlocked())
	{
		UE_LOG(LogAqueductMaterial, Error,
			TEXT("Bake refused for %s: %s"), *FullPath, *Executor->GetExecutionBlockReason());
		return false;
	}

	UAqueductGraph* GraphAsset = GetOrCreateGraphAsset(FullPath);
	UMaterialInstanceConstant* MIC = State->MIC ? State->MIC.Get() : GetOrCreateMIC(FullPath);
	// GetOrCreateMIC can grow GraphStates (rehash) — re-resolve the pointer.
	State = GraphStates.Find(FullPath);
	if (!State) return false;

	// Run the delivery chain so the per-delivery PACKED textures hold the
	// just-executed pixels — the bake reads those back (the same quantized
	// bytes every viewport is sampling through the BC encode).
	EnsureOutputTextureAssets(FullPath, Executor.Get(), MIC);
	State = GraphStates.Find(FullPath);
	if (!State) return false;
	State->CachedAnisoConstants = AqueductTextureWriter::ResolveAnisotropyConstantsFromBindingsJson(
		Executor->GetOutputBindingsJson());
	CopyOutputsToTextures(FullPath, Executor.Get());

	// Batched readback of the STALE deliveries only (blocking: this path
	// runs inside saves and unattended runs).
	// Shared-owned: the lock/unlock render commands keep their own ref, so
	// no early return can leave a command dangling against freed memory.
	TSharedRef<TArray<FPendingBakeOutput>> TargetsRef = MakeShared<TArray<FPendingBakeOutput>>();
	TArray<FPendingBakeOutput>& Targets = TargetsRef.Get();
	for (const FDeliveryDesc& Delivery : BuildDeliverySet(Executor.Get()))
	{
		UTexture2D* Tex = State->OutputTextures.FindRef(Delivery.Name);
		UAqueductTextureUserData* Meta = AqueductTexture::GetUserData(Tex, /*bCreate=*/false);
		// Fresh (hash match) → skip; no texture/meta yet → stale by definition.
		if (Meta && Meta->GraphContentHash == Delivery.CombinedHash) continue;

		FPendingBakeOutput& Target = Targets.AddDefaulted_GetRef();
		Target.Name = Delivery.Name;
		Target.Kind = Delivery.Kind;
		Target.CombinedHash = Delivery.CombinedHash;
		Target.Resolution = Delivery.Resolution;
		Target.ReadbackPF = AqueductTextureWriter::PackedPixelFormat(Delivery.Kind);
		Target.Readback = MakeShared<FRHIGPUTextureReadback>(TEXT("AqueductBakeReadback"));
	}
	if (Targets.Num() == 0)
	{
		UE_LOG(LogAqueductMaterial, Log,
			TEXT("Bake: all outputs current, nothing to do: %s"), *FullPath);
		return true;
	}
	EnqueueBakeReadbackCommands(TargetsRef, &State->DeliveryScratch, /*bBlocking=*/true);
	FlushRenderingCommands();

	ENQUEUE_RENDER_COMMAND(AqueductBakeLock)([TargetsRef](FRHICommandListImmediate& RHICmdList)
	{
		for (FPendingBakeOutput& Target : TargetsRef.Get())
		{
			if (!Target.Readback->IsReady()) continue;
			Target.MappedRows = Target.Readback->Lock(Target.RowPitchInPixels);
		}
	});
	FlushRenderingCommands();

	TArray<UTexture*> BakedTextures;
	TArray<FString> OutputNames;

	for (FPendingBakeOutput& Target : Targets)
	{
		if (!Target.MappedRows || Target.RowPitchInPixels <= 0) continue;

		UTexture2D* Tex = State->OutputTextures.FindRef(Target.Name);
		if (!Tex) continue;

		uint8* MipPtr = AqueductTexture::BeginWritePixels(Tex, Target.Resolution, Target.Kind, bAnimated);
		if (MipPtr)
		{
			CompactRowsInto(Target, MipPtr);
		}
		AqueductTexture::EndWritePixels(Tex);

		UAqueductTextureUserData* Meta = AqueductTexture::GetUserData(Tex, /*bCreate=*/true);
		Meta->OutputName = Target.Name;
		// Per-delivery closure hash: this texture is current until a branch
		// feeding IT changes.
		Meta->GraphContentHash = Target.CombinedHash;
		Meta->bDynamicWritable = bAnimated;
		Meta->SourceGraph = GraphAsset;
		BakedTextures.Add(Tex);
		OutputNames.Add(Target.Name);
	}

	ENQUEUE_RENDER_COMMAND(AqueductBakeUnlock)([TargetsRef](FRHICommandListImmediate& RHICmdList)
	{
		for (FPendingBakeOutput& Target : TargetsRef.Get())
		{
			if (Target.MappedRows) Target.Readback->Unlock();
		}
	});
	FlushRenderingCommands();

	if (BakedTextures.Num() == 0) return false;

	// Update the wrapper asset (everything a shipped game needs to parse and
	// execute the graph: TOML + registry + texture references; the cook adds
	// the precompiled shader metadata in UAqueductGraph::PreSave).
	if (GraphAsset)
	{
		GraphAsset->GraphName = FPaths::GetBaseFilename(FullPath);
		GraphAsset->GraphToml = State->GraphToml;
		GraphAsset->ContentHash = WholeHash;
		if (!State->SourceDocumentId.IsEmpty())
		{
			GraphAsset->SourceDocumentId = State->SourceDocumentId;
		}
		GraphAsset->bAnimated = bAnimated;
		GraphAsset->RegistryToml = CachedRegistryToml;
		// ALL outputs, not just the stale subset this bake touched.
		GraphAsset->OutputNames.Empty();
		GraphAsset->OutputTextures.Empty();
		for (const auto& TexPair : State->OutputTextures)
		{
			GraphAsset->OutputNames.Add(TexPair.Key);
			GraphAsset->OutputTextures.Add(TSoftObjectPtr<UTexture2D>(TexPair.Value));
		}
	}
	State->GraphAsset = GraphAsset;

	// Flag the packages the bake actually wrote — the baked texture Source and
	// the wrapper asset's metadata — so the user's save persists them. NOT the
	// MIC: the bake does not modify it (its texture-parameter bindings are
	// persisted when they change, in EnsureOutputTextureAssets). Because the bake
	// runs inside the pre-save hook, dirtying the MIC here would re-dirty it
	// AFTER the save already serialized it, leaving a spurious "unsaved MI_…"
	// prompt on editor close.
	for (UTexture* Tex : BakedTextures) Tex->MarkPackageDirty();
	if (GraphAsset) GraphAsset->MarkPackageDirty();

	// Source now holds the real pixels: the engine's rebuilt resource is
	// correct by construction, so live maintenance can end. Release the
	// override textures (safe: maintenance is off and rendering was flushed
	// above, so no copies are in flight; the RHI texture reference keeps the
	// last override alive — showing exactly the baked pixels — until the
	// post-bake resource init repoints it at the rebuilt resource).
	State->bMaintainLiveResource = false;
	State->DeliveryScratch.Empty();

	// Static (and idle animated) graphs stop here: hand every session texture
	// back to its engine-built BC resource so the saved/cooked asset is exactly
	// what shows. A still-Live animated graph keeps its live resources (the
	// ticker re-asserts and re-delivers each frame).
	if (!(bAnimated && State->Tier == FGraphState::EAnimatedTier::Live))
	{
		RevertLiveResources(*State);
	}

	// A Live animated graph resumes its clock (the ticker keeps running).
	if (bAnimated && State->Tier == FGraphState::EAnimatedTier::Live)
	{
		const double Now = FPlatformTime::Seconds();
		Executor->SetTime((float)(Now - State->AnimationStartTime));
		Executor->EvaluateTimeExpressions();
	}
	else if (State->Tier == FGraphState::EAnimatedTier::Ready)
	{
		// The bake's execute allocated working VRAM for a graph in the Ready
		// tier (nothing on screen samples it). Keep the tier's invariant —
		// Ready means zero working VRAM — by releasing what the bake used.
		FlushRenderingCommands();
		Executor->ReleaseGraphResources();
		State->DeliveryScratch.Empty();
	}

	UE_LOG(LogAqueductMaterial, Log, TEXT("Baked graph: %s (%d stale output(s), hash %s%s)"),
		*FullPath, BakedTextures.Num(), *WholeHash.Left(10),
		bAnimated ? TEXT(", animated t=0") : TEXT(""));

	// bAllowUnattendedSave=false: we are already inside a save (pre-save
	// hook) — a nested SavePackage would be re-entrant.
	if (FApp::IsUnattended() && bAllowUnattendedSave)
	{
		SaveBakedPackagesUnattended(FullPath);
	}
	return true;
}

void FAqueductMaterialManager::SaveBakedPackagesUnattended(const FString& GraphFilePath)
{
	FGraphState* State = GraphStates.Find(GraphFilePath);
	if (!State) return;

	TArray<UTexture*> Textures;
	for (const auto& Pair : State->OutputTextures)
	{
		if (Pair.Value) Textures.Add(Pair.Value);
	}
	FTextureCompilingManager::Get().FinishCompilation(Textures);

	for (UTexture* Tex : Textures) SaveAssetPackage(Tex);
	if (State->GraphAsset) SaveAssetPackage(State->GraphAsset);
	if (State->MIC) SaveAssetPackage(State->MIC);

	UE_LOG(LogAqueductMaterial, Log, TEXT("Saved baked packages: %s"), *GraphFilePath);
}



FString FAqueductMaterialManager::GetVramStatsJson(const FString& GraphFilePath)
{
	check(IsInGameThread());
	const FString FullPath = FPaths::ConvertRelativePathToFull(GraphFilePath);
	FGraphState* State = GraphStates.Find(FullPath);
	if (!State || !State->Executor.IsValid())
	{
		return FString();
	}

	FlushRenderingCommands(); // drain in-flight executor work before reading its maps

	return UE::Aqueduct::FAqueductGraphExecutor::VramSnapshotToJson(
		State->Executor->GetVramSnapshot());
}
