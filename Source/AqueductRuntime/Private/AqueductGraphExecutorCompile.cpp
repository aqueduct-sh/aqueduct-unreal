// AqueductGraphExecutor shader compilation: Slang -> HLSL via Rust FFI,
// permutation registration, incremental recompiles keyed on the plan's
// definition hashes, and cooked-game precompiled metadata.

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

void FAqueductGraphExecutor::RemoveDeletedNodes(const TSet<FString>& ValidNodeIds)
{
	TArray<FString> DeletedNodes;
	for (const auto& Pair : NodeShaders)
	{
		if (!ValidNodeIds.Contains(Pair.Key))
		{
			DeletedNodes.Add(Pair.Key);
		}
	}
	for (const FString& NodeId : DeletedNodes)
	{
		NodeShaders.Remove(NodeId);
	}

	// Evict every texture owned by a deleted node: the plain output key,
	// multi-output "{node}.{name}", expand/mip/dither copies, and host
	// textures. Ownership comes from the same key classification the VRAM
	// snapshot uses; unowned keys (the shared multipass pair, placeholders)
	// are never evicted. Without this, deleted nodes' textures stay
	// resident until the next ReleaseGraphResources.
	TArray<FString> DoomedTextures;
	TSet<FString> LivePassResources;
	for (const auto& Node : PlanNodes)
	{
		if (!Node.Value.UsesStaging()) continue;
		for (const auto& Pass : Node.Value.SizedPasses)
		{
			for (const auto& Output : Pass.Outputs)
			{
				LivePassResources.Add(Output.Value);
			}
		}
	}
	for (const auto& Pair : NodeTextures)
	{
		if (Pair.Key.StartsWith(TEXT("_multipass.")) && !LivePassResources.Contains(Pair.Key))
		{
			DoomedTextures.Add(Pair.Key);
			continue;
		}
		FString OwnerNode;
		ClassifyVramTextureKey(Pair.Key, OwnerNode);
		if (!OwnerNode.IsEmpty() && !ValidNodeIds.Contains(OwnerNode))
		{
			DoomedTextures.Add(Pair.Key);
		}
	}
	for (const FString& Key : DoomedTextures)
	{
		NodeTextures.Remove(Key);
	}

	for (const FString& NodeId : DeletedNodes)
	{
		BitmapSourceCache.Remove(NodeId);
	}
}

// ---------------------------------------------------------------------------
// Incremental load + compile (preserves cached shaders across reloads)
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::LoadGraphIncremental(const FString& GraphToml)
{
	if (!Registry)
	{
		UE_LOG(LogAqueduct, Error, TEXT("LoadGraphIncremental: no registry loaded"));
		return false;
	}

	if (Graph)
	{
		aqueduct_graph_free(Graph);
		Graph = nullptr;
	}

	// Parse new graph. Runtime scalar overrides belong to the previous parsed
	// document; the incoming TOML is authoritative after a structural/full
	// reload. Time expressions repopulate their values before execution.
	ParamValuesFloat.Empty();
	ParamValuesInt.Empty();
	FTCHARToUTF8 Utf8(*GraphToml);
	Graph = aqueduct_graph_parse_toml(Utf8.Get(), Registry);
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("LoadGraphIncremental: failed to parse graph: %s"), *GetLastError());
		return false;
	}

	// Topology may have changed — invalidate compiled dispatch list
	OutputTextures.Empty();
	ResolvedFormats.Empty();
	CompiledGraph.Reset();
	InvalidateProfileCache();

	// The R32→RGBA expansion SRVs cache CONTENT, not just views: a cached
	// entry short-circuits the expansion dispatch, so after a reload every
	// consumer of an r32float node would read the expansion of the
	// PRE-reload pixels (stale image downstream of any r32float source —
	// e.g. cell noises — even though the source itself re-dispatched).

	UE_LOG(LogAqueduct, Log, TEXT("Graph loaded incrementally: %d nodes, resolution %d"),
		aqueduct_graph_node_count(Graph), aqueduct_graph_resolution(Graph));
	RefreshNodeSignatures();
	return LoadExecutionPlan();
}

