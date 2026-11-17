#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "AqueductGraphExecutor.h"
#include "AqueductTextureWriter.h"

class UMaterial;
class UMaterialInstanceConstant;
class UTexture2D;
class UAqueductGraph;
class FRHIGPUTextureReadback;
struct FFileChangeData;

/**
 * Manages materials for all Aqueduct graphs.
 *
 * Owns the base material, per-graph Material Instance Constants,
 * UAqueductTexture output assets, executors, the animation ticker, and the
 * always-on file watcher over .aqueduct files.
 *
 * Architecture invariants:
 * - "Reload from file" (ReloadGraphFromFile) is the single ingestion path for
 *   graph changes. The OS file watcher invokes it for external edits; the
 *   live link invokes it synchronously after writing the file itself.
 * - Material Instance Constants bind UAqueductTexture assets permanently.
 *   Live updates overwrite the textures' GPU resources in place; nothing
 *   ever rebinds a material.
 * - Bakes are LAZY: live edits only mark packages dirty. Baked pixels are
 *   written exactly when a save needs them (pre-save hook), at the startup
 *   hash guard, or explicitly in unattended runs. The graph content hash
 *   recorded on the assets owns correctness: mismatch at load → auto re-bake,
 *   so an unsaved (or crashed) session can never produce a wrong material.
 * - Animated graphs follow a residency ladder (see FGraphState::EAnimatedTier):
 *   compiled and ready at load, executing only while something on screen
 *   samples their textures, zero working VRAM otherwise.
 *
 * Singleton, accessed via Get().
 */
class FAqueductMaterialManager
{
public:
	struct FNumericParamEdit
	{
		FString NodeId;
		FString ParamName;
		double Value = 0.0;
		bool bInteger = false;
	};

	static FAqueductMaterialManager& Get();

	/** Idempotent. Scans Content/Aqueduct, applies the at-rest/re-bake
	 *  hash policy per graph, and starts the file watcher. */
	void EnsureInitialized();

	/** Called at module shutdown. */
	void Shutdown();

	/**
	 * The single ingestion path: (re)load a graph from its file on disk,
	 * compile if needed, execute on the GPU, copy outputs into the bound
	 * UAqueductTexture assets, and mark their packages dirty so the editor's
	 * standard save flow (where the pre-save hook bakes) knows about them.
	 */
	bool ReloadGraphFromFile(const FString& GraphFilePath);

	/** Fast path for a live slider frame. The exact TOML has already been
	 *  written to GraphFilePath, so this only updates compiled uniforms,
	 *  replays the dirty dependency cone, and delivers the new GPU outputs.
	 *  Returns false when the edit cannot safely use the existing compiled
	 *  graph; callers then fall back to ReloadGraphFromFile. */
	bool ApplyLiveParameterEdits(const FString& GraphFilePath, const FString& GraphToml,
		const TArray<FNumericParamEdit>& Edits);

	/**
	 * Persist the graph's current GPU outputs into its texture assets
	 * (blocking: GPU sync + readback + source write) and update the
	 * UAqueductGraph wrapper asset. Animated graphs bake their t=0 frame.
	 *
	 * Bakes are LAZY: this runs only (a) from the pre-save hook when a
	 * stale Aqueduct texture is about to be saved — i.e. inside Ctrl+S /
	 * Save All / the exit prompt, operations that already block by user
	 * intent; (b) from the startup hash guard; (c) explicitly in
	 * unattended runs (tests, automation). Editing sessions themselves
	 * never bake — they mark packages dirty and move on.
	 *
	 * bAllowUnattendedSave=false suppresses the unattended inline save
	 * (used by the pre-save hook, which is already inside a save).
	 */
	bool BakeGraph(const FString& GraphFilePath, bool bAllowUnattendedSave = true);

	/**
	 * VRAM bookkeeping JSON for one graph's executor working set (node
	 * textures, scratch buffers, singletons), in the camelCase contract
	 * shared with the web frontend. Empty string if the graph is unknown
	 * or has no executor.
	 */
	FString GetVramStatsJson(const FString& GraphFilePath);

