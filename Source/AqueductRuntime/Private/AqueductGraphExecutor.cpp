// AqueductGraphExecutor core: lifecycle, graph/registry loading, the
// Rust-computed execution plan, parameters, time, and texture bookkeeping.
// Compilation lives in AqueductGraphExecutorCompile.cpp, dispatch in
// AqueductGraphExecutorExecute.cpp, the replay fast path in
// AqueductGraphExecutorCompiled.cpp, and profiling/VRAM diagnostics in
// AqueductGraphExecutorDiagnostics.cpp.

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

DEFINE_LOG_CATEGORY(LogAqueduct);

namespace UE::Aqueduct
{

FString FAqueductGraphExecutor::GetLastError()
{
	return FfiToString(aqueduct_last_error());
}

// Parse a JSON [r, g, b, a] array into a color; missing components stay 0.
static FLinearColor RgbaArrayToColor(const TArray<TSharedPtr<FJsonValue>>& Rgba)
{
	FLinearColor Color(0.f, 0.f, 0.f, 0.f);
	if (Rgba.Num() > 0) Color.R = (float)Rgba[0]->AsNumber();
	if (Rgba.Num() > 1) Color.G = (float)Rgba[1]->AsNumber();
	if (Rgba.Num() > 2) Color.B = (float)Rgba[2]->AsNumber();
	if (Rgba.Num() > 3) Color.A = (float)Rgba[3]->AsNumber();
	return Color;
}
// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FAqueductGraphExecutor::FAqueductGraphExecutor()
{
}

FAqueductGraphExecutor::~FAqueductGraphExecutor()
{
	// Belt-and-braces against the lifetime hazard class: if any render
	// command still references this executor, freeing the FFI handles and
	// RHI refs under it is a use-after-free. Owners capture TSharedPtr into
	// render commands (so last-release may happen ON the render thread,
	// where everything queued has already run); a game-thread destruction
	// drains the render thread first.
	if (IsInGameThread() && !IsEngineExitRequested())
	{
		FlushRenderingCommands();
	}
	Reset();
}

void FAqueductGraphExecutor::UpdatePermutationOwnership()
{
	TSet<int32> Indices;
	for (const TPair<FString, FNodeShaderData>& Pair : NodeShaders)
	{
		if (Pair.Value.PermutationIndex >= 0)
		{
			Indices.Add(Pair.Value.PermutationIndex);
		}
	}
	FAqueductNodeShaderStorage::SetOwnerIndices(this, Indices);
}

void FAqueductGraphExecutor::Reset()
{
	// Release our permutation-index references so unreferenced shaders become
	// recyclable (this executor is going away).
	FAqueductNodeShaderStorage::ReleaseOwner(this);

	NodeTextures.Empty();
	OutputTextures.Empty();
	ResolvedFormats.Empty();
	NodeShaders.Empty();
	ParamValuesFloat.Empty();
	ParamValuesInt.Empty();
	PlaceholderTexture = nullptr;
	PlaceholderSRV = nullptr;
	BlueNoiseTexture = nullptr;
	BlueNoiseSRV = nullptr;
	CompiledGraph.Reset();
	bShadersCompiled = false;
	InvalidateProfileCache();

	if (Graph)
	{
		aqueduct_graph_free(Graph);
		Graph = nullptr;
	}
	if (Registry)
	{
		aqueduct_registry_free(Registry);
		Registry = nullptr;
	}
	if (HLSLCompiler)
	{
		aqueduct_compiler_free(HLSLCompiler);
		HLSLCompiler = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::LoadRegistry(const FString& RegistryToml)
{
	if (Registry)
	{
		aqueduct_registry_free(Registry);
		Registry = nullptr;
	}

	FTCHARToUTF8 Utf8(*RegistryToml);
	Registry = aqueduct_registry_load(Utf8.Get());
	if (!Registry)
	{
		UE_LOG(LogAqueduct, Error, TEXT("Failed to load registry: %s"), *GetLastError());
		return false;
	}

	int32 NodeCount = aqueduct_registry_node_count(Registry);
	UE_LOG(LogAqueduct, Log, TEXT("Registry loaded: %d node definitions"), NodeCount);
	return true;
}

bool FAqueductGraphExecutor::LoadGraph(const FString& GraphToml)
{
	if (!Registry)
	{
		UE_LOG(LogAqueduct, Error, TEXT("Cannot load graph: no registry loaded"));
		return false;
	}

	if (Graph)
	{
		aqueduct_graph_free(Graph);
		Graph = nullptr;
	}

	NodeTextures.Empty();
	OutputTextures.Empty();
	ResolvedFormats.Empty();
	NodeShaders.Empty();
	bShadersCompiled = false;
	InvalidateProfileCache();

	FTCHARToUTF8 Utf8(*GraphToml);
	Graph = aqueduct_graph_parse_toml(Utf8.Get(), Registry);
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("Failed to parse graph: %s"), *GetLastError());
		return false;
	}

	UE_LOG(LogAqueduct, Log, TEXT("Graph loaded: %d nodes, resolution %d"),
		aqueduct_graph_node_count(Graph), aqueduct_graph_resolution(Graph));
	LastRenderedSigs.Empty();
	LastRenderedTimes.Empty();
	RefreshNodeSignatures();
	return LoadExecutionPlan();
}

void FAqueductGraphExecutor::RefreshNodeSignatures()
{
	NodeSigs.Empty();
	TimeCone.Empty();
	if (!Graph || !Registry)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Root =
		FfiJsonObject(aqueduct_graph_node_signatures_json(Graph, Registry));
	if (!Root.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Nodes;
	if (Root->TryGetObjectField(TEXT("nodes"), Nodes))
	{
		for (const auto& Pair : (*Nodes)->Values)
		{
			FString Sig;
			if (Pair.Value->TryGetString(Sig))
			{
				NodeSigs.Add(Pair.Key, Sig);
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Cone;
	if (Root->TryGetArrayField(TEXT("time_cone"), Cone))
	{
		for (const auto& Value : *Cone)
		{
			FString Id;
			if (Value->TryGetString(Id))
			{
				TimeCone.Add(Id);
			}
		}
	}
}

bool FAqueductGraphExecutor::LoadGraphJson(const FString& GraphJson)
{
	if (Graph)
	{
		aqueduct_graph_free(Graph);
		Graph = nullptr;
	}

	NodeTextures.Empty();
	OutputTextures.Empty();
	ResolvedFormats.Empty();
	NodeShaders.Empty();
	bShadersCompiled = false;
	InvalidateProfileCache();

	FTCHARToUTF8 Utf8(*GraphJson);
	Graph = aqueduct_graph_parse_json(Utf8.Get());
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("Failed to parse graph JSON: %s"), *GetLastError());
		return false;
	}

	LastRenderedSigs.Empty();
	LastRenderedTimes.Empty();
	RefreshNodeSignatures();
	return LoadExecutionPlan();
}

bool FAqueductGraphExecutor::LoadExecutionPlan()
{
	PlanOrder.Reset();
	PlanNodes.Reset();
	PlanOutputs.Reset();
	MipRequiredTextureKeys.Reset();
	TimeParamSlots.Reset();
	TimeParamValues.Reset();
	PlanHash = 0;
	if (!Graph || !Registry)
	{
		return false;
	}

	const FString PlanJson = FfiToString(aqueduct_graph_execution_plan_json(Graph, Registry));
	if (PlanJson.IsEmpty())
	{
		UE_LOG(LogAqueduct, Error, TEXT("Execution plan failed: %s"), *GetLastError());
		return false;
	}
	const TSharedPtr<FJsonObject> Plan = ParseJsonObject(PlanJson);
	if (!Plan.IsValid())
	{
		UE_LOG(LogAqueduct, Error, TEXT("Execution plan JSON malformed"));
		return false;
	}
	PlanHash = GetTypeHash(PlanJson);

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Plan->TryGetArrayField(TEXT("nodes"), Nodes))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = Value->AsObject();
			if (!Node.IsValid())
			{
				continue;
			}
			FString NodeId;
			Node->TryGetStringField(TEXT("id"), NodeId);
			if (NodeId.IsEmpty())
			{
				continue;
			}
			FExecutionPlanNode Entry;
			Node->TryGetStringField(TEXT("kind"), Entry.Kind);
			Node->TryGetStringField(TEXT("format"), Entry.Format);
			Node->TryGetStringField(TEXT("compute_format"), Entry.ComputeFormat);
			Node->TryGetStringField(TEXT("definition_hash"), Entry.DefinitionHash);
			double Passes = 1.0;
			Node->TryGetNumberField(TEXT("passes"), Passes);
			Entry.Passes = FMath::Max(1u, (uint32)Passes);
			Node->TryGetBoolField(TEXT("dither_8bit"), Entry.bDither8Bit);
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (Node->TryGetArrayField(TEXT("outputs"), Outputs))
			{
				for (const auto& Output : *Outputs)
				{
					Entry.Outputs.Add(Output->AsString());
				}
			}
			if (Entry.Outputs.IsEmpty())
			{
				Entry.Outputs.Add(TEXT("output"));
			}

			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			if (Node->TryGetObjectField(TEXT("inputs"), Inputs))
			{
				for (const auto& Pair : (*Inputs)->Values)
				{
					Entry.Inputs.Add(Pair.Key, Pair.Value->AsString());
				}
			}
			const TSharedPtr<FJsonObject>* Defaults = nullptr;
			if (Node->TryGetObjectField(TEXT("input_defaults"), Defaults))
			{
				for (const auto& Pair : (*Defaults)->Values)
				{
					Entry.InputDefaults.Add(Pair.Key, RgbaArrayToColor(Pair.Value->AsArray()));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Mips = nullptr;
			if (Node->TryGetArrayField(TEXT("mipmapped_inputs"), Mips))
			{
				for (const auto& Mip : *Mips)
				{
					Entry.MipmappedInputs.Add(Mip->AsString());
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Scratch = nullptr;
			if (Node->TryGetArrayField(TEXT("scratch"), Scratch))
			{
				for (const auto& DeclValue : *Scratch)
				{
					const TSharedPtr<FJsonObject> Decl = DeclValue->AsObject();
					if (!Decl.IsValid())
					{
						continue;
					}
					FString Name, Type;
					Decl->TryGetStringField(TEXT("name"), Name);
					Decl->TryGetStringField(TEXT("ty"), Type);
					FScratchDeclInfo Info;
					Info.Stride = Type.EndsWith(TEXT("4")) ? 16 : 4;
					double Count = 1.0;
					Decl->TryGetNumberField(TEXT("count"), Count);
					Info.Count = (uint32)Count;
					Entry.Scratch.Add(Name, Info);
				}
			}
			PlanOrder.Add(NodeId);
			PlanNodes.Add(NodeId, MoveTemp(Entry));
		}
	}
	for (const auto& Pair : PlanNodes)
	{
		const FExecutionPlanNode& Consumer = Pair.Value;
		for (const FString& Semantic : Consumer.MipmappedInputs)
		{
			if (const FString* SourceKey = Consumer.Inputs.Find(Semantic))
			{
				MipRequiredTextureKeys.Add(*SourceKey);
			}
		}
	}

	const TSharedPtr<FJsonObject>* Outputs = nullptr;
	if (Plan->TryGetObjectField(TEXT("outputs"), Outputs))
	{
		for (const auto& Pair : (*Outputs)->Values)
		{
			PlanOutputs.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	const TSharedPtr<FJsonValue> Slots =
		ParseJsonValue(FfiToString(aqueduct_graph_time_param_slots_json(Graph)));
	if (Slots.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* SlotArray = nullptr;
		if (Slots->TryGetArray(SlotArray))
		{
			for (const TSharedPtr<FJsonValue>& SlotValue : *SlotArray)
			{
				const TSharedPtr<FJsonObject> Slot = SlotValue->AsObject();
				if (!Slot.IsValid())
				{
					continue;
				}
				FString NodeId, Param;
				Slot->TryGetStringField(TEXT("node"), NodeId);
				Slot->TryGetStringField(TEXT("param"), Param);
				TimeParamSlots.Emplace(MoveTemp(NodeId), MoveTemp(Param));
			}
		}
	}
	TimeParamValues.SetNumZeroed(TimeParamSlots.Num());
	// Bounded execution artifacts (Rust computes, C++ obeys). Absent or
	// malformed sections leave the plans invalid and bounded modes off.
	OneShotPlan = FBoundedPlan();
	FramePlan = FBoundedPlan();
	PredictedResidentBytes = 0;
	PoolRemap.Reset();
	{
		const uint64 ResPx = uint64(GetResolution()) * uint64(GetResolution());
		auto FormatBpp = [](const FString& Token) -> uint64 {
			if (Token == TEXT("rgba32float")) return 16;
			if (Token == TEXT("rgba16float")) return 8;
			return 4; // r32float, rgba8unorm
		};
		for (const auto& Pair : PlanNodes)
		{
			const FExecutionPlanNode& P = Pair.Value;
			PredictedResidentBytes += ResPx * FormatBpp(P.ComputeFormat);
			if (P.bDither8Bit) PredictedResidentBytes += ResPx * 4;
			if (P.Passes > 1) PredictedResidentBytes += 2 * ResPx * 16;
		}
		for (const FString& SourceKey : MipRequiredTextureKeys)
		{
			FString ProducerId = SourceKey;
			int32 DotIndex = INDEX_NONE;
			if (ProducerId.FindChar(TEXT('.'), DotIndex))
			{
				ProducerId = ProducerId.Left(DotIndex);
			}
			const FExecutionPlanNode* Producer = PlanNodes.Find(ProducerId);
			const uint64 Bpp = FormatBpp(Producer ? Producer->ComputeFormat : TEXT("rgba16float"));
			uint32 Side = GetResolution();
			while (Side > 1)
			{
				Side = FMath::Max(Side / 2u, 1u);
				PredictedResidentBytes += uint64(Side) * uint64(Side) * Bpp;
			}
		}

		auto ParseBounded = [](const TSharedPtr<FJsonObject>& Obj, FBoundedPlan& Out) {
			if (!Obj.IsValid()) return;
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Obj->TryGetArrayField(TEXT("order"), Arr))
			{
				for (const auto& V : *Arr) Out.Order.Add(V->AsString());
			}
			if (Obj->TryGetArrayField(TEXT("pool"), Arr))
			{
				for (const auto& V : *Arr)
				{
					const TSharedPtr<FJsonObject> Slot = V->AsObject();
					if (!Slot.IsValid()) continue;
					Out.Pool.Emplace(Slot->GetStringField(TEXT("format")),
						Slot->GetBoolField(TEXT("mipped")));
				}
			}
			const TSharedPtr<FJsonObject>* AssignObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("assign"), AssignObj))
			{
				for (const auto& A : (*AssignObj)->Values)
				{
					Out.Assign.Add(A.Key, int32(A.Value->AsNumber()));
				}
			}
			if (Obj->TryGetArrayField(TEXT("free_after"), Arr))
			{
				for (const auto& StepVal : *Arr)
				{
					TArray<FString>& Keys = Out.FreeAfter.AddDefaulted_GetRef();
					const TArray<TSharedPtr<FJsonValue>>* StepArr = nullptr;
					if (StepVal->TryGetArray(StepArr))
					{
						for (const auto& K : *StepArr) Keys.Add(K->AsString());
					}
				}
			}
		};
		const TSharedPtr<FJsonObject>* Bounded = nullptr;
		if (Plan->TryGetObjectField(TEXT("bounded"), Bounded))
		{
			const TSharedPtr<FJsonObject>* OneShot = nullptr;
			if ((*Bounded)->TryGetObjectField(TEXT("one_shot"), OneShot))
			{
				ParseBounded(*OneShot, OneShotPlan);
			}
			const TSharedPtr<FJsonObject>* Repeated = nullptr;
			if ((*Bounded)->TryGetObjectField(TEXT("repeated"), Repeated))
			{
				const TSharedPtr<FJsonObject>* FP = nullptr;
				if ((*Repeated)->TryGetObjectField(TEXT("frame_plan"), FP))
				{
					ParseBounded(*FP, FramePlan);
				}
			}
		}
	}

	return true;
}

uint32 FAqueductGraphExecutor::GetResolution() const
{
	if (!Graph) return 0;
	return aqueduct_graph_resolution(Graph);
}

int32 FAqueductGraphExecutor::GetNormalFormat() const
{
	if (!Graph) return 0;
	return aqueduct_graph_normal_format(Graph);
}

// ---------------------------------------------------------------------------
// Expression evaluation (game thread only)
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::EvaluateTimeExpressions()
{
	if (!Graph || TimeParamSlots.IsEmpty())
	{
		return;
	}
	// Slot layout is fixed at plan load; Rust fills one float per slot.
	// No JSON and no allocation on this per-frame path.
	aqueduct_graph_eval_time_params(
		Graph, CurrentTime, TimeParamValues.GetData(), TimeParamValues.Num());
	for (int32 Slot = 0; Slot < TimeParamSlots.Num(); Slot++)
	{
		SetNodeParam(TimeParamSlots[Slot].Key, TimeParamSlots[Slot].Value, TimeParamValues[Slot]);
	}
}

bool FAqueductGraphExecutor::HasTimeExpressions() const
{
	return TimeParamSlots.Num() > 0;
}

// ---------------------------------------------------------------------------
// Parameter updates (no recompilation needed)
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::SetNodeParam(const FString& NodeId, const FString& ParamName, float Value)
{
	if (CompiledGraph.IsValid() && !CompiledGraph->SetParam(NodeId, ParamName, Value))
	{
		return false;
	}
	FString Key = NodeId + TEXT(".") + ParamName;
	ParamValuesFloat.Add(Key, Value);
	return true;
}

bool FAqueductGraphExecutor::SetNodeParam(const FString& NodeId, const FString& ParamName, int32 Value)
{
	if (CompiledGraph.IsValid() && !CompiledGraph->SetParam(NodeId, ParamName, (float)Value))
	{
		return false;
	}
	FString Key = NodeId + TEXT(".") + ParamName;
	ParamValuesInt.Add(Key, Value);
	return true;
}

void FAqueductGraphExecutor::ReleaseGraphResources()
{
	check(IsInGameThread());

	NodeTextures.Empty();
	OutputTextures.Empty();
	SharedScratch.Empty();
	ResolvedFormats.Empty();
	CompiledGraph.Reset();
	InvalidateProfileCache();
	PeakVramBytes = 0;

	UE_LOG(LogAqueduct, Log, TEXT("Released graph working GPU memory (shaders + parsed state kept)"));
}

namespace
{

/** Parse `{"name": "hash", ...}` JSON into a TMap. */
bool ParseHashMapJson(const FFfiString& Json, TMap<FString, FString>& OutMap)
{
	if (!Json.IsValid()) return false;
	const TSharedPtr<FJsonObject> Obj = ParseJsonObject(Json.ToString());
	if (!Obj.IsValid()) return false;
	for (const auto& Pair : Obj->Values)
	{
		FString Value;
		if (Pair.Value->TryGetString(Value))
		{
			OutMap.Add(Pair.Key, Value);
		}
	}
	return true;
}

} // namespace

FString FAqueductGraphExecutor::GetGraphContentHash() const
{
	return Graph ? FfiToString(aqueduct_graph_content_hash(Graph)) : FString();
}

TMap<FString, FString> FAqueductGraphExecutor::GetOutputContentHashes() const
{
	TMap<FString, FString> Result;
	if (Graph)
	{
		const FFfiString Json(aqueduct_graph_output_content_hashes_json(Graph));
		ParseHashMapJson(Json, Result);
	}
	return Result;
}

FString FAqueductGraphExecutor::GetOutputBindingsJson() const
{
	return Graph ? FfiToString(aqueduct_graph_outputs_json(Graph)) : FString();
}

bool FAqueductGraphExecutor::ComputeContentHashes(const FString& RegistryToml,
	const FString& GraphToml, FString& OutGraphHash, TMap<FString, FString>& OutOutputHashes)
{
	FTCHARToUTF8 RegUtf8(*RegistryToml);
	LoadedRegistry* Reg = aqueduct_registry_load(RegUtf8.Get());
	if (!Reg) return false;

	FTCHARToUTF8 GraphUtf8(*GraphToml);
	LoadedGraph* ParsedGraph = aqueduct_graph_parse_toml(GraphUtf8.Get(), Reg);
	if (!ParsedGraph)
	{
		aqueduct_registry_free(Reg);
		return false;
	}

	bool bOk = false;
	OutGraphHash = FfiToString(aqueduct_graph_content_hash(ParsedGraph));
	if (!OutGraphHash.IsEmpty())
	{
		const FFfiString Json(aqueduct_graph_output_content_hashes_json(ParsedGraph));
		bOk = ParseHashMapJson(Json, OutOutputHashes);
	}

	aqueduct_graph_free(ParsedGraph);
	aqueduct_registry_free(Reg);
	return bOk;
}

bool FAqueductGraphExecutor::ReloadGraphKeepShaders(const FString& GraphToml)
{
	if (!Registry) return false;

	// Shaders and textures are preserved — only topology changes need recompilation.
	if (Graph)
	{
		aqueduct_graph_free(Graph);
		Graph = nullptr;
	}

	FTCHARToUTF8 Utf8(*GraphToml);
	Graph = aqueduct_graph_parse_toml(Utf8.Get(), Registry);
	if (!Graph)
	{
		UE_LOG(LogAqueduct, Error, TEXT("ReloadGraphKeepShaders: failed to parse graph: %s"), *GetLastError());
		return false;
	}

	// Force compiled graph rebuild on next Execute (reuses existing shaders)
	CompiledGraph.Reset();
	InvalidateProfileCache();
	return LoadExecutionPlan();
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

bool FAqueductGraphExecutor::ShouldEngageBounded() const
{
	// Mirrors the native engine: 3.5 GiB enforced tracked budget
	// (aqueduct/crates/engine/src/vram.rs DEFAULT_TRACKED_BUDGET_BYTES).
	const uint64 TrackedBudgetBytes = 3584ull * 1024 * 1024;
	return PredictedResidentBytes > TrackedBudgetBytes;
}

void FAqueductGraphExecutor::CreateMippedPoolSlots(
	const FBoundedPlan& Plan, const TCHAR* Prefix, uint32 Resolution)
{
	for (int32 Slot = 0; Slot < Plan.Pool.Num(); Slot++)
	{
		if (!Plan.Pool[Slot].Value)
		{
			continue; // plain slots materialize lazily
		}
		const FString Key = FString::Printf(TEXT("%s%d"), Prefix, Slot);
		if (NodeTextures.Contains(Key))
		{
			continue;
		}
		// UE computes rgba8unorm nodes in rgba16float (narrow formats
		// widen; only delivered outputs quantize), so mipped slots for that
		// class must be created wide or the base-level mip copy mismatches.
		const FString FormatToken = Plan.Pool[Slot].Key == TEXT("rgba8unorm")
			? TEXT("rgba16float")
			: Plan.Pool[Slot].Key;
		const EPixelFormat PixelFormat = AqueductFormatToPixelFormat(FormatToken);
		const uint32 NumMips = FMath::FloorLog2(Resolution) + 1;
		const FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(TEXT("AqueductPool"))
			.SetExtent(Resolution, Resolution)
			.SetFormat(PixelFormat)
			.SetNumMips(NumMips)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::UAVCompute);
		FAqueductTexture Entry;
		Entry.Texture = RHICreateTexture(Desc);
		Entry.Resolution = Resolution;
		Entry.FormatStr = FormatToken;
		NodeTextures.Add(Key, MoveTemp(Entry));
	}
}

FTextureRHIRef FAqueductGraphExecutor::GetOrCreateNodeTexture(
	const FString& NodeId, const FString& Format, uint32 Resolution, bool bMipmapped)
{
	const FString& Key = ResolvePoolKey(NodeId);
	FAqueductTexture* Existing = NodeTextures.Find(Key);
	const uint32 RequiredMips = bMipmapped ? FMath::FloorLog2(Resolution) + 1 : 1;
	if (Existing && Existing->Resolution == Resolution && Existing->FormatStr == Format &&
		Existing->Texture.IsValid() && Existing->Texture->GetNumMips() >= RequiredMips)
	{
		return Existing->Texture;
	}

	EPixelFormat PixelFormat = AqueductFormatToPixelFormat(Format);

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("AqueductNode"))
		.SetExtent(Resolution, Resolution)
		.SetFormat(PixelFormat)
		.SetNumMips(RequiredMips)
		.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
		.SetInitialState(ERHIAccess::UAVCompute);

	FTextureRHIRef Texture = RHICreateTexture(Desc);

	FAqueductTexture Entry;
	Entry.Texture = Texture;
	Entry.FormatStr = Format;
	Entry.Resolution = Resolution;
	NodeTextures.Add(Key, MoveTemp(Entry));

	return Texture;
}

FString FAqueductGraphExecutor::RuntimeTextureKey(const FString& PlanKey) const
{
	int32 DotIndex = INDEX_NONE;
	if (!PlanKey.FindChar(TEXT('.'), DotIndex))
	{
		return PlanKey;
	}
	const FString ProducerId = PlanKey.Left(DotIndex);
	const FExecutionPlanNode* Producer = PlanNodes.Find(ProducerId);
	return Producer && Producer->Outputs.Num() == 1 ? ProducerId : PlanKey;
}

FTextureRHIRef FAqueductGraphExecutor::GetNodeTexture(const FString& NodeId) const
{
	const FAqueductTexture* Entry = NodeTextures.Find(ResolvePoolKey(NodeId));
	return Entry ? Entry->Texture : nullptr;
}

} // namespace UE::Aqueduct