bool FAqueductGraphExecutor::CompileShadersIncremental()
{
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("CompileShadersIncremental: no graph loaded"));
		return false;
	}

	const TArray<FString>& ExecutionOrder = PlanOrder;
	TSet<FString> NewGraphNodes(ExecutionOrder);

	// Layer 1: the plan's definition hash is shader-permutation identity.
	// A matching stored hash skips Slang entirely.
	TArray<FString> NodesToCompile;
	for (const FString& NodeId : ExecutionOrder)
	{
		const FExecutionPlanNode& Plan = PlanNodes[NodeId];
		const FNodeShaderData* Existing = NodeShaders.Find(NodeId);
		if (!Existing || Existing->DefinitionHash != Plan.DefinitionHash)
		{
			NodesToCompile.Add(NodeId);
		}
	}

	const int32 CachedCount = ExecutionOrder.Num() - NodesToCompile.Num();

	if (NodesToCompile.Num() == 0)
	{
		RemoveDeletedNodes(NewGraphNodes);

		UE_LOG(LogAqueduct, Log, TEXT("Incremental compile: all %d nodes cached — skipping Slang + VerifyGlobalShaders entirely"),
			CachedCount);
		CompiledGraph.Reset();
		InvalidateProfileCache();
		bShadersCompiled = true;
		UpdatePermutationOwnership();
		return true;
	}

	// -----------------------------------------------------------------------
	// Layer 2: Slang compilation for changed/new nodes only
	// -----------------------------------------------------------------------

	if (!HLSLCompiler)
	{
		HLSLCompiler = aqueduct_compiler_new_hlsl(nullptr);
		if (!HLSLCompiler)
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to create HLSL compiler: %s"), *GetLastError());
			return false;
		}
	}

	int32 RecompiledCount = 0;
	int32 NewCount = 0;

	// Snapshot the registration epoch before this compile. Epoch (not index
	// growth) is the correct "needs VerifyGlobalShaders" signal: recycling
	// reassigns an existing index to new HLSL without growing NextIndex.
	const int32 EpochBefore = FAqueductNodeShaderStorage::RegistrationEpoch();

	for (const FString& NodeId : NodesToCompile)
	{
		FTCHARToUTF8 NodeIdUtf8(*NodeId);

		// Call Rust FFI: Slang → HLSL + bindings. Built-in precompiled HLSL
		// comes from the embedded node assets; custom shaders are read from
		// the loaded graph's [custom_nodes] table inside Rust.
		const FString JsonStr = FfiToString(aqueduct_compile_node_shader(
			HLSLCompiler, Graph, Registry, NodeIdUtf8.Get()));
		if (JsonStr.IsEmpty())
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to compile node %s: %s"), *NodeId, *GetLastError());
			return false;
		}

		FString HLSLSource;
		TArray<FString> InputSemantics;
		TArray<FString> OutputSemantics;
		TArray<TPair<FString, FString>> Params;
		TArray<FString> ScratchSemantics;
		if (!ParseShaderResultJson(JsonStr, HLSLSource, InputSemantics, OutputSemantics, Params, ScratchSemantics))
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to parse shader result for node %s"), *NodeId);
			return false;
		}

		// HLSL arrives already normalized onto the fixed AQ_* FParameters ABI by the
		// Rust compiler (reflection-driven); the plugin no longer rewrites shaders.
		const FString& ProcessedHLSL = HLSLSource;

		// Register with UE's permutation system (dedup identical HLSL)
		int32 PermIdx = FAqueductNodeShaderStorage::RegisterOrReuse(ProcessedHLSL);
		if (PermIdx == INDEX_NONE)
		{
			UE_LOG(LogAqueduct, Error,
				TEXT("Node %s: no shader permutation available — compile aborted"), *NodeId);
			return false;
		}

		FNodeShaderData* ExistingData = NodeShaders.Find(NodeId);
		bool bIsRecompile = ExistingData != nullptr;

		FNodeShaderData ShaderData;
		ShaderData.PermutationIndex = PermIdx;
		ShaderData.InputSemantics = MoveTemp(InputSemantics);
		ShaderData.OutputSemantics = MoveTemp(OutputSemantics);
		ShaderData.Params = MoveTemp(Params);
		ShaderData.ScratchSemantics = MoveTemp(ScratchSemantics);
		const FExecutionPlanNode& Plan = PlanNodes[NodeId];
		ShaderData.Passes = Plan.Passes;
		ShaderData.DefinitionHash = Plan.DefinitionHash;
		ShaderData.MipmappedInputs = Plan.MipmappedInputs;
		ShaderData.InputDefaults = Plan.InputDefaults;
		NodeShaders.Add(NodeId, MoveTemp(ShaderData));

		if (bIsRecompile)
		{
			RecompiledCount++;
			UE_LOG(LogAqueduct, Log, TEXT("Node %s: recompiled"), *NodeId);
		}
		else
		{
			NewCount++;
			UE_LOG(LogAqueduct, Log, TEXT("Node %s: new (perm %d)"), *NodeId, PermIdx);
		}
	}

	RemoveDeletedNodes(NewGraphNodes);

	// Call VerifyGlobalShaders if any index got new HLSL this pass (fresh
	// allocation OR recycle — both bump the epoch; index growth alone would
	// miss recycles and leave the recycled index uncompiled → stale shader).
	const bool bNewPermutationsRegistered =
		FAqueductNodeShaderStorage::RegistrationEpoch() != EpochBefore;

	if (bNewPermutationsRegistered && !IsRunningCookCommandlet())
	{
		UE_LOG(LogAqueduct, Log, TEXT("Incremental compile: %d cached, %d recompiled, %d new — triggering VerifyGlobalShaders"),
			CachedCount, RecompiledCount, NewCount);
		VerifyGlobalShaders(GMaxRHIShaderPlatform, nullptr, false);
	}
	else
	{
		UE_LOG(LogAqueduct, Log, TEXT("Incremental compile: %d cached, %d recompiled, %d new — VerifyGlobalShaders SKIPPED"),
			CachedCount, RecompiledCount, NewCount);
	}

	CompiledGraph.Reset();
	InvalidateProfileCache();
	bShadersCompiled = true;
	UpdatePermutationOwnership();
	return true;
}

