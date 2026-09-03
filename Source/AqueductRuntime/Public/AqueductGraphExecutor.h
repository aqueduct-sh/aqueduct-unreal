#pragma once

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHIResources.h"
#include "AqueductShader.h"
#include "AqueductDynamicShader.h"
#include "aqueduct.h"
#include "AqueductCompiledGraph.h"

namespace UE::Aqueduct
{

enum class EAqueductNodeProfileState : uint8
{
	MeasuredThisRender,
	Cached,
	Unknown,
	Unsupported,
};

struct FAqueductNodeProfile
{
	FString NodeId;
	FString Kind;
	double GpuMs = 0.0;
	bool bHasGpuMs = false;
	uint32 Dispatches = 0;
	uint32 Passes = 1;
	EAqueductNodeProfileState State = EAqueductNodeProfileState::Unknown;
};

struct FAqueductRenderProfile
{
	double ActualGpuMs = 0.0;
	bool bHasActualGpuMs = false;
	double ModeledFullGraphGpuMs = 0.0;
	bool bHasModeledFullGraphGpuMs = false;
};

struct FAqueductProfileSnapshot
{
	uint32 Version = 1;
	FString Backend = TEXT("unreal");
	uint64 GraphRevision = 0;
	uint64 RenderRevision = 0;
	bool bTimestampSupported = false;
	FAqueductRenderProfile Render;
	TArray<FAqueductNodeProfile> Nodes;
};

// ---------------------------------------------------------------------------
// VRAM bookkeeping — mirrors AqueductVramSnapshot in the Rust engine
// (aqueduct/crates/engine/src/vram.rs), including category names.
// ---------------------------------------------------------------------------

struct FAqueductVramCategory
{
	FString Name;
	uint64 Bytes = 0;
	uint32 Count = 0;
};

struct FAqueductVramNodeUsage
{
	FString NodeId;
	uint64 Bytes = 0;
	uint32 Count = 0;
};

struct FAqueductVramSnapshot
{
	uint32 Version = 1;
	FString Backend = TEXT("unreal");
	uint32 Resolution = 0;
	uint64 CurrentBytes = 0;
	uint64 PeakBytes = 0;
	uint64 TextureBytes = 0;
	uint64 BufferBytes = 0;
	uint32 TextureCount = 0;
	uint32 BufferCount = 0;
	TArray<FAqueductVramCategory> Categories;
	TArray<FAqueductVramNodeUsage> Nodes;
};

// ---------------------------------------------------------------------------
// Managed texture — tracks an RHI texture + its format string
// ---------------------------------------------------------------------------

struct FAqueductTexture
{
	FTextureRHIRef Texture;
	FString FormatStr; // e.g. "rgba16float"
	uint32 Resolution = 0;
};

// ---------------------------------------------------------------------------
// Per-node compiled shader metadata
// ---------------------------------------------------------------------------

struct FNodeShaderData
{
	int32 PermutationIndex = -1;

	// Binding metadata from Slang reflection
	TArray<FString> InputSemantics;    // Input names in binding order
	TArray<FString> OutputSemantics;   // Output names in binding order (e.g., ["output", "convex", "concave"])
	TArray<TPair<FString, FString>> Params; // [(name, "int"|"float"), ...]
	TArray<FString> ScratchSemantics;  // Scratch buffer names in binding order (AQ_Scratch0..)

	// Multi-pass iteration count (from registry, default 1)
	uint32 Passes = 1;

	// Input names that need mipmapped textures
	TArray<FString> MipmappedInputs;

	// Per-input RGBA color bound when the input is unconnected (from the
	// node definition's input_defaults; absent = transparent black).
	TMap<FString, FLinearColor> InputDefaults;

	// Shader-permutation identity from the execution plan (opaque, Rust
	// computed). Matching hash = skip Slang compilation for this node.
	FString DefinitionHash;
};

// ---------------------------------------------------------------------------
// Graph executor — compiles + dispatches Aqueduct graphs via UE's RHI
// ---------------------------------------------------------------------------

/**
 * Executes an Aqueduct graph on the GPU using Unreal's RHI.
 *
 * Compilation (game thread): Slang → HLSL via Rust FFI, then HLSL → platform
 * shader via UE's standard compiler. Compiled shaders expose their parameters
 * so values can be updated at runtime without recompilation.
 *
 * Execution (render thread): dispatches each node's compiled compute shader
 * with input textures (SRVs), output texture (UAV), and scalar params.
 */
class AQUEDUCTRUNTIME_API FAqueductGraphExecutor
{
public:
	FAqueductGraphExecutor();
	~FAqueductGraphExecutor();

