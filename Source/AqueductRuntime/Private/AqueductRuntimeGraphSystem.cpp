#include "AqueductRuntimeGraphSystem.h"
#include "AqueductGraphAsset.h"
#include "AqueductPaths.h"
#include "AqueductTextureWriter.h"
#include "AqueductLiveTextureResource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductRuntimeGraphs, Log, All);

FAqueductRuntimeGraphSystem& FAqueductRuntimeGraphSystem::Get()
{
	static FAqueductRuntimeGraphSystem Instance;
	return Instance;
}

void FAqueductRuntimeGraphSystem::Initialize()
{
	if (bInitialized || GIsEditor) return;
	bInitialized = true;

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AssetRegistry.WaitForCompletion();

	TArray<FAssetData> GraphAssets;
	AssetRegistry.GetAssetsByClass(UAqueductGraph::StaticClass()->GetClassPathName(), GraphAssets);

	for (const FAssetData& AssetData : GraphAssets)
	{
		UAqueductGraph* GraphAsset = Cast<UAqueductGraph>(AssetData.GetAsset());
		if (!GraphAsset || !GraphAsset->bAnimated) continue;
		if (GraphAsset->GraphToml.IsEmpty() || GraphAsset->RegistryToml.IsEmpty())
		{
			UE_LOG(LogAqueductRuntimeGraphs, Warning,
				TEXT("%s: animated graph missing TOML/registry — skipping"), *AssetData.AssetName.ToString());
			continue;
		}

		TUniquePtr<FRuntimeGraph> Runtime = MakeUnique<FRuntimeGraph>();
		Runtime->GraphName = GraphAsset->GraphName;
		Runtime->Executor = MakeShared<UE::Aqueduct::FAqueductGraphExecutor>();

		if (!Runtime->Executor->LoadRegistry(GraphAsset->RegistryToml) ||
			!Runtime->Executor->LoadGraph(GraphAsset->GraphToml))
		{
			UE_LOG(LogAqueductRuntimeGraphs, Error,
				TEXT("%s: graph parse failed at runtime"), *Runtime->GraphName);
			continue;
		}

		bool bReady = false;
		if (!GraphAsset->CookedMetadataJson.IsEmpty())
		{
			// Cooked path: shaders are in the cooked global shader map.
			bReady = Runtime->Executor->LoadPrecompiledMetadata(GraphAsset->CookedMetadataJson);
		}
		else
		{
			// Uncooked `-game` runs (editor binary): full compile available.
			bReady = Runtime->Executor->CompileAllShaders();
		}
		if (!bReady)
		{
			UE_LOG(LogAqueductRuntimeGraphs, Error,
				TEXT("%s: shader metadata unavailable — graph will not animate"), *Runtime->GraphName);
			continue;
		}

		// Resolve the texture assets whose GPU contents we overwrite.
		// Rooted for the system's lifetime: nothing else references them in a
		// game until a level using the material loads, and the initial
		// map-load garbage collection would otherwise destroy them between
		// here and the first tick (dangling FTextureResource on the render
		// thread).
		for (const TSoftObjectPtr<UTexture2D>& SoftTex : GraphAsset->OutputTextures)
		{
			UTexture2D* Tex = SoftTex.LoadSynchronous();
			if (!Tex) continue;
			Tex->AddToRoot();
			// Output name = suffix after "T_<graph>_"
			FString TexName = Tex->GetName();
			FString Prefix = TEXT("T_") + Runtime->GraphName + TEXT("_");
			FString OutputName = TexName.StartsWith(Prefix) ? TexName.Mid(Prefix.Len()) : TexName;
			Runtime->OutputTextures.Add(OutputName, Tex);
		}

		Runtime->StartTime = FPlatformTime::Seconds();
		UE_LOG(LogAqueductRuntimeGraphs, Log,
			TEXT("Animated graph online: %s (%d outputs, %s)"),
			*Runtime->GraphName, Runtime->OutputTextures.Num(),
			GraphAsset->CookedMetadataJson.IsEmpty() ? TEXT("compiled") : TEXT("precompiled"));
		Graphs.Add(MoveTemp(Runtime));
	}

	if (Graphs.Num() > 0)
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FAqueductRuntimeGraphSystem::Tick), 0.0f);
	}
}