// ---------------------------------------------------------------------------
// Parse FFI shader result JSON
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::ParseShaderResultJson(
	const FString& Json,
	FString& OutSource,
	TArray<FString>& OutInputSemantics,
	TArray<FString>& OutOutputSemantics,
	TArray<TPair<FString, FString>>& OutParams,
	TArray<FString>& OutScratchSemantics)
{
	TSharedPtr<FJsonObject> Root = ParseJsonObject(Json);
	if (!Root.IsValid())
	{
		UE_LOG(LogAqueduct, Error, TEXT("Failed to parse shader result JSON"));
		return false;
	}

	Root->TryGetStringField(TEXT("source"), OutSource);  // FFI JSON: non-throwing

	const TSharedPtr<FJsonObject>* BindingsObj;
	if (!Root->TryGetObjectField(TEXT("bindings"), BindingsObj))
	{
		UE_LOG(LogAqueduct, Error, TEXT("Shader result missing 'bindings'"));
		return false;
	}

	// Parse inputs: { "background": 0, "foreground": 1, ... }
	// Sort by binding index to get deterministic order
	const TSharedPtr<FJsonObject>* InputsObj;
	if ((*BindingsObj)->TryGetObjectField(TEXT("inputs"), InputsObj))
	{
		TArray<TPair<FString, int32>> InputPairs;
		for (const auto& Pair : (*InputsObj)->Values)
		{
			InputPairs.Add(TPair<FString, int32>(Pair.Key, (int32)Pair.Value->AsNumber()));
		}
		InputPairs.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) {
			return A.Value < B.Value;
		});
		for (const auto& Pair : InputPairs)
		{
			OutInputSemantics.Add(Pair.Key);
		}
	}

	// Parse outputs: { "output": 0, "convex": 1, "concave": 2 }
	// Sort by binding index to get deterministic order
	const TSharedPtr<FJsonObject>* OutputsObj;
	if ((*BindingsObj)->TryGetObjectField(TEXT("outputs"), OutputsObj))
	{
		TArray<TPair<FString, int32>> OutputPairs;
		for (const auto& Pair : (*OutputsObj)->Values)
		{
			OutputPairs.Add(TPair<FString, int32>(Pair.Key, (int32)Pair.Value->AsNumber()));
		}
		OutputPairs.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) {
			return A.Value < B.Value;
		});
		for (const auto& Pair : OutputPairs)
		{
			OutOutputSemantics.Add(Pair.Key);
		}
	}

	// Parse params: [["opacity", "float"], ["mode", "int"], ...]
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if ((*BindingsObj)->TryGetArrayField(TEXT("params"), ParamsArray))
	{
		for (const auto& ParamVal : *ParamsArray)
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = ParamVal->AsArray();
			if (Pair.Num() >= 2)
			{
				OutParams.Add(TPair<FString, FString>(Pair[0]->AsString(), Pair[1]->AsString()));
			}
		}
	}

	// Parse scratch: [["counter", 0], ["seeds", 1], ...]
	// Sort by binding index; order determines which AQ_ScratchN slot each binds to.
	const TArray<TSharedPtr<FJsonValue>>* ScratchArray;
	if ((*BindingsObj)->TryGetArrayField(TEXT("scratch"), ScratchArray))
	{
		TArray<TPair<FString, int32>> ScratchPairs;
		for (const auto& Entry : *ScratchArray)
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = Entry->AsArray();
			if (Pair.Num() >= 2)
			{
				ScratchPairs.Add(TPair<FString, int32>(Pair[0]->AsString(), (int32)Pair[1]->AsNumber()));
			}
		}
		ScratchPairs.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) {
			return A.Value < B.Value;
		});
		for (const auto& Pair : ScratchPairs)
		{
			OutScratchSemantics.Add(Pair.Key);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Precompiled metadata (cooked-game path: no shader compilation possible)
// ---------------------------------------------------------------------------

FString FAqueductGraphExecutor::ExportPrecompiledMetadataJson() const
{
	TArray<TSharedPtr<FJsonValue>> NodeArray;
	for (const auto& Pair : NodeShaders)
	{
		const FNodeShaderData& Data = Pair.Value;
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("nodeId"), Pair.Key);
		Node->SetStringField(TEXT("definitionHash"), Data.DefinitionHash);
		Node->SetNumberField(TEXT("permutationIndex"), Data.PermutationIndex);

		auto ToStringArray = [](const TArray<FString>& Items)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& S : Items) Out.Add(MakeShared<FJsonValueString>(S));
			return Out;
		};
		Node->SetArrayField(TEXT("inputs"), ToStringArray(Data.InputSemantics));
		Node->SetArrayField(TEXT("outputs"), ToStringArray(Data.OutputSemantics));
		Node->SetArrayField(TEXT("scratch"), ToStringArray(Data.ScratchSemantics));

		TArray<TSharedPtr<FJsonValue>> ParamArray;
		for (const auto& Param : Data.Params)
		{
			TArray<TSharedPtr<FJsonValue>> Entry;
			Entry.Add(MakeShared<FJsonValueString>(Param.Key));
			Entry.Add(MakeShared<FJsonValueString>(Param.Value));
			ParamArray.Add(MakeShared<FJsonValueArray>(Entry));
		}
		Node->SetArrayField(TEXT("params"), ParamArray);

		NodeArray.Add(MakeShared<FJsonValueObject>(Node));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 3);
	Root->SetStringField(TEXT("shaderCompilationId"), FfiToString(aqueduct_shader_compilation_id()));
	Root->SetArrayField(TEXT("nodes"), NodeArray);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