	/** Load the node registry from a TOML string. */
	bool LoadRegistry(const FString& RegistryToml);

	/** Parse a graph from a TOML string (requires registry). */
	bool LoadGraph(const FString& GraphToml);

	/** Parse a graph from a JSON string. */
	bool LoadGraphJson(const FString& GraphJson);

	/**
	 * Compile all node shaders. Call on the game thread after loading a
	 * graph. Built-in precompiled HLSL and bindings come from the node
	 * assets embedded in the Rust FFI.
	 */
	bool CompileAllShaders();

	/** Execute the graph. Uses compiled replay if available, falls back to full execute. */
	void Execute(FRHICommandListImmediate& RHICmdList);

	/** Get the output texture for a node. Returns nullptr if not yet executed. */
	FTextureRHIRef GetNodeTexture(const FString& NodeId) const;


	/** Get the final output textures (keyed by output name). */
	const TMap<FString, FTextureRHIRef>& GetOutputTextures() const { return OutputTextures; }
	const TMap<FString, FString>& GetResolvedFormats() const { return ResolvedFormats; }

	/**
	 * GAME THREAD: verify every bitmap source resolves (store/file existence;
	 * no loads, no decode). Returns false and fills OutReason when the graph
	 * must not execute (correctness contract: render exactly or not at all).
	 * Populates MissingAssetUrls for the editor module's downloader.
	 */
	bool CheckBitmapSourcesResolvable(FString& OutReason);

	/**
	 * Asset URLs referenced by bitmap nodes that were not in the project
	 * store during the last CheckBitmapSourcesResolvable (game thread only).
	 * The editor module downloads these (FHttpModule), places them in the
	 * store, and reloads the graph. The executor itself never touches the
	 * network.
	 */
	const TArray<FString>& GetMissingAssetUrls() const { return MissingAssetUrls; }

	/**
	 * True when the last execute was BLOCKED because a bitmap source was
	 * unresolvable (correctness contract: a graph never substitutes pixels;
	 * it renders exactly or not at all). Blocked executes dispatch nothing
	 * and must not be delivered or baked.
	 */
	bool IsExecutionBlocked() const { return bExecutionBlocked; }
	const FString& GetExecutionBlockReason() const { return ExecutionBlockReason; }

	/** Update a scalar parameter. Marks node dirty for compiled replay. */
	bool SetNodeParam(const FString& NodeId, const FString& ParamName, float Value);
	bool SetNodeParam(const FString& NodeId, const FString& ParamName, int32 Value);

	/** Re-parse graph TOML without clearing shaders or textures.
	 *  Use after expression/param changes that don't affect topology. */
	bool ReloadGraphKeepShaders(const FString& GraphToml);

	/**
	 * Serialize the per-node shader metadata (permutation indices, binding
	 * semantics, params, passes) to JSON. Valid after CompileAllShaders.
	 * Written into UAqueductGraph assets at cook time so shipped games can
	 * execute graphs without any shader compilation.
	 */
	FString ExportPrecompiledMetadataJson() const;

	/**
	 * Populate shader metadata from cook-time JSON instead of compiling.
	 * The shaders must already exist in the cooked global shader map at the
	 * recorded permutation indices. Call after LoadRegistry + LoadGraph.
	 * No slangc, no UE shader compilation, no disk access.
	 */
	bool LoadPrecompiledMetadata(const FString& MetadataJson);

	/**
	 * Release the graph's working GPU memory (node textures, scratch
	 * buffers, expansion/mip caches, the compiled dispatch list's RHI
	 * references) while KEEPING parsed graph state and shader metadata.
	 *
	 * This is the Live → Ready residency transition: an animated graph that
	 * nothing on screen samples costs zero VRAM. The next Execute lazily
	 * recreates everything (allocation + one dispatch — no compilation).
	 * RHI references released here are deferred-deleted safely by the RHI.
	 */
	void ReleaseGraphResources();

