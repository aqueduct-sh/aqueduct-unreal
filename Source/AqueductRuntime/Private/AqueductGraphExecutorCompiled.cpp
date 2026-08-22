// AqueductGraphExecutor compiled graph: record the dispatch list once,
// then replay it every frame with per-node dirty tracking.

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

// ---------------------------------------------------------------------------
// Compiled graph: build
// ---------------------------------------------------------------------------

// Record replay-time regeneration for one canonical producer pyramid.
static void RecordMipChain(
	FCompiledGraph& CG,
	const FString& SourceKey,
	FTextureRHIRef OutputTexture,
	uint32 Resolution)
{
	if (!OutputTexture.IsValid())
	{
		return;
	}
	FString SourceNodeId = SourceKey;
	int32 DotIdx;
	if (SourceNodeId.FindChar(TEXT('.'), DotIdx))
	{
		SourceNodeId = SourceNodeId.Left(DotIdx);
	}
	const uint32* SourceIndex = CG.NodeIdToIndex.Find(SourceNodeId);
	if (!SourceIndex)
	{
		return;
	}
	for (const FCompiledMipChain& Existing : CG.MipChains)
	{
		if (Existing.SourceNodeIndex == *SourceIndex && Existing.OutputTexture == OutputTexture)
		{
			return;
		}
	}
	FCompiledMipChain Chain;
	Chain.OutputTexture = OutputTexture;
	Chain.Resolution = Resolution;
	Chain.SourceNodeIndex = *SourceIndex;
	CG.MipChains.Add(MoveTemp(Chain));
}