bool FAqueductGraphExecutor::LoadPrecompiledMetadata(const FString& MetadataJson)
{
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("LoadPrecompiledMetadata: no graph loaded"));
		return false;
	}

	TSharedPtr<FJsonObject> Root = ParseJsonObject(MetadataJson);
	if (!Root.IsValid())
	{
		UE_LOG(LogAqueduct, Error, TEXT("LoadPrecompiledMetadata: malformed JSON"));
		return false;
	}

	double Version = 0;
	FString CompilationId;
	const FString CurrentCompilationId = FfiToString(aqueduct_shader_compilation_id());
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 3 ||
		!Root->TryGetStringField(TEXT("shaderCompilationId"), CompilationId) ||
		CurrentCompilationId.IsEmpty() || CompilationId != CurrentCompilationId)
	{
		UE_LOG(LogAqueduct, Error, TEXT("Cooked Aqueduct shaders are incompatible with this runtime. Recook the asset with the current Aqueduct plugin."));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NodeArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("nodes"), NodeArray) || !NodeArray)
	{
		UE_LOG(LogAqueduct, Error, TEXT("LoadPrecompiledMetadata: missing nodes array"));
		return false;
	}

	auto ToStrings = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Field, Arr) && Arr)
		{
			for (const auto& V : *Arr) Out.Add(V->AsString());
		}
		return Out;
	};

	TMap<FString, FNodeShaderData> LoadedShaders;
	for (const TSharedPtr<FJsonValue>& Entry : *NodeArray)
	{
		const TSharedPtr<FJsonObject> Node = Entry->AsObject();
		if (!Node.IsValid()) return false;

		FNodeShaderData Data;
		Data.PermutationIndex = (int32)Node->GetNumberField(TEXT("permutationIndex"));
		Data.InputSemantics = ToStrings(Node, TEXT("inputs"));
		Data.OutputSemantics = ToStrings(Node, TEXT("outputs"));
		Data.ScratchSemantics = ToStrings(Node, TEXT("scratch"));

		const TArray<TSharedPtr<FJsonValue>>* ParamArray = nullptr;
		if (Node->TryGetArrayField(TEXT("params"), ParamArray) && ParamArray)
		{
			for (const auto& P : *ParamArray)
			{
				const TArray<TSharedPtr<FJsonValue>>& Pair = P->AsArray();
				if (Pair.Num() >= 2)
				{
					Data.Params.Add(TPair<FString, FString>(Pair[0]->AsString(), Pair[1]->AsString()));
				}
			}
		}

		FString MetaNodeId;
		Node->TryGetStringField(TEXT("nodeId"), MetaNodeId);  // cooked metadata: non-throwing
		const FExecutionPlanNode* Plan = PlanNodes.Find(MetaNodeId);
		FString DefinitionHash;
		if (!Plan || LoadedShaders.Contains(MetaNodeId) ||
			!Node->TryGetStringField(TEXT("definitionHash"), DefinitionHash) ||
			DefinitionHash != Plan->DefinitionHash)
		{
			UE_LOG(LogAqueduct, Error, TEXT("Cooked Aqueduct shader for '%s' does not match its node definition. Recook the asset."), *MetaNodeId);
			return false;
		}
		Data.Passes = Plan->Passes;
		Data.MipmappedInputs = Plan->MipmappedInputs;
		Data.InputDefaults = Plan->InputDefaults;
		Data.DefinitionHash = DefinitionHash;
		LoadedShaders.Add(MetaNodeId, MoveTemp(Data));
	}
	if (LoadedShaders.Num() != PlanOrder.Num())
	{
		UE_LOG(LogAqueduct, Error, TEXT("Cooked Aqueduct shader metadata has an incomplete node set. Recook the asset."));
		return false;
	}
	NodeShaders = MoveTemp(LoadedShaders);

	// Shaders already live in the cooked global shader map at these
	// permutation indices — no compilation, no VerifyGlobalShaders.
	bShadersCompiled = true;
	UpdatePermutationOwnership();
	UE_LOG(LogAqueduct, Log, TEXT("Loaded precompiled metadata for %d nodes"), NodeShaders.Num());
	return true;
}