	/** Re-parse graph TOML, preserving NodeShaders/textures/compiler for incremental compile. */
	bool LoadGraphIncremental(const FString& GraphToml);

	/** Compile only new/changed nodes. Skips VerifyGlobalShaders if nothing changed.
	 *  Returns true on success. */
	bool CompileShadersIncremental();

	/** Access the compiled graph (for animation benchmark, etc.). */
	/**
	 * Canonical content hash of the loaded graph, computed by the Rust core
	 * over canonical structure — TOML formatting, comments, key order, node
	 * positions, and frames never affect it. Empty if no graph is loaded.
	 */
	FString GetGraphContentHash() const;

	/**
	 * Per-output dependency-closure content hashes (core-computed): output
	 * name → hash over exactly the upstream subgraph that can affect that
	 * output's pixels. The basis for per-texture staleness. Empty if no
	 * graph is loaded.
	 */
	TMap<FString, FString> GetOutputContentHashes() const;

	/** Raw graph output bindings as the core JSON union: source string,
	 *  scalar constant, or three/four-component constant. */
	FString GetOutputBindingsJson() const;

	/**
	 * Hashes for a graph TOML WITHOUT building an executor (parse-only via
	 * the core; needs the registry for parameter resolution). Used by the
	 * at-rest startup check and the pre-save hook when no session state
	 * exists. Returns false if parsing fails.
	 */
	static bool ComputeContentHashes(const FString& RegistryToml, const FString& GraphToml,
		FString& OutGraphHash, TMap<FString, FString>& OutOutputHashes);

		const FCompiledGraph* GetCompiledGraph() const { return CompiledGraph.IsValid() ? CompiledGraph.Get() : nullptr; }
	FCompiledGraph* GetCompiledGraph() { return CompiledGraph.IsValid() ? CompiledGraph.Get() : nullptr; }

	/** Get the graph resolution. */
	uint32 GetResolution() const;

	/** Get the graph's declared normal-map format: 0 = OpenGL (Y-up),
	 *  1 = DirectX (Y-down). Valid after LoadGraph. */
	int32 GetNormalFormat() const;

	/** Set/get time for expression evaluation. */
	void SetTime(float InTime) { CurrentTime = InTime; }
	float GetTime() const { return CurrentTime; }

	/** Evaluate time-dependent expressions and apply as param values.
	 *  Must be called on the game thread before Execute(). */
	void EvaluateTimeExpressions();

	/** Whether the loaded graph contains any time expressions (animated).
	 *  Valid after LoadGraph — does not require compilation. */
	bool HasTimeExpressions() const;

	/** Product GPU profiling. Uses UE RHI timestamp queries when available. */
	void SetGraphProfilingEnabled(bool bEnabled);
	void SetGraphPerNodeProfilingEnabled(bool bEnabled);
	bool IsGraphProfilingSupported() const;
	bool ResolvePendingProfileQueries(bool bWait);
	bool HasLatestProfileSnapshot() const { return bHasLatestProfileSnapshot; }
	const FAqueductProfileSnapshot& GetLatestProfileSnapshot() const { return LatestProfileSnapshot; }

	/** Benchmark/test helper: mark every compiled node dirty for the next replay. */
	void ForceAllDirty();

	/**
	 * VRAM bookkeeping over this executor's working set: node textures
	 * (intermediates, ping/pong, mips, expand/dither copies, host textures),
	 * scratch buffers, and the small shared singletons. Texture sizes are
	 * platform sizes from RHICalcTexturePlatformSize; buffer sizes are the
	 * requested byte counts. Final baked assets are engine-owned and excluded.
	 * Peak resets on topology/resolution change, mirroring node timings.
	 *
	 * Same threading contract as ReleaseGraphResources: call on the render
	 * thread, or on the game thread when no executor render work is in flight.
	 */
	FAqueductVramSnapshot GetVramSnapshot() const;

	/** Serialize a VRAM snapshot to the camelCase JSON contract shared with
	 *  the web frontend ({"currentBytes": ..., "categories": [...], ...}). */
	static FString VramSnapshotToJson(const FAqueductVramSnapshot& Snapshot);

