#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "AqueductAnimated.h"
#include "AqueductGraphExecutor.h"
#include "AqueductTextureWriter.h"

class UAqueductGraph;
class UTexture2D;

/**
 * Game-runtime playback of animated Aqueduct graphs.
 *
 * In a cooked game (or `-game` mode), discovers every animated UAqueductGraph
 * asset, builds an executor from its cook-time precompiled shader metadata
 * (no shader compilation — the node shaders live in the cooked global shader
 * map), and overwrites the graph's texture assets' GPU contents each update
 * so the shipped Material Instance Constants animate with zero rebinding.
 *
 * Execution is visibility-driven: a graph runs only while something on screen
 * samples its textures (the engine's LastRenderTime signal), and releases its
 * working VRAM after AqueductAnimated::IdleReleaseSeconds unsampled. The
 * baked t=0 pixels in the shipped textures are what's on screen until the
 * first update.
 *
 * Inactive in the editor (the MaterialManager owns editor playback).
 * Update rate is capped by AqueductAnimated::MaxFPS.
 */
class FAqueductRuntimeGraphSystem
{
public:
	static FAqueductRuntimeGraphSystem& Get();

	/** Called on PostEngineInit in game contexts. Safe to call repeatedly. */
	void Initialize();
	void Shutdown();

	struct FRuntimeGraph
	{
		FString GraphName;
		TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor;
		AqueductTextureWriter::FAnisotropyConstants AnisoConstants;
		TMap<FString, TObjectPtr<UTexture2D>> OutputTextures;
		TMap<FString, AqueductTextureWriter::FDeliveryScratch> DeliveryScratch; // render-thread owned
		/** Delivery name → the FAqueductLiveTextureResource installed on the
		 *  texture (owned by the UTexture2D). Animated graphs render into an
		 *  uncompressed live texture, exactly like the editor session. */
		TMap<FString, FTextureResource*> LiveResources;
		double StartTime = 0.0;
		double LastUpdateTime = 0.0;

		/** Visibility-driven residency: executing (true) vs working VRAM
		 *  released after AqueductAnimated::IdleReleaseSeconds unsampled
		 *  (false). */
		bool bLive = false;
		double LastVisibleTime = 0.0;
	};

private:
	FAqueductRuntimeGraphSystem() = default;

	bool Tick(float DeltaTime);
	void ExecuteGraphAtTime(FRuntimeGraph& Graph, float Time);

	TArray<TUniquePtr<FRuntimeGraph>> Graphs;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bInitialized = false;
};