// ---------------------------------------------------------------------------
// Shader Compilation (game thread)
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::CompileAllShaders()
{
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("CompileAllShaders: no graph loaded"));
		return false;
	}

	if (!HLSLCompiler)
	{
		HLSLCompiler = aqueduct_compiler_new_hlsl(nullptr);
		if (!HLSLCompiler)
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to create HLSL compiler: %s"), *GetLastError());
			return false;
		}
	}

	const TArray<FString>& ExecutionOrder = PlanOrder;
	UE_LOG(LogAqueduct, Log, TEXT("Compiling shaders for %d nodes"), ExecutionOrder.Num());

	// Clear previous registrations (preserves dedup cache for RegisterOrReuse)
	FAqueductNodeShaderStorage::ClearForReload();
	NodeShaders.Empty();

	// Compile each node
	for (const FString& NodeId : ExecutionOrder)
	{
		FTCHARToUTF8 NodeIdUtf8(*NodeId);

		// Call Rust FFI: Slang → HLSL + bindings. Built-in precompiled HLSL
		// comes from the embedded node assets; custom shaders are read from
		// the loaded graph's [custom_nodes] table inside Rust.
		const FString JsonStr = FfiToString(aqueduct_compile_node_shader(
			HLSLCompiler, Graph, Registry, NodeIdUtf8.Get()));
		if (JsonStr.IsEmpty())
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to compile node %s: %s"), *NodeId, *GetLastError());
			return false;
		}

		FString HLSLSource;
		TArray<FString> InputSemantics;
		TArray<FString> OutputSemantics;
		TArray<TPair<FString, FString>> Params;
		TArray<FString> ScratchSemantics;
		if (!ParseShaderResultJson(JsonStr, HLSLSource, InputSemantics, OutputSemantics, Params, ScratchSemantics))
		{
			UE_LOG(LogAqueduct, Error, TEXT("Failed to parse shader result for node %s"), *NodeId);
			return false;
		}

		// HLSL arrives already normalized onto the fixed AQ_* FParameters ABI by the
		// Rust compiler (reflection-driven); the plugin no longer rewrites shaders.
		const FString& ProcessedHLSL = HLSLSource;

		UE_LOG(LogAqueduct, Log, TEXT("Node %s: %d inputs, %d params, %d outputs, HLSL=%d chars"),
			*NodeId, InputSemantics.Num(), Params.Num(), OutputSemantics.Num(), ProcessedHLSL.Len());

		// Register with UE's permutation system (dedup identical HLSL)
		int32 PermIdx = FAqueductNodeShaderStorage::RegisterOrReuse(ProcessedHLSL);
		if (PermIdx == INDEX_NONE)
		{
			UE_LOG(LogAqueduct, Error,
				TEXT("Node %s: no shader permutation available — compile aborted"), *NodeId);
			return false;
		}

		const FExecutionPlanNode& Plan = PlanNodes[NodeId];
		FNodeShaderData ShaderData;
		ShaderData.PermutationIndex = PermIdx;
		ShaderData.InputSemantics = MoveTemp(InputSemantics);
		ShaderData.OutputSemantics = MoveTemp(OutputSemantics);
		ShaderData.Params = MoveTemp(Params);
		ShaderData.ScratchSemantics = MoveTemp(ScratchSemantics);
		ShaderData.Passes = Plan.Passes;
		ShaderData.DefinitionHash = Plan.DefinitionHash;
		ShaderData.MipmappedInputs = Plan.MipmappedInputs;
		ShaderData.InputDefaults = Plan.InputDefaults;
		NodeShaders.Add(NodeId, MoveTemp(ShaderData));
	}

	// Trigger UE shader compilation for new permutations.
	// VerifyGlobalShaders checks ShouldCompilePermutation() for each registered
	// shader type and compiles any permutations that are missing from the shader map.
	//
	// In the cook commandlet there is no host RHI global shader map
	// (NullRHI) — and none is needed: registration into PendingHLSL is what
	// matters; the cooker compiles the TARGET platform's global shader map
	// itself, consulting ShouldCompilePermutation against the storage.
	if (!IsRunningCookCommandlet())
	{
		UE_LOG(LogAqueduct, Log, TEXT("Triggering shader compilation for %d permutations..."), NodeShaders.Num());
		VerifyGlobalShaders(GMaxRHIShaderPlatform, nullptr, false);
	}
	else
	{
		UE_LOG(LogAqueduct, Log, TEXT("Cook: registered %d node shader permutations (target compile deferred to cooker)"),
			NodeShaders.Num());
	}

	bShadersCompiled = true;
	UpdatePermutationOwnership();
	TopologyHash = PlanHash;
	UE_LOG(LogAqueduct, Log, TEXT("All shaders compiled successfully"));
	return true;
}


} // namespace UE::Aqueduct