	/** Set the directory containing the .aqueduct file (for resolving relative bitmap paths). */
	void SetGraphDirectory(const FString& InDir) { GraphDirectory = InDir; }
	const FString& GetGraphDirectory() const { return GraphDirectory; }

	/** Free all GPU resources and FFI handles. */
	void Reset();

private:
	/** Report this executor's current permutation indices to the shader
	 *  storage so unreferenced ones become recyclable. Call after each
	 *  (re)compile; Reset releases them. */
	void UpdatePermutationOwnership();

	// Directory containing the .aqueduct file (for resolving relative bitmap paths)
	FString GraphDirectory;
	// FFI handles (opaque pointers into Rust)
	LoadedRegistry* Registry = nullptr;
	LoadedGraph* Graph = nullptr;
	CompilerHandle* HLSLCompiler = nullptr;

	// Per-node compiled shader metadata (keyed by node ID)
	TMap<FString, FNodeShaderData> NodeShaders;

	// GPU resources keyed by node ID
	TMap<FString, FAqueductTexture> NodeTextures;

	// Final output textures keyed by output name
	TMap<FString, FTextureRHIRef> OutputTextures;

	// Resolved formats keyed by node ID
	TMap<FString, FString> ResolvedFormats;

	// Bitmap source cache: node ID -> the source string whose decode is
	// currently uploaded as the node's host texture. Re-decode happens only
	// when the source changes.
	TMap<FString, FString> BitmapSourceCache;

	// Content signatures for the loaded document and the nodes whose pixels
	// depend on t, both from aqueduct_graph_node_signatures_json at load.
	TMap<FString, FString> NodeSigs;
	TSet<FString> TimeCone;

	// Signature and time each node's textures were last dispatched with.
	// ExecuteFull skips a node when signature, time validity, and resident
	// textures all match: its pixels cannot have changed.
	TMap<FString, FString> LastRenderedSigs;
	TMap<FString, float> LastRenderedTimes;

	// Asset URLs missing from the project store, populated on the game
	// thread by CheckBitmapSourcesResolvable.
	TArray<FString> MissingAssetUrls;

	// Set when a bitmap source was unresolvable: the execute dispatched
	// nothing and its (stale) textures must not be delivered or baked.
	bool bExecutionBlocked = false;
	FString ExecutionBlockReason;

	// Per-node parameter values (keyed by "nodeId.paramName")
	TMap<FString, float> ParamValuesFloat;
	TMap<FString, int32> ParamValuesInt;

	// Current time for expression evaluation (seconds)
	float CurrentTime = 0.0f;

	// Whether CompileAllShaders has been called successfully
	bool bShadersCompiled = false;

	// Hash of the plan JSON the current compiled graph was built against.
	// The plan covers structure (nodes, wiring, formats, resolution, output
	// mapping) but never parameter values, so param edits replay while any
	// structural change rebuilds.
	uint32 TopologyHash = 0;

	struct FScratchDeclInfo
	{
		uint32 Stride = 4;
		uint32 Count = 1;
	};
	struct FNodeDispatchSize
	{
		uint32 Width = 1;
		uint32 Height = 1;
	};

	/** One node of the Rust-computed execution plan: every semantic fact the
	 *  dispatch paths need (formats, passes, wiring, defaults, scratch
	 *  sizing, dirty hash), from aqueduct_graph_execution_plan_json. */
	struct FExecutionPlanNode
	{
		FString Kind;
		FString Format;        // resolved output format token
		FString ComputeFormat; // compute-texture format (narrow formats compute wide)
		TMap<FString, FString> Formats;        // semantic -> destination format
		TMap<FString, FString> ComputeFormats; // semantic -> dispatch format
		uint32 Passes = 1;
		TArray<FNodeDispatchSize> Dispatch;
		TArray<FString> Outputs;
		TMap<FString, FString> Inputs; // semantic -> resolved source texture key
		TMap<FString, FLinearColor> InputDefaults;
		TArray<FString> MipmappedInputs;
		TMap<FString, FScratchDeclInfo> Scratch; // keyed by scratch name
		bool bDither8Bit = false;
		TMap<FString, bool> Dither8BitOutputs;
		FString DefinitionHash;
	};