	/**
	 * Suppress the next file-watcher notification for this file.
	 * Called before a self-write (live link) so the synchronous reload is not
	 * followed by a redundant watcher-triggered reload.
	 */
	void SuppressNextWatcherEvent(const FString& GraphFilePath);

	/**
	 * Ensure a file can be written: version-control workspaces (Perforce)
	 * keep files read-only until checked out, so this attempts a checkout
	 * through the editor's source-control provider. Returns false (with a
	 * user-facing message) if the file is still read-only — callers surface
	 * the error rather than silently dropping the write.
	 */
	static bool EnsureFileWritable(const FString& FilePath, FString& OutError);

	/**
	 * Mark a newly created file for add in source control (async, no-op when
	 * source control is disabled). Every file the plugin creates directly —
	 * graph recipes, directly-saved packages, AqueductAssets downloads — goes
	 * through this, so a teammate's sync always has the complete material.
	 * (Files saved through the editor's own save flow are covered by the
	 * editor's add-on-save behavior and don't need this.)
	 */
	static void MarkFileForAdd(const FString& FilePath);

	/**
	 * Canonical location of a graph's recipe file. An existing recipe
	 * anywhere under Content/Aqueduct keeps its location (checked in
	 * session state first, then on disk); a new graph gets a per-graph
	 * subfolder, Content/Aqueduct/<graph>/<graph>.aqueduct — every asset
	 * of a graph is created next to its recipe. Game thread only.
	 */
	FString ResolveGraphPath(const FString& GraphName);

	/**
	 * Resolve an established cloud destination by its persisted source
	 * document id. Session state is checked first, then all Aqueduct graph
	 * wrapper assets. Empty means no established association.
	 */
	FString ResolveGraphPathBySourceDocumentId(const FString& SourceDocumentId);

	/**
	 * The write-ownership guard for document identity (see
	 * UAqueductGraph::SourceDocumentId): may a sync from SourceDocumentId
	 * write this graph? True (adopting/stamping as needed) unless the graph
	 * is stamped by a DIFFERENT document and bForce is false — the caller
	 * refuses the sync so the editor can ask before overwriting. An empty
	 * incoming id (local-file documents) always passes: file semantics.
	 * Game thread only.
	 */
	bool AcquireSourceOwnership(const FString& GraphFilePath, const FString& SourceDocumentId, bool bForce);

	/** Promote a graph into the Live animation tier (editing implies
	 *  attention, so live-link edits force this regardless of sampling). */
	void RegisterAnimatedGraph(const FString& GraphFilePath,
		TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor,
		double AnimationStartTime);

	/** Drop a graph out of the animation tiers (graph became static). */
	void UnregisterAnimatedGraph(const FString& GraphFilePath);

private:
	FAqueductMaterialManager() = default;

	/** Get or create the MIC for a graph. Returns nullptr if the graph is outside Content/. */
	UMaterialInstanceConstant* GetOrCreateMIC(const FString& TomlPath);

	/** Get the executor for a graph if one exists. */
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> FindExecutor(const FString& GraphFilePath);

	/**
	 * Download whitelisted asset URLs missing from the project store
	 * (Content/Aqueduct/AqueductAssets). Each completed download is hash-
	 * verified, atomically placed in the store, marked for add in source
	 * control, and the graph reloads so the bitmap node picks it up.
	 * Deduplicates in-flight URLs; failures log and degrade per-node.
	 */
	void StartAssetDownloads(const FString& GraphFilePath, const TArray<FString>& MissingUrls);

	/** Asset URLs with an in-flight HTTP request. */
	TSet<FString> InFlightAssetDownloads;

	/** Aqueduct-owned parent materials. A graph gets the simplest parent that
	 *  shades everything it binds, so graphs without fuzz or anisotropy keep the
	 *  plain parents and pay for neither feature. */
	enum class EAqueductParentKind : uint8
	{
		Base,              // Default Lit, opaque
		Anisotropic,       // Default Lit, opaque, anisotropy and tangent wired
		Cloth,             // Cloth, opaque, fuzz wired
		Translucent,       // Default Lit, translucent
		TranslucentCloth,  // Cloth, translucent, fuzz wired
		Count,
	};