void FAqueductGraphExecutor::BuildCompiledGraph(FRHICommandListImmediate& RHICmdList)
{
	// Bounded playback: over
	// budget, the animated cone's intermediates share the frame pool. The
	// remap is installed before the build so every cached SRV/UAV binds a
	// pool slot; the dirty system already re-dispatches only the animated
	// cone, so static results persist untouched outside the pool. The pool
	// lives until ReleaseGraphResources.
	if (ShouldEngageBounded() && FramePlan.IsValid())
	{
		CreateMippedPoolSlots(FramePlan, TEXT("_pool_"), GetResolution());
		PoolRemap.Reset();
		for (const auto& A : FramePlan.Assign)
		{
			// Identity-stable keys only, same rule as one-shot: multipass
			// finalization replaces map entries and cannot share a slot.
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

	if (!Graph || !bShadersCompiled) return;

	uint32 Resolution = aqueduct_graph_resolution(Graph);
	uint32 GroupCount = FMath::DivideAndRoundUp(Resolution, (uint32)WORKGROUP_SIZE);

	const TArray<FString>& ExecutionOrder = PlanOrder;

	auto CG = MakeUnique<FCompiledGraph>();
	CG->Resolution = Resolution;

	// Which params are time expressions (evaluation itself happens in Rust
	// each frame; replay only needs the dirty-tracking set).
	TSet<FString> ExpressionKeys; // "nodeId.paramName"
	for (const TPair<FString, FString>& Slot : TimeParamSlots)
	{
		ExpressionKeys.Add(Slot.Key + TEXT(".") + Slot.Value);
	}

	// Phase 1: Build nodes with resolved params
	for (const FString& NodeId : ExecutionOrder)
	{
		const FNodeShaderData* SD = NodeShaders.Find(NodeId);
		if (!SD) continue;

		uint32 NodeIdx = CG->Nodes.Num();
		CG->NodeIdToIndex.Add(NodeId, NodeIdx);

		FCompiledNode& Node = CG->Nodes.AddDefaulted_GetRef();
		Node.NodeId = NodeId;
		Node.NodeKind = PlanNodes[NodeId].Kind;
		FTCHARToUTF8 NodeIdUtf8(*NodeId);

		const TSharedPtr<FJsonObject> ResolvedParams = FfiJsonObject(
			aqueduct_graph_get_node_resolved_params_json(Graph, Registry, NodeIdUtf8.Get()));

		bool bNodeHasExpressions = false;
		for (const auto& ParamPair : SD->Params)
		{
			FCompiledParam& CP = Node.Params.AddDefaulted_GetRef();
			CP.Name = ParamPair.Key;
			CP.bIsInt = (ParamPair.Value == TEXT("int"));

			if (ParamPair.Key == TEXT("pass_index"))
			{
				CP.Source = EParamSource::PassIndex;
			}
			else if (ParamPair.Key == TEXT("total_passes"))
			{
				CP.Source = EParamSource::TotalPasses;
			}
			else
			{
				// All params (including expressions) are Named.
				// Expression values are applied per-frame via EvaluateExpressions().
				CP.Source = EParamSource::Named;

				// Check ParamValuesFloat first (may have expression-evaluated values)
				FString OverrideKey = NodeId + TEXT(".") + ParamPair.Key;
				if (const float* Override = ParamValuesFloat.Find(OverrideKey))
				{
					CP.Value = *Override;
				}
				else if (ResolvedParams.IsValid())
				{
					double Num = 0;
					if (ResolvedParams->TryGetNumberField(ParamPair.Key, Num))
					{
						CP.Value = (float)Num;
					}
				}

				if (ExpressionKeys.Contains(OverrideKey))
				{
					bNodeHasExpressions = true;
				}
			}
		}

		// Track time-dependent nodes (those with expressions)
		if (bNodeHasExpressions)
		{
			CG->TimeDepNodes.Add(NodeIdx);
		}

		// Buffer size: 4 bytes per param, 16-byte aligned, minimum 16
		if (Node.Params.Num() > 0)
		{
			uint32 RawSize = Node.Params.Num() * 4;
			Node.BufferSize = FMath::Max(16u, (RawSize + 15u) & ~15u);
		}
	}

	// Phase 2: Build dispatches with pre-resolved SRVs/UAVs
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	FShaderResourceViewRHIRef Placeholder = GetPlaceholderSRV(RHICmdList);
	uint32 NextPipelineId = 0;
	TMap<int32, uint32> PipelineIdMap;

	for (const FString& NodeId : ExecutionOrder)
	{
		const FNodeShaderData* SD = NodeShaders.Find(NodeId);
		if (!SD) continue;

		uint32 NodeIdx = *CG->NodeIdToIndex.Find(NodeId);
		uint32 DispatchStart = CG->Dispatches.Num();

		// Pipeline ID dedup
		uint32 PipelineId;
		if (uint32* Existing = PipelineIdMap.Find(SD->PermutationIndex))
		{
			PipelineId = *Existing;
		}
		else
		{
			PipelineId = NextPipelineId++;
			PipelineIdMap.Add(SD->PermutationIndex, PipelineId);
		}

		auto BuildDispatch = [&](uint32 PassIdx, uint32 TotalPasses) -> FCompiledDispatch&
		{
			FCompiledDispatch& D = CG->Dispatches.AddDefaulted_GetRef();
			D.PermutationIndex = SD->PermutationIndex;
			D.PipelineId = PipelineId;
			D.WorkgroupsX = GroupCount;
			D.WorkgroupsY = GroupCount;
			D.NodeIndex = NodeIdx;
			D.PassIndex = PassIdx;
			D.TotalPasses = TotalPasses;
			D.Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			// Resolve scratch decls from the effective NodeDef (registry for
			// built-ins, materialized from the graph's [custom_nodes] table
			// otherwise) — the SAME source DispatchDynamicNode uses, so the
			// compiled-replay path sizes custom-node scratch identically to the
			// full-execute path. Slots bind the SHARED pool via count-sized
			// views; by build time the preceding full execute has already
			// grown every slot to its graph-wide maximum, so these views stay
			// valid for the compiled graph's lifetime.
			{
				const TArray<FScratchDeclInfo> Decls = ResolveScratchDecls(NodeId, *SD);
				for (int32 i = 0; i < Decls.Num(); i++)
				{
					D.ScratchUAVs[i] = GetSharedScratchUAV(RHICmdList, i, Decls[i].Stride, Decls[i].Count);
				}
				FUnorderedAccessViewRHIRef ScratchPlaceholder = GetScratchPlaceholderUAV(RHICmdList);
				for (int32 i = 0; i < MAX_SCRATCH_SLOTS; i++)
				{
					if (!D.ScratchUAVs[i]) D.ScratchUAVs[i] = ScratchPlaceholder;
				}
				// Schedule a scratch zero-fill before this node's first dispatch
				// each replay. Required for correctness on two counts: shared
				// slots hold the previous node's garbage, and some shaders
				// (e.g. flood_fill_to_random_color's pass 0) only write to
				// scratch slots for extant islands and rely on every other
				// slot being zero. Matches the wgpu backend's behavior in
				// replay_compiled_graph — both backends must zero-fill so
				// that cross-render determinism holds.
				if (PassIdx == 0)
				{
					for (int32 i = 0; i < Decls.Num(); i++)
					{
						D.ScratchUAVsToZero.Add(D.ScratchUAVs[i]);
					}
				}
			}

			// Multi-pass outputs write to rgba32 staging: shared ping/pong for the
			// primary semantic and one stable staging texture per secondary.
			bool bMultiOutput = SD->OutputSemantics.Num() > 1;
			for (int32 i = 0; i < SD->OutputSemantics.Num() && i < MAX_NODE_OUTPUTS; i++)
			{
				FString TexKey;
				if (TotalPasses > 1)
				{
					if (i == 0)
					{
						// The primary output rotates through shared ping/pong.
						if (PassIdx == 0)
							TexKey = GSharedPingKey;
						else
							TexKey = (PassIdx % 2 == 1) ? GSharedPongKey : GSharedPingKey;
					}
					else
					{
						TexKey = NodeId + TEXT(".__stage_") + SD->OutputSemantics[i];
					}
				}
				else
				{
					TexKey = bMultiOutput
						? (NodeId + TEXT(".") + SD->OutputSemantics[i])
						: NodeId;
				}

				FAqueductTexture* Entry = NodeTextures.Find(ResolvePoolKey(TexKey));
				if (Entry && Entry->Texture.IsValid())
				{
					D.OutputUAVs[i] = RHICmdList.CreateUnorderedAccessView(
						Entry->Texture, FRHIViewDesc::CreateTextureUAV()
							.SetDimension(ETextureDimension::Texture2D)
							.SetMipLevel(0));
					D.OutputTextures.Add(Entry->Texture);
				}
			}

			// Build input SRVs from the plan's resolved wiring.
			const FExecutionPlanNode& NodePlan = PlanNodes[NodeId];
			for (int32 i = 0; i < SD->InputSemantics.Num() && i < MAX_NODE_INPUTS; i++)
			{
				const FString& Semantic = SD->InputSemantics[i];

				if (Semantic == TEXT("ping"))
				{
					// Multi-pass ping: bind the correct read buffer for this pass
					if (PassIdx == 0)
					{
						// First pass: no feedback. Transparent black, matching
						// the Rust engine's `_default_ping` texture.
						D.InputSRVs[i] = GetDefaultColorSRV(RHICmdList, FLinearColor(0.f, 0.f, 0.f, 0.f), Resolution);
					}
					else
					{
						FString PingKey = GSharedPingKey;
						FString PongKey = GSharedPongKey;
						FString ReadKey = (PassIdx % 2 == 1) ? PingKey : PongKey;
						FAqueductTexture* ReadEntry = NodeTextures.Find(ResolvePoolKey(ReadKey));
						if (ReadEntry && ReadEntry->Texture.IsValid())
						{
							D.InputSRVs[i] = RHICmdList.CreateShaderResourceView(
								ReadEntry->Texture, FRHIViewDesc::CreateTextureSRV()
									.SetDimension(ETextureDimension::Texture2D));
						}
						else
						{
							D.InputSRVs[i] = Placeholder;
						}
					}
					continue;
				}

				if (const FString* SourceKeyPtr = NodePlan.Inputs.Find(Semantic))
				{
					const FString& SourceKey = *SourceKeyPtr;
					FTextureRHIRef InputTex = GetNodeTexture(SourceKey);
					if (InputTex.IsValid())
					{
						D.InputSRVs[i] = RHICmdList.CreateShaderResourceView(
							InputTex, FRHIViewDesc::CreateTextureSRV()
								.SetDimension(ETextureDimension::Texture2D));
						if (SD->MipmappedInputs.Contains(Semantic))
						{
							RecordMipChain(*CG, SourceKey, InputTex, Resolution);
						}
						continue;
					}
				}
				// Unconnected/missing input: 1x1 default color (node def's
				// input_defaults entry, else transparent black), matching
				// the Rust engine's unconnected-input default.
				const FLinearColor* Override = SD->InputDefaults.Find(Semantic);
				D.InputSRVs[i] = GetDefaultColorSRV(RHICmdList, Override ? *Override : FLinearColor(0.f, 0.f, 0.f, 0.f), Resolution);
			}

			// SRV slots the shader never reads still need a bound
			// descriptor; fill them with the white placeholder.
			for (int32 i = SD->InputSemantics.Num(); i < MAX_NODE_INPUTS; i++)
			{
				D.InputSRVs[i] = Placeholder;
			}

			return D;
		};

		if (SD->Passes > 1)
		{
			for (uint32 Pass = 0; Pass < SD->Passes; Pass++)
			{
				BuildDispatch(Pass, SD->Passes);
			}
		}
		else
		{
			BuildDispatch(0, 1);
		}

		CG->NodeDispatchRanges.Add(TPair<uint32, uint32>(DispatchStart, CG->Dispatches.Num()));

		// Build one format conversion entry per multi-pass semantic.
		if (SD->Passes > 1)
		{
			FString PingKey = GSharedPingKey;
			FString PongKey = GSharedPongKey;
			const FString PrimaryStageKey = (SD->Passes % 2 == 0) ? PongKey : PingKey;
			const bool bMultiOutput = SD->OutputSemantics.Num() > 1;
			for (int32 OutputIndex = 0; OutputIndex < SD->OutputSemantics.Num(); ++OutputIndex)
			{
				const FString& Semantic = SD->OutputSemantics[OutputIndex];
				const FString SourceKey = OutputIndex == 0
					? PrimaryStageKey
					: NodeId + TEXT(".__stage_") + Semantic;
				const FString DestinationKey = bMultiOutput ? NodeId + TEXT(".") + Semantic : NodeId;
				FAqueductTexture* SrcEntry = NodeTextures.Find(ResolvePoolKey(SourceKey));
				FAqueductTexture* DstEntry = NodeTextures.Find(ResolvePoolKey(DestinationKey));
				if (SrcEntry && DstEntry && SrcEntry->Texture.IsValid() && DstEntry->Texture.IsValid())
				{
					FCompiledFormatConvert FC;
					FC.SrcTexture = SrcEntry->Texture;
					FC.DstTexture = DstEntry->Texture;
					FC.Resolution = Resolution;
					FC.NodeIndex = NodeIdx;
					CG->FormatConverts.Add(MoveTemp(FC));
				}
			}
		}
	}

	// Phase 3: Build downstream dependency graph
	uint32 NumNodes = CG->Nodes.Num();
	TArray<TArray<uint32>> DownstreamTmp;
	DownstreamTmp.SetNum(NumNodes);

	for (const FString& NodeId : ExecutionOrder)
	{
		if (!NodeShaders.Contains(NodeId)) continue;
		uint32 NodeIdx = *CG->NodeIdToIndex.Find(NodeId);

		for (const auto& Input : PlanNodes[NodeId].Inputs)
		{
			FString UpstreamId = Input.Value;
			int32 DotIdx;
			if (UpstreamId.FindChar(TEXT('.'), DotIdx)) UpstreamId = UpstreamId.Left(DotIdx);

			if (const uint32* UpIdx = CG->NodeIdToIndex.Find(UpstreamId))
			{
				DownstreamTmp[*UpIdx].AddUnique(NodeIdx);
			}
		}
	}

	CG->DownstreamOffsets.SetNum(NumNodes + 1);
	for (uint32 i = 0; i < NumNodes; i++)
	{
		CG->DownstreamOffsets[i] = CG->DownstreamFlat.Num();
		CG->DownstreamFlat.Append(DownstreamTmp[i]);
	}
	CG->DownstreamOffsets[NumNodes] = CG->DownstreamFlat.Num();

	CG->IsTimeDep.SetNumZeroed(NumNodes);
	for (uint32 Idx : CG->TimeDepNodes)
	{
		CG->IsTimeDep[Idx] = true;
	}


	// Phase 4: Initialize dirty flags (all dirty for first replay)
	CG->NodeDirty.SetNumZeroed(NumNodes);
	CG->NodeUniformDirty.SetNumZeroed(NumNodes);
	for (uint32 i = 0; i < NumNodes; i++)
	{
		CG->NodeDirty[i] = true;
		CG->NodeUniformDirty[i] = true;
	}

	UE_LOG(LogAqueduct, Log, TEXT("Built compiled graph: %d nodes, %d dispatches, %d mip chains"),
		CG->Nodes.Num(), CG->Dispatches.Num(), CG->MipChains.Num());

	CompiledGraph = MoveTemp(CG);
}

// ---------------------------------------------------------------------------
// Compiled graph: replay
// ---------------------------------------------------------------------------

void FAqueductGraphExecutor::ReplayCompiledGraph(FRHICommandListImmediate& RHICmdList)
{
	if (!ensure(CompiledGraph.IsValid())) return;
	FCompiledGraph& CG = *CompiledGraph;
	uint32 NumNodes = CG.Nodes.Num();

	float Time = CurrentTime;

	// Phase 1: Mark dirty nodes
	for (uint32 i = 0; i < NumNodes; i++)
	{
		CG.NodeDirty[i] = false;
		CG.NodeUniformDirty[i] = false;
	}

	// Time-dependent nodes: always dirty (their expressions change with t)
	for (uint32 Idx : CG.TimeDepNodes)
	{
		CG.NodeDirty[Idx] = true;
		CG.NodeUniformDirty[Idx] = true;
	}

	// Pending param changes (from SetNodeParam between frames)
	if (CG.bHasPendingDirty)
	{
		for (uint32 Idx : CG.PendingDirtyNodes)
		{
			CG.NodeDirty[Idx] = true;
			CG.NodeUniformDirty[Idx] = true;
		}
		CG.PendingDirtyNodes.Empty();
		CG.bHasPendingDirty = false;
	}

	// If no time-dep nodes and no pending changes, redispatch everything
	// (graph is static — still need to dispatch at least once per frame)
	if (CG.TimeDepNodes.Num() == 0 && !CG.bHasPendingDirty)
	{
		bool bAnyDirty = false;
		for (uint32 i = 0; i < NumNodes; i++)
		{
			if (CG.NodeDirty[i]) { bAnyDirty = true; break; }
		}
		if (!bAnyDirty)
		{
			// Fully static graph with no changes — skip dispatch entirely, but still
			// publish an explicit profile snapshot so "Last render" does not stay running.
			if (bGraphProfilingEnabled && IsGraphProfilingSupported())
			{
				PendingProfileRecords.Empty();
				++ProfileRenderRevision;
				const TSet<FString> MeasuredNodeIds;
				BuildProfileSnapshotFromCache(0.0, true, MeasuredNodeIds);
			}
			else if (bGraphProfilingEnabled)
			{
				BuildUnsupportedProfileSnapshot();
			}
			return;
		}
	}

	// Phase 2: Propagate dirty downstream
	for (uint32 i = 0; i < NumNodes; i++)
	{
		if (CG.NodeDirty[i])
		{
			uint32 Start = CG.DownstreamOffsets[i];
			uint32 End = CG.DownstreamOffsets[i + 1];
			for (uint32 j = Start; j < End; j++)
			{
				CG.NodeDirty[CG.DownstreamFlat[j]] = true;
			}
		}
	}

	// Phase 3: Dispatch
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	uint32 LastPipelineId = UINT32_MAX;
	const bool bProfileThisReplay = bGraphProfilingEnabled && IsGraphProfilingSupported();
	const bool bProfilePerNode = bProfileThisReplay && bGraphPerNodeProfilingEnabled;
	if (bGraphProfilingEnabled && !IsGraphProfilingSupported())
	{
		BuildUnsupportedProfileSnapshot();
	}
	if (bProfileThisReplay)
	{
		PendingProfileRecords.Empty();
		++ProfileRenderRevision;
	}
	FRenderQueryRHIRef GraphProfileStartQuery;
	FRenderQueryRHIRef GraphProfileEndQuery;
	if (bProfileThisReplay && !bProfilePerNode)
	{
		GraphProfileStartQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
		GraphProfileEndQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
		RHICmdList.EndRenderQuery(GraphProfileStartQuery);
	}

	// Multi-pass format-converts include the SHARED primary ping/pong pair, so
	// they run immediately after their source node's last pass, before another
	// multipass node overwrites it. Map each node to all semantic conversions;
	// the batch loop after the dispatches catches any not flushed here.
	TArray<TArray<int32>> ConvertIndicesByNode;
	ConvertIndicesByNode.SetNum(CG.Nodes.Num());
	for (int32 FCIdx = 0; FCIdx < CG.FormatConverts.Num(); FCIdx++)
	{
		ConvertIndicesByNode[CG.FormatConverts[FCIdx].NodeIndex].Add(FCIdx);
	}
	TArray<bool> ConvertIssued;
	ConvertIssued.Init(false, CG.FormatConverts.Num());
	const auto IssueFormatConvert = [&](int32 ConvertIdx)
	{
		const FCompiledFormatConvert& FC = CG.FormatConverts[ConvertIdx];
		FRenderQueryRHIRef ConvertStartQuery;
		FRenderQueryRHIRef ConvertEndQuery;
		if (bProfilePerNode)
		{
			ConvertStartQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
			ConvertEndQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
			RHICmdList.EndRenderQuery(ConvertStartQuery);
		}
		DispatchFormatConvert(RHICmdList, FC.SrcTexture, FC.DstTexture, FC.Resolution);
		LastPipelineId = UINT32_MAX; // convert changed pipeline
		if (bProfilePerNode)
		{
			RHICmdList.EndRenderQuery(ConvertEndQuery);
			FPendingProfileRecord& Record = PendingProfileRecords.AddDefaulted_GetRef();
			Record.NodeIndex = FC.NodeIndex;
			Record.Dispatches = 1;
			Record.TotalPasses = 1;
			Record.StartQuery = ConvertStartQuery;
			Record.EndQuery = ConvertEndQuery;
		}
		ConvertIssued[ConvertIdx] = true;
	};
	int32 ConvertFlushNode = -1;
	const auto FlushNodePostWork = [&](int32 NodeIdx)
	{
		if (NodeIdx < 0)
		{
			return;
		}
		for (const int32 ConvertIdx : ConvertIndicesByNode[NodeIdx])
		{
			if (!ConvertIssued[ConvertIdx]) IssueFormatConvert(ConvertIdx);
		}
		for (const FCompiledMipChain& Chain : CG.MipChains)
		{
			if (Chain.SourceNodeIndex == uint32(NodeIdx))
			{
				GenerateMipmapsInPlace(RHICmdList, Chain.OutputTexture, Chain.Resolution);
				LastPipelineId = UINT32_MAX;
			}
		}
	};

	for (FCompiledDispatch& D : CG.Dispatches)
	{
		if (!CG.NodeDirty[D.NodeIndex])
		{
			continue;
		}

		// Crossing into a new node's segment: flush the previous node's
		// shared-pair convert before its data can be overwritten.
		if ((int32)D.NodeIndex != ConvertFlushNode)
		{
			FlushNodePostWork(ConvertFlushNode);
			ConvertFlushNode = (int32)D.NodeIndex;
		}

		// Shipped-game safety: if the cooked global shader map lacks this
		// permutation (stale/mismatched cooked metadata), skip the dispatch with
		// a logged error rather than crashing in GetShader on the render thread.
		// Mirrors the guard in DispatchDynamicNode.
		{
			FAqueductDynamicCS::FPermutationDomain GuardPerm;
			GuardPerm.Set<FAqueductDynamicCS::FNodeIndex>(D.PermutationIndex);
			if (!GlobalShaderMap->HasShader(&FAqueductDynamicCS::GetStaticType(), GuardPerm.ToDimensionValueId()))
			{
				UE_LOG(LogAqueduct, Error,
					TEXT("ReplayCompiledGraph: shader permutation %d missing for node index %d — skipping dispatch"),
					D.PermutationIndex, D.NodeIndex);
				continue;
			}
		}

		FRenderQueryRHIRef ProfileStartQuery;
		FRenderQueryRHIRef ProfileEndQuery;
		if (bProfilePerNode)
		{
			ProfileStartQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
			ProfileEndQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
			RHICmdList.EndRenderQuery(ProfileStartQuery);
		}

		// Zero scratch buffers before the first pass of each multipass
		// scratch-using node. See the population site in BuildCompiledGraph
		// for the full rationale — short version: some shaders rely on
		// zero-initialized scratch beyond what their pass 0 explicitly
		// writes, and without this the compiled-replay path would leak
		// state from the previous render's final scratch contents. Mirrors
		// the wgpu backend's zero_fill_keys loop in replay_compiled_graph.
		for (const FUnorderedAccessViewRHIRef& ScratchUAV : D.ScratchUAVsToZero)
		{
			ZeroFillScratchUAV(RHICmdList, ScratchUAV);
		}

		for (FTextureRHIRef& Tex : D.OutputTextures)
		{
			RHICmdList.Transition(FRHITransitionInfo(Tex, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		}

		if (D.PipelineId != LastPipelineId)
		{
			FAqueductDynamicCS::FPermutationDomain PermDomain;
			PermDomain.Set<FAqueductDynamicCS::FNodeIndex>(D.PermutationIndex);
			TShaderRef<FAqueductDynamicCS> ComputeShader =
				GlobalShaderMap->GetShader<FAqueductDynamicCS>(PermDomain);
			FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
			SetComputePipelineState(RHICmdList, ShaderRHI);
			LastPipelineId = D.PipelineId;
		}

			FAqueductDynamicCS::FParameters PassParameters;
		if (D.OutputUAVs[0]) PassParameters.AQ_Output0 = D.OutputUAVs[0];
		if (D.OutputUAVs[1]) PassParameters.AQ_Output1 = D.OutputUAVs[1];
		if (D.OutputUAVs[2]) PassParameters.AQ_Output2 = D.OutputUAVs[2];
		if (D.OutputUAVs[3]) PassParameters.AQ_Output3 = D.OutputUAVs[3];

		PassParameters.AQ_Input0  = D.InputSRVs[0];
		PassParameters.AQ_Input1  = D.InputSRVs[1];
		PassParameters.AQ_Input2  = D.InputSRVs[2];
		PassParameters.AQ_Input3  = D.InputSRVs[3];
		PassParameters.AQ_Input4  = D.InputSRVs[4];
		PassParameters.AQ_Input5  = D.InputSRVs[5];
		PassParameters.AQ_Input6  = D.InputSRVs[6];
		PassParameters.AQ_Input7  = D.InputSRVs[7];
		PassParameters.AQ_Input8  = D.InputSRVs[8];
		PassParameters.AQ_Input9  = D.InputSRVs[9];
		PassParameters.AQ_Input10 = D.InputSRVs[10];
		PassParameters.AQ_Input11 = D.InputSRVs[11];
		PassParameters.AQ_Input12 = D.InputSRVs[12];
		PassParameters.AQ_Input13 = D.InputSRVs[13];
		PassParameters.AQ_Input14 = D.InputSRVs[14];
		PassParameters.AQ_Input15 = D.InputSRVs[15];

		PassParameters.AQ_Sampler = D.Sampler;

		for (int32 i = 0; i < MAX_SCRATCH_SLOTS; i++)
		{
			BindScratchSlot(PassParameters, i, D.ScratchUAVs[i]);
		}

		// Pack uniform params. Skip re-packing if this node is only dirty because
		// an upstream node changed (its own params/expressions haven't changed).
		const FCompiledNode& Node = CG.Nodes[D.NodeIndex];
		const bool bNeedsUniformUpdate = CG.NodeUniformDirty[D.NodeIndex];

		for (int32 i = 0; i < MAX_PARAM_VECTORS; i++)
		{
			PassParameters.AQ_Params[i] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		}

		for (int32 i = 0; i < Node.Params.Num() && i < MAX_PARAMS; i++)
		{
			const FCompiledParam& P = Node.Params[i];
			float Val = P.Value;
			// Expression values are already applied via EvaluateExpressions() +
			// SetParam() before replay starts. P.Value has the current value.
			//
			// THREADING: for an animated editor graph the game thread can write
			// P.Value (next frame's EvaluateTimeExpressions) while this RT read
			// runs. That is the only GT/RT overlap here, and it is a benign
			// scalar tear: P.Value is a 4-byte aligned float in a FIXED-SIZE
			// array (no realloc, no rehash), so the worst case is a one-frame-
			// stale animated value — never a crash or corruption. The dangerous
			// container-mutation race (ParamValuesFloat TMap rehash vs Find) is
			// NOT reachable: that map is only read on the structural
			// BuildCompiledGraph path, which is always preceded by a flush.
			if (P.Source == EParamSource::PassIndex) Val = (float)D.PassIndex;
			else if (P.Source == EParamSource::TotalPasses) Val = (float)D.TotalPasses;

			int32 ArrayIdx = i / 4;
			int32 CompIdx = i % 4;

			if (P.bIsInt)
			{
				int32 IntVal = (int32)Val;
				float FloatVal;
				FloatVal = (float)IntVal;
				reinterpret_cast<float*>(&PassParameters.AQ_Params[ArrayIdx])[CompIdx] = FloatVal;
			}
			else
			{
				reinterpret_cast<float*>(&PassParameters.AQ_Params[ArrayIdx])[CompIdx] = Val;
			}
		}

		FAqueductDynamicCS::FPermutationDomain PermDomain;
		PermDomain.Set<FAqueductDynamicCS::FNodeIndex>(D.PermutationIndex);
		TShaderRef<FAqueductDynamicCS> ComputeShader =
			GlobalShaderMap->GetShader<FAqueductDynamicCS>(PermDomain);

		// Canonical helper: sets pipeline + binds + dispatches + UNBINDS.
		// Stale UAV bindings from raw batched dispatches were the root cause
		// of two silent corruption bugs (utility dispatches mis-binding
		// after a different shader's leftovers).
		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, PassParameters,
			FIntVector(D.WorkgroupsX, D.WorkgroupsY, 1));

		for (FTextureRHIRef& Tex : D.OutputTextures)
		{
			RHICmdList.Transition(FRHITransitionInfo(Tex, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		}

		if (bProfilePerNode)
		{
			RHICmdList.EndRenderQuery(ProfileEndQuery);
			FPendingProfileRecord& Record = PendingProfileRecords.AddDefaulted_GetRef();
			Record.NodeIndex = D.NodeIndex;
			Record.Dispatches = 1;
			Record.TotalPasses = D.TotalPasses;
			Record.StartQuery = ProfileStartQuery;
			Record.EndQuery = ProfileEndQuery;
		}
	}

	// Flush the final node's shared-pair convert, then catch any stray
	// converts for dirty nodes (e.g. nodes whose dispatches were all skipped).
	FlushNodePostWork(ConvertFlushNode);
	for (int32 FCIdx = 0; FCIdx < CG.FormatConverts.Num(); FCIdx++)
	{
		if (ConvertIssued[FCIdx] || !CG.NodeDirty[CG.FormatConverts[FCIdx].NodeIndex])
		{
			continue;
		}
		IssueFormatConvert(FCIdx);
	}

	// Re-run dithered rgba8 conversion for dirty outputs.
	// Uses the same DispatchDitherConversion as ExecuteFull.
	uint32 Resolution = CG.Resolution;
	for (const auto& FmtPair : ResolvedFormats)
	{
		if (FmtPair.Value != TEXT("rgba8unorm")) continue;
		FString ProducerId = FmtPair.Key;
		int32 DotIndex = INDEX_NONE;
		if (ProducerId.FindChar(TEXT('.'), DotIndex)) ProducerId = ProducerId.Left(DotIndex);
		const uint32* NodeIdx = CG.NodeIdToIndex.Find(ProducerId);
		if (!NodeIdx || !CG.NodeDirty[*NodeIdx]) continue;

		FString ConvKey = TEXT("_dither_") + FmtPair.Key;
		if (NodeTextures.Contains(ResolvePoolKey(ConvKey)))
		{
			FRenderQueryRHIRef ProfileStartQuery;
			FRenderQueryRHIRef ProfileEndQuery;
			if (bProfilePerNode)
			{
				ProfileStartQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
				ProfileEndQuery = RHICreateRenderQuery(RQT_AbsoluteTime);
				RHICmdList.EndRenderQuery(ProfileStartQuery);
			}
			DispatchDitherConversion(RHICmdList, FmtPair.Key, ConvKey, Resolution);
			if (bProfilePerNode)
			{
				RHICmdList.EndRenderQuery(ProfileEndQuery);
				FPendingProfileRecord& Record = PendingProfileRecords.AddDefaulted_GetRef();
				Record.NodeIndex = *NodeIdx;
				Record.Dispatches = 1;
				Record.TotalPasses = 1;
				Record.StartQuery = ProfileStartQuery;
				Record.EndQuery = ProfileEndQuery;
			}
		}
	}
	if (bProfileThisReplay && !bProfilePerNode)
	{
		RHICmdList.EndRenderQuery(GraphProfileEndQuery);
		FPendingProfileRecord& Record = PendingProfileRecords.AddDefaulted_GetRef();
		// Invalid node index is the graph-level sentinel. ResolvePendingProfileQueries
		// includes it in actual GPU time but skips per-node cache aggregation.
		Record.NodeIndex = TNumericLimits<uint32>::Max();
		Record.Dispatches = 1;
		Record.TotalPasses = 1;
		Record.StartQuery = GraphProfileStartQuery;
		Record.EndQuery = GraphProfileEndQuery;
	}

}


} // namespace UE::Aqueduct