	TArray<FString> PlanOrder;                  // topological execution order
	TMap<FString, FExecutionPlanNode> PlanNodes;
	TMap<FString, FString> PlanOutputs;         // output name -> texture key
	/** Actual producer outputs that carry the canonical in-place mip pyramid. */
	TSet<FString> MipRequiredTextureKeys;
	uint32 PlanHash = 0;                        // hash of the plan JSON

	// -- Bounded execution ------------------------------------------------
	// The Rust plan carries texture lifetimes and a static reuse table;
	// this executor only obeys them (Rust computes, C++ obeys). Engaged
	// only when the predicted resident set exceeds the tracked budget;
	// below budget nothing changes.

	/** One bounded plan: memory-aware order, pool specs, key->slot
	 *  assignment, and per-step free lists. */
	struct FBoundedPlan
	{
		TArray<FString> Order;
		TArray<TPair<FString, bool>> Pool; // (format token, mipped)
		TMap<FString, int32> Assign;       // logical key -> pool slot
		TArray<TArray<FString>> FreeAfter; // keys dead after each step
		bool IsValid() const { return Order.Num() > 0; }
	};
	FBoundedPlan OneShotPlan;   // full graph, one-shot renders (bakes)
	FBoundedPlan FramePlan;     // animated cone, per-frame playback
	/** Predicted full-resident bytes for the loaded plan (textures only). */
	uint64 PredictedResidentBytes = 0;
	/** logical key -> physical pool key; non-empty only while a bounded
	 *  execution mode is engaged. */
	TMap<FString, FString> PoolRemap;
	/** Resolve a logical texture key through the pool remap. */
	const FString& ResolvePoolKey(const FString& Key) const
	{
		const FString* Phys = PoolRemap.Find(Key);
		return Phys ? *Phys : Key;
	}
	/** Convert the shared plan's `node.semantic` spelling to the external
	 *  executor's bare `node` spelling for single-output nodes. */
	FString RuntimeTextureKey(const FString& PlanKey) const;
	/** True when the loaded plan's resident prediction exceeds the tracked
	 *  budget: bounded modes engage, mirroring the native engine's rule. */
	bool ShouldEngageBounded() const;
	/** Pre-create the mip-chained pool slots of a bounded plan (plain slots
	 *  materialize lazily through GetOrCreateNodeTexture). */
	void CreateMippedPoolSlots(const FBoundedPlan& Plan, const TCHAR* Prefix, uint32 Resolution);

	/** Fetch and parse the execution plan for the loaded graph. Called after
	 *  every (re)parse; everything downstream reads the plan, never the graph. */
	bool LoadExecutionPlan();

	// Time-dependent params: slot layout fixed at plan load, one float per
	// slot evaluated per frame by Rust into a preallocated buffer.
	TArray<TPair<FString, FString>> TimeParamSlots; // (node id, param name)
	TArray<float> TimeParamValues;

	// 1x1 white placeholder texture for SRV slots the shader never reads.
	// Prevents GPU hang from reading unbound descriptors.
	FTextureRHIRef PlaceholderTexture;
	FShaderResourceViewRHIRef PlaceholderSRV;

	/** Get or lazily create the 1x1 white placeholder texture + SRV. */
	FShaderResourceViewRHIRef GetPlaceholderSRV(FRHICommandListImmediate& RHICmdList);

	// 1x1 default-color textures for unconnected declared inputs: the node
	// definition's input_defaults color, else transparent black. Keyed by
	// color; matches the Rust engine's unconnected-input defaults.
	TMap<FString, TPair<FTextureRHIRef, FShaderResourceViewRHIRef>> DefaultColorSRVs;

	/**
	 * Get or lazily create the default-color texture + SRV for a color.
	 * Non-zero colors are Resolution-sized (shaders Load() at pixel
	 * coordinates); the zero color stays 1x1.
	 */
	FShaderResourceViewRHIRef GetDefaultColorSRV(FRHICommandListImmediate& RHICmdList, const FLinearColor& Color, uint32 Resolution);

	/** Dispatch dithered float16→rgba8 conversion. */
	void DispatchDitherConversion(
		FRHICommandListImmediate& RHICmdList,
		const FString& SourceKey,
		const FString& DestKey,
		uint32 Resolution);