	struct FGraphState;  // defined below; referenced by the helpers above the definition

	UMaterial* GetOrCreateBaseMaterial(EAqueductParentKind ParentKind = EAqueductParentKind::Base);

	/** Get the executor for a graph, creating + compiling one if needed. */
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> GetOrCreateExecutor(
		const FString& GraphFilePath,
		const FString& RegistryToml,
		const FString& GraphToml);

	/**
	 * Ensure a UAqueductTexture asset exists (right size/format) for each
	 * executor output and is bound to the MIC. Creates zero-filled assets on
	 * first encounter so live copies have a destination before the first bake.
	 */
	void EnsureOutputTextureAssets(const FString& GraphFilePath,
		UE::Aqueduct::FAqueductGraphExecutor* Executor,
		UMaterialInstanceConstant* MIC);

	/** GPU-copy executor outputs into the bound texture assets' live resources:
	 *  installs/maintains the uncompressed FAqueductLiveTextureResource on each
	 *  session texture (so the texture editor and thumbnails sample live too) and
	 *  delivers the executor's pixels into it.
	 *
	 *  Called every maintenance tick while a graph has unsaved edits, and it
	 *  re-delivers UNCONDITIONALLY (it does not try to detect "did anything
	 *  change"). The install creates an EMPTY texture — the delivery is what
	 *  fills it — and the engine reverts our live resource on routine editor
	 *  actions (opening the texture editor or a material preview triggers
	 *  UpdateResource). Re-delivering every tick is the simple, robust repair: it
	 *  refills and re-asserts the texture reference after any disturbance, one
	 *  tick later, without having to detect which disturbance occurred. Gating
	 *  delivery on a "did it reinstall this tick" flag is the kind of cleverness
	 *  that previously shipped uninitialized garbage — don't. */
	void CopyOutputsToTextures(const FString& GraphFilePath,
		UE::Aqueduct::FAqueductGraphExecutor* Executor);

	/** Install (or re-assert, idempotently) the uncompressed live-texture
	 *  resource on one session texture, so every surface that samples it — the
	 *  reference (viewport, material preview) and GetResource() (texture editor,
	 *  thumbnails) — shows the live pixels. No-op if already installed at the
	 *  right size, or while the engine is still async-building the asset. */
	void EnsureLiveResource(FGraphState& State, const FString& DeliveryName,
		UTexture2D* Texture, AqueductTextureWriter::EAqueductMapKind Kind, uint32 Size);

	/** Revert every session texture to its normal engine-built (BC) resource —
	 *  used when a session ends so assets never linger on an override. */
	void RevertLiveResources(FGraphState& State);

	/** Load-or-create the UAqueductGraph wrapper asset for a graph file. */
	UAqueductGraph* GetOrCreateGraphAsset(const FString& GraphFilePath);

	/**
	 * UNATTENDED ONLY: save a baked graph's packages (textures, wrapper,
	 * material instance) after blocking on the texture build. Tests and
	 * automation need files on disk before process exit. Interactive
	 * sessions never save — the user does, and the pre-save hook bakes
	 * stale graphs at that moment.
	 */
	void SaveBakedPackagesUnattended(const FString& GraphFilePath);

	/** Watch a directory for .aqueduct changes (idempotent per directory). */
	void WatchDirectory(const FString& Directory);