void FAqueductRuntimeGraphSystem::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	// Drain the render thread before destroying executors/scratch: the
	// execute+deliver commands hold executor refs and &DeliveryScratch.
	FlushRenderingCommands();
	// At engine exit, modules unload AFTER the UObject system is torn down
	// ("Object subsystem successfully closed" precedes module shutdown). Touching
	// UTextures then (GetResource/UpdateResource/RemoveFromRoot) dereferences
	// freed objects and asserts. Only do the UObject-side cleanup while the
	// object system is live (real runtime shutdown / re-init); at exit purge
	// everything is being destroyed anyway.
	if (UObjectInitialized())
	{
		for (auto& Graph : Graphs)
		{
			// Hand textures back to their engine resources before dropping them,
			// so none lingers on a plugin-owned override.
			for (auto& Pair : Graph->LiveResources)
			{
				AqueductLiveTexture::RevertLiveResource(
					Graph->OutputTextures.FindRef(Pair.Key), Pair.Value);
			}
			Graph->LiveResources.Empty();
			for (auto& TexPair : Graph->OutputTextures)
			{
				if (TexPair.Value) TexPair.Value->RemoveFromRoot();
			}
		}
	}
	Graphs.Empty();
	bInitialized = false;  // symmetric lifecycle (re-init after shutdown works)
}

bool FAqueductRuntimeGraphSystem::Tick(float DeltaTime)
{
	const float IdleRelease = AqueductAnimated::IdleReleaseSeconds;
	const double MinInterval = 1.0 / AqueductAnimated::MaxFPS;
	const double Now = FPlatformTime::Seconds();
	const double RenderNow = FApp::GetCurrentTime(); // LastRenderTime domain

	for (auto& Graph : Graphs)
	{
		// Visibility-driven execution: a texture's LastRenderTime advances
		// whenever a draw actually samples it (the texture streamer's
		// signal). If nothing on screen uses this graph, executing it would
		// burn GPU time on pixels nobody sees — skip, and after the idle
		// window release its working VRAM (Live → Ready; the textures still
		// hold their last contents, so a return to view never shows black,
		// and resuming costs an allocation + dispatch — never a compile).
		bool bVisible = false;
		for (const auto& TexPair : Graph->OutputTextures)
		{
			if (TexPair.Value && RenderNow - TexPair.Value->GetLastRenderTimeForStreaming() < 0.3)
			{
				bVisible = true;
				break;
			}
		}

		if (!bVisible)
		{
			if (Graph->bLive && Now - Graph->LastVisibleTime > IdleRelease)
			{
				// Fence: prior updates' render commands hold the executor and
				// &DeliveryScratch — release only after the RT has drained.
				FlushRenderingCommands();
				Graph->Executor->ReleaseGraphResources();
				Graph->DeliveryScratch.Empty();
				// Free the live uncompressed textures too; the cooked BC poster
				// shows while idle (off-screen), and resuming reinstalls them.
				for (auto& Pair : Graph->LiveResources)
				{
					AqueductLiveTexture::RevertLiveResource(
						Graph->OutputTextures.FindRef(Pair.Key), Pair.Value);
				}
				Graph->LiveResources.Empty();
				Graph->bLive = false;
				UE_LOG(LogAqueductRuntimeGraphs, Log,
					TEXT("%s: idle %.0fs, released working VRAM"), *Graph->GraphName, IdleRelease);
			}
			continue;
		}

		Graph->LastVisibleTime = Now;
		if (!Graph->bLive)
		{
			Graph->bLive = true;
			UE_LOG(LogAqueductRuntimeGraphs, Log, TEXT("%s: visible, resuming"), *Graph->GraphName);
		}

		if (Now - Graph->LastUpdateTime < MinInterval) continue;
		Graph->LastUpdateTime = Now;
		ExecuteGraphAtTime(*Graph, (float)(Now - Graph->StartTime));
	}
	return true;
}