	/** Regenerate the lower levels of an output's canonical in-place pyramid. */
	void GenerateMipmapsInPlace(
		FRHICommandListImmediate& RHICmdList,
		const FTextureRHIRef& Texture,
		uint32 Resolution) const;
	void GenerateRequiredMipmapsForNode(
		FRHICommandListImmediate& RHICmdList,
		const FString& NodeId,
		uint32 Resolution) const;

	// Blue noise texture for dithered rgba8 conversion (64x64 RGBA8)
	FTextureRHIRef BlueNoiseTexture;
	FShaderResourceViewRHIRef BlueNoiseSRV;
	FShaderResourceViewRHIRef GetBlueNoiseSRV(FRHICommandListImmediate& RHICmdList);

	// Shared scratch storage pool. Node execution is strictly serial and
	// every node-execution starts from a zero-filled state, so scratch
	// buffers are pooled instead of per-node: one buffer per
	// (slot index, element stride), grow-only (a cached view or compiled
	// dispatch references the buffer object, so it must keep its identity
	// until the next topology change rebuilds the compiled graph).
	// `Views` caches count-sized UAVs over the buffer; nodes bind and
	// zero-fill the view matching their own declared element count, so
	// per-execution clear cost matches the old per-node buffers.
	struct FSharedScratchSlot
	{
		FBufferRHIRef Buffer;
		uint32 SizeBytes = 0;
		uint32 Stride = 4;
		TMap<uint32, FUnorderedAccessViewRHIRef> Views;
	};
	// Key: (slot index << 8) | stride. Strides are 4 (scalar) or 16 (vec4).
	TMap<uint32, FSharedScratchSlot> SharedScratch;

	/** (stride, element count) per ScratchSemantics slot, from the plan.
	 *  Index-aligned with ScratchSemantics. */
	TArray<FScratchDeclInfo> ResolveScratchDecls(
		const FString& NodeId, const FNodeShaderData& ShaderData) const;

	/** Get (creating or growing as needed) the shared scratch UAV for a slot,
	 *  sized to exactly `Count` elements of `Stride` bytes. */
	FUnorderedAccessViewRHIRef GetSharedScratchUAV(
		FRHICommandListImmediate& RHICmdList, int32 Slot, uint32 Stride, uint32 Count);

	/** Zero-fill a scratch UAV. Called once per node-execution before first pass. */
	void ZeroFillScratchUAV(
		FRHICommandListImmediate& RHICmdList, const FUnorderedAccessViewRHIRef& UAV);

	// 1-element placeholder UAV for unused AQ_Scratch slots (every shader
	// permutation has 8 scratch slot declarations; unused ones bind this).
	FBufferRHIRef ScratchPlaceholderBuffer;
	FUnorderedAccessViewRHIRef ScratchPlaceholderUAV;
	FUnorderedAccessViewRHIRef GetScratchPlaceholderUAV(FRHICommandListImmediate& RHICmdList);

	/** Get or create a texture for a node output with the given format.
	 *  TextureKey is "nodeId" for single-output nodes, "nodeId.outputName" for multi-output. */
	FTextureRHIRef GetOrCreateNodeTexture(
		const FString& TextureKey,
		const FString& Format,
		uint32 Resolution,
		bool bMipmapped = false);

	/** Dispatch a dynamically compiled node shader. */
	void DispatchDynamicNode(
		FRHICommandListImmediate& RHICmdList,
		const FString& NodeId,
		const FNodeShaderData& ShaderData,
		uint32 Resolution,
		uint32 PassIndex = 0
	);
	FNodeDispatchSize ResolveNodeDispatchSize(
		const FString& NodeId,
		uint32 PassIndex,
		uint32 Resolution
	) const;

	/** Dispatch a multi-pass node with ping-pong buffers. */
	void DispatchMultiPassNode(
		FRHICommandListImmediate& RHICmdList,
		const FString& NodeId,
		const FNodeShaderData& ShaderData,
		uint32 Resolution
	);

	/** Dispatch using the static test shader (fallback). */
	void DispatchTestNode(
		FRHICommandListImmediate& RHICmdList,
		const FString& NodeId,
		uint32 Resolution
	);