	/** File watcher callback (arbitrary thread → marshals to game thread). */
	void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);

	/** Per-graph state. */
	struct FGraphState
	{
		TObjectPtr<UMaterialInstanceConstant> MIC;
		TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor;

		/** Delivery name → bound texture asset (permanent MIC bindings).
		 *  Deliveries are the ASSET-level unit: ao/roughness/metallic outputs
		 *  pack into one "orm" delivery; every other output is its own. */
		TMap<FString, TObjectPtr<UTexture2D>> OutputTextures;

		/** Delivery name → render-thread-owned chain scratch (packed mips,
		 *  block staging). */
		TMap<FString, AqueductTextureWriter::FDeliveryScratch> DeliveryScratch;

		/** Delivery name → the FAqueductLiveTextureResource installed on the
		 *  texture asset during this session (raw, owned by the UTexture2D). Used
		 *  only for identity checks: if GetResource() no longer matches, the
		 *  engine's async build reverted it and we reinstall. */
		TMap<FString, FTextureResource*> LiveResources;

		/** Cached TOML + core-computed canonical hashes of the most recent
		 *  reload. CurrentHash gates the wrapper asset; OutputContentHashes
		 *  (output name → dependency-closure hash) gate each texture
		 *  individually — the basis for per-texture dirty/bake granularity. */
		FString GraphToml;
		FString CurrentHash;
		TMap<FString, FString> OutputContentHashes;
		AqueductTextureWriter::FAnisotropyConstants CachedAnisoConstants;

		/** Wrapper asset (cached for the deferred save). */
		TObjectPtr<UAqueductGraph> GraphAsset;

		/** Session truth of the cloud document id syncing this graph (see
		 *  UAqueductGraph::SourceDocumentId). Stamped onto the wrapper at
		 *  creation/bake; persisted by the user's save. */
		FString SourceDocumentId;

		/**
		 * Animated-graph residency (the texture-streaming model applied to
		 * procedural graphs — readiness is decoupled from cost):
		 *   None         static graph; no animation machinery
		 *   ReadyPending animated, awaiting background shader preparation
		 *   Ready        compiled + parsed; ZERO working VRAM
		 *   Live         visible (something samples its textures) → executing
		 * Live → Ready releases the graph's working GPU memory after
		 * AqueductAnimated::IdleReleaseSeconds unsampled. Ready → Live costs one
		 * allocation + dispatch — never a compile.
		 */
		enum class EAnimatedTier : uint8 { None, ReadyPending, Ready, Live };
		EAnimatedTier Tier = EAnimatedTier::None;
		double AnimationStartTime = 0.0;  // wall-clock t=0 (continuous across tiers)
		double LastVisibleTime = 0.0;

		/**
		 * Unsaved edits exist: the ticker re-copies the executor's outputs
		 * into the texture assets' resources every tick, because the
		 * engine's async build pipeline rebuilds those resources from the
		 * stale Source on its own schedule with uploads unordered against
		 * render commands — un-raceable, so we repair instead (one tick).
		 * Cleared by the bake (Source fresh ⇒ engine builds are correct).
		 */
		bool bMaintainLiveResource = false;
	};

	TMap<FString, FGraphState> GraphStates;
	UMaterial* ParentMaterials[static_cast<int32>(EAqueductParentKind::Count)] = {};
	FTSTicker::FDelegateHandle TickerHandle;
	bool bInitialized = false;

	/** Pre-save hook: a stale Aqueduct texture is about to be serialized —
	 *  bake its graph first so the user never saves stale pixels. This is
	 *  the ONLY interactive bake trigger. */
	void OnObjectPreSave(UObject* Object, class FObjectPreSaveContext SaveContext);
	FDelegateHandle PreSaveHandle;

	/**
	 * Recipe-dies-with-the-wrapper (the same semantic as the engine's
	 * "Delete Source Files with Assets", which cannot fire for .aqueduct
	 * because it has no importer): deleting a graph's wrapper asset in the
	 * editor deletes its recipe file too, so a content-browser folder
	 * delete removes the whole graph instead of leaving an invisible
	 * recipe that resurrects it on the next startup.
	 *
	 * Two-delegate handshake for precision: OnAssetsPreDelete (editor
	 * delete flow ONLY, fires before confirmation) records candidate
	 * wrappers; OnAssetRemoved confirms the deletion actually happened.
	 * Disk-scan removals (no PreDelete record) and cancelled delete
	 * dialogs (no Removed event) both fall through without effect.
	 */
	void OnAssetsPreDelete(const TArray<UObject*>& Objects);
	void OnWrapperAssetRemoved(const struct FAssetData& AssetData);
	FDelegateHandle PreDeleteHandle;
	FDelegateHandle AssetRemovedHandle;
	/** Wrapper object path → its recipe's absolute file path, recorded at
	 *  PreDelete and consumed (or abandoned) at Removed. */
	TMap<FSoftObjectPath, FString> PendingRecipeDeletes;

	/**
	 * One delivery texture: the asset-level unit shared by asset creation,
	 * live copies, and bakes. ao/roughness/metallic outputs combine into the
	 * "orm" delivery; every other output maps 1:1.
	 */
	struct FDeliveryDesc
	{
		FString Name;                                       // asset key ("base_color", "orm", ...)
		AqueductTextureWriter::EAqueductMapKind Kind;
		TArray<FString> OutputNames;                        // contributing graph outputs
		FString CombinedHash;                               // per-delivery content hash
		uint32 Resolution = 0;
	};

	/** Group executor outputs into deliveries (with combined closure hashes). */
	static TArray<FDeliveryDesc> BuildDeliverySet(UE::Aqueduct::FAqueductGraphExecutor* Executor);

	/** One blocking-readback target (also the unit of the blocking bake).
	 *  Reads the delivery chain's PACKED mip 0 — bytes already quantized on
	 *  the GPU by the exact code path live pixels went through; the CPU only
	 *  row-compacts (and BGRA-swizzles 8-bit RGBA into TSF_BGRA8). */
	struct FPendingBakeOutput
	{
		FString Name;                     // delivery name
		AqueductTextureWriter::EAqueductMapKind Kind;
		FString CombinedHash;
		EPixelFormat ReadbackPF;          // packed format (PackedPixelFormat(Kind))
		TSharedPtr<FRHIGPUTextureReadback> Readback;
		uint32 Resolution = 0;
		const void* MappedRows = nullptr; // staging memory (render-thread Lock)
		int32 RowPitchInPixels = 0;
	};

	/** Render-thread body: enqueue staging copies of each delivery's packed
	 *  mip 0 (found by name in the graph's scratch map). */
	static void EnqueueBakeCopiesRT(FRHICommandListImmediate& RHICmdList,
		TArray<FPendingBakeOutput>& Targets,
		TMap<FString, AqueductTextureWriter::FDeliveryScratch>* Scratch);

	/** Blocking wrapper around EnqueueBakeCopiesRT (+ full GPU sync).
	 *  Targets is shared-owned: the render commands hold their own ref, so
	 *  lifetime never depends on the caller reaching a later flush. */
	static void EnqueueBakeReadbackCommands(TSharedRef<TArray<FPendingBakeOutput>> Targets,
		TMap<FString, AqueductTextureWriter::FDeliveryScratch>* Scratch, bool bBlocking);

	/** Row-compact mapped staging rows into the texture-source layout.
	 *  Pure memcpys/swizzles — any thread. */
	static void CompactRowsInto(const FPendingBakeOutput& Target, uint8* Dest);

	/** Is any of the graph's output textures being sampled by a draw right
	 *  now (the same engine LastRenderTime signal texture streaming uses)? */
	static bool IsGraphVisible(const FGraphState& State);

	/** Background readiness: compile one ReadyPending graph (DDC-cached;
	 *  never blocks startup, never runs on first sight). */
	void PrepareOneReadyPendingGraph();

	/** Ready → Live: allocate working VRAM + one dispatch (never a compile),
	 *  bind/copy outputs, log the measured warm-up. */
	void PromoteToLive(const FString& GraphFilePath);

	/** Directory → watcher delegate handle. */
	TMap<FString, FDelegateHandle> WatchedDirectories;

	/** Normalized full paths whose next watcher event is a self-write echo. */
	TSet<FString> SuppressedFiles;

	/** Registry TOML, loaded once on first use. */
	FString CachedRegistryToml;

	bool Tick(float DeltaTime);
	void EnsureTicker();
};