void FAqueductRuntimeGraphSystem::ExecuteGraphAtTime(FRuntimeGraph& Graph, float Time)
{
	using namespace AqueductTextureWriter;

	// Build the delivery targets on the game thread (UTexture2D access is GT
	// law); leave Src unresolved — the executor's output map is RENDER THREAD
	// state (Execute populates/rebuilds it there), so it must only be read on
	// the render thread, after Execute. The cooked textures are keyed by
	// DELIVERY name (the asset suffix). Animated graphs are recomputed every
	// frame, so — exactly like the editor session — they render into an
	// UNCOMPRESSED live texture (no realtime BC encode): the same delivery
	// chain, byte-identical to the baked poster the cooked asset holds at rest.
	TArray<FDeliveryTarget> Targets;
	for (const auto& TexPair : Graph.OutputTextures)
	{
		UTexture2D* Tex = TexPair.Value;
		if (!Tex) continue;

		// Classify from the asset's compression settings (set by
		// ApplyDeliverySettings): the cooked asset's format encodes its kind.
		const EAqueductMapKind Kind = (TexPair.Key == OrmDeliveryName())
			? EAqueductMapKind::Orm
			: Tex->CompressionSettings == TC_Normalmap ? EAqueductMapKind::Normal
			: Tex->CompressionSettings == TC_Grayscale ? EAqueductMapKind::Gray
			: EAqueductMapKind::Color;

		// Install/maintain the uncompressed live resource (the shared helper —
		// same mechanism as the editor). Size from the executor resolution: the
		// SAME source the delivery chain sizes from, so the live resource and the
		// delivery can never disagree on size (a mismatch would silently skip the
		// delivery and freeze the animation on its poster).
		const uint32 Size = Graph.Executor->GetResolution();
		EnsureLiveDeliveryResource(Tex, Kind, Size, Graph.LiveResources.FindOrAdd(TexPair.Key));

		FTextureResource* DstResource = Tex->GetResource();
		if (!DstResource || !DstResource->TextureRHI.IsValid()) continue;
		if (!Tex->TextureReference.TextureReferenceRHI.IsValid()) continue;

		FDeliveryTarget T;
		T.Name = TexPair.Key;
		T.Kind = Kind;
		T.bFlipNormalGreen =
			(Kind == EAqueductMapKind::Normal && Graph.Executor->GetNormalFormat() == 0) ? 1 : 0;
		// RHI refs pinned on the game thread — see FDeliveryTarget. The
		// reference repoint + uncompressed delivery into the live texture is the
		// editor's live-override path, now shared with the cooked runtime.
		T.DstTexture = DstResource->TextureRHI;
		T.TextureReference = Tex->TextureReference.TextureReferenceRHI;
		Targets.Add(MoveTemp(T));
	}
	if (Targets.Num() == 0) return;

	// ONE render command per update: time evaluation, graph execution,
	// output resolution, and delivery all run on the render thread, in
	// order, holding a SHARED ref to the executor. The game thread never
	// touches executor state between updates, and the executor cannot be
	// destroyed under an in-flight command. (The graph/registry FFI handles
	// are only mutated at Initialize, before the ticker exists.)
	ENQUEUE_RENDER_COMMAND(AqueductRuntimeExec)(
		[Exec = Graph.Executor, Time, Targets = MoveTemp(Targets),
		 Scratch = &Graph.DeliveryScratch](FRHICommandListImmediate& RHICmdList) mutable
	{
		Exec->SetTime(Time);
		Exec->EvaluateTimeExpressions();
		Exec->Execute(RHICmdList);

		// Resolve sources from the (now current) output map.
		const TMap<FString, FTextureRHIRef>& Outputs = Exec->GetOutputTextures();
		for (int32 i = Targets.Num() - 1; i >= 0; --i)
		{
			FDeliveryTarget& T = Targets[i];
			if (T.Kind == EAqueductMapKind::Orm)
			{
				const TCHAR* const* Components = OrmComponentOutputs();
				bool bAny = false;
				for (int32 c = 0; c < 3; ++c)
				{
					T.OrmSrc[c] = Outputs.FindRef(Components[c]);
					bAny |= T.OrmSrc[c].IsValid();
				}
				if (!bAny) Targets.RemoveAtSwap(i);
			}
			else
			{
				T.Src = Outputs.FindRef(T.Name);
				if (!T.Src.IsValid()) Targets.RemoveAtSwap(i);
			}
		}
		DeliverOutputsRT(RHICmdList, Targets, Scratch);
	});
}