	/** Dispatch a compute-shader blit that reads any-format source via texture Load
	 *  and writes to a UAV, letting the hardware convert on write. Used to go from
	 *  rgba32float multi-pass ping/pong buffers to the node's declared output format
	 *  (rgba16f, r32f, rgba8unorm). Mirrors the wgpu engine's
	 *  dispatch_multipass_format_convert. */
	void DispatchFormatConvert(
		FRHICommandListImmediate& RHICmdList,
		FTextureRHIRef SrcTexture,
		FTextureRHIRef DstTexture,
		uint32 Resolution) const;

	/** Remove NodeShaders/NodeTextures entries for nodes not in ValidNodeIds. */
	void RemoveDeletedNodes(const TSet<FString>& ValidNodeIds);

	/** Parse shader compilation result JSON from FFI. */
	static bool ParseShaderResultJson(
		const FString& Json,
		FString& OutSource,
		TArray<FString>& OutInputSemantics,
		TArray<FString>& OutOutputSemantics,
		TArray<TPair<FString, FString>>& OutParams,
		TArray<FString>& OutScratchSemantics
	);

	/** Get the last FFI error as an FString, or empty if none. */
	static FString GetLastError();

	// ---------------------------------------------------------------------------
	// Compiled graph (built once, replayed every frame)
	// ---------------------------------------------------------------------------

	TUniquePtr<FCompiledGraph> CompiledGraph;

	struct FCachedNodeTiming
	{
		FString Kind;
		double GpuMs = 0.0;
		uint32 Dispatches = 0;
		uint32 Passes = 1;
	};

	struct FPendingProfileRecord
	{
		uint32 NodeIndex = 0;
		uint32 Dispatches = 0;
		uint32 TotalPasses = 1;
		FRenderQueryRHIRef StartQuery;
		FRenderQueryRHIRef EndQuery;
	};

	/** High-water mark of the working set's bytes. Follows the profile
	 *  cache's invalidation discipline: reset on topology/resolution change
	 *  and on ReleaseGraphResources; persists across param-only edits. */
	mutable uint64 PeakVramBytes = 0;

	/** Sum working-set bytes without building the full snapshot. */
	uint64 ComputeCurrentVramBytes() const;

	/** Fold the current working-set total into PeakVramBytes. Called at the
	 *  end of Execute, after any allocations for the frame. */
	void UpdateVramPeak();

	bool bGraphProfilingEnabled = false;
	bool bGraphPerNodeProfilingEnabled = false;
	bool bHasLatestProfileSnapshot = false;
	uint64 ProfileGraphRevision = 0;
	uint64 ProfileRenderRevision = 0;
	TArray<FPendingProfileRecord> PendingProfileRecords;
	TMap<FString, FCachedNodeTiming> ProfileNodeTimings;
	FAqueductProfileSnapshot LatestProfileSnapshot;
	void InvalidateProfileCache();
	void BuildUnsupportedProfileSnapshot();
	void BuildProfileSnapshotFromCache(double ActualGpuMs, bool bHasActualGpuMs, const TSet<FString>& MeasuredNodeIds);

	/** Full execution path (first frame, builds textures + dispatches). */
	void ExecuteFull(FRHICommandListImmediate& RHICmdList);

	/** Reload NodeSigs/TimeCone from the FFI for the current graph. */
	void RefreshNodeSignatures();

	/** Whether ExecuteFull can skip this node's dispatch entirely. */
	bool CanSkipNode(const FString& NodeId, const FNodeShaderData* ShaderData) const;

public:
	/** Dispatch/skip counts from the most recent ExecuteFull (edit gates). */
	int32 GetLastFullDispatchedNodes() const { return LastFullDispatchedNodes; }
	int32 GetLastFullSkippedNodes() const { return LastFullSkippedNodes; }

private:
	int32 LastFullDispatchedNodes = 0;
	int32 LastFullSkippedNodes = 0;

	/** Build the compiled dispatch list from the current graph + shaders. */
	void BuildCompiledGraph(FRHICommandListImmediate& RHICmdList);

	/** Replay the compiled dispatch list (fast path). */
	void ReplayCompiledGraph(FRHICommandListImmediate& RHICmdList);
};

} // namespace UE::Aqueduct
