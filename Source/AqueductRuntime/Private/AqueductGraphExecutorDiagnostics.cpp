// AqueductGraphExecutor diagnostics: product GPU profiling and VRAM
// bookkeeping over the executor's working set.

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

bool FAqueductGraphExecutor::IsGraphProfilingSupported() const
{
	return GSupportsTimestampRenderQueries;
}

void FAqueductGraphExecutor::SetGraphProfilingEnabled(bool bEnabled)
{
	bGraphProfilingEnabled = bEnabled && IsGraphProfilingSupported();
	if (!bGraphProfilingEnabled)
	{
		bGraphPerNodeProfilingEnabled = false;
	}
	if (bEnabled && !IsGraphProfilingSupported())
	{
		BuildUnsupportedProfileSnapshot();
	}
}

void FAqueductGraphExecutor::SetGraphPerNodeProfilingEnabled(bool bEnabled)
{
	bGraphPerNodeProfilingEnabled = bEnabled && bGraphProfilingEnabled && IsGraphProfilingSupported();
}

void FAqueductGraphExecutor::InvalidateProfileCache()
{
	++ProfileGraphRevision;
	PendingProfileRecords.Empty();
	ProfileNodeTimings.Empty();
	bHasLatestProfileSnapshot = false;
	LatestProfileSnapshot = FAqueductProfileSnapshot();
	LatestProfileSnapshot.GraphRevision = ProfileGraphRevision;
	LatestProfileSnapshot.RenderRevision = ProfileRenderRevision;
}

void FAqueductGraphExecutor::BuildUnsupportedProfileSnapshot()
{
	LatestProfileSnapshot = FAqueductProfileSnapshot();
	LatestProfileSnapshot.GraphRevision = ProfileGraphRevision;
	LatestProfileSnapshot.RenderRevision = ProfileRenderRevision;
	LatestProfileSnapshot.bTimestampSupported = false;
	bHasLatestProfileSnapshot = true;
}

void FAqueductGraphExecutor::BuildProfileSnapshotFromCache(double ActualGpuMs, bool bHasActualGpuMs, const TSet<FString>& MeasuredNodeIds)
{
	LatestProfileSnapshot = FAqueductProfileSnapshot();
	LatestProfileSnapshot.GraphRevision = ProfileGraphRevision;
	LatestProfileSnapshot.RenderRevision = ProfileRenderRevision;
	LatestProfileSnapshot.bTimestampSupported = true;
	LatestProfileSnapshot.Render.bHasActualGpuMs = bHasActualGpuMs;
	LatestProfileSnapshot.Render.ActualGpuMs = ActualGpuMs;

	bool bAllKnown = CompiledGraph.IsValid() && CompiledGraph->Nodes.Num() > 0;
	double ModeledFullGraphMs = 0.0;
	if (CompiledGraph.IsValid())
	{
		for (const FCompiledNode& Node : CompiledGraph->Nodes)
		{
			FAqueductNodeProfile NodeProfile;
			NodeProfile.NodeId = Node.NodeId;
			NodeProfile.Kind = Node.NodeKind;
			if (const FCachedNodeTiming* Cached = ProfileNodeTimings.Find(Node.NodeId))
			{
				NodeProfile.Kind = Cached->Kind;
				NodeProfile.GpuMs = Cached->GpuMs;
				NodeProfile.bHasGpuMs = true;
				NodeProfile.Dispatches = Cached->Dispatches;
				NodeProfile.Passes = Cached->Passes;
				NodeProfile.State = MeasuredNodeIds.Contains(Node.NodeId)
					? EAqueductNodeProfileState::MeasuredThisRender
					: EAqueductNodeProfileState::Cached;
				ModeledFullGraphMs += Cached->GpuMs;
			}
			else
			{
				NodeProfile.State = EAqueductNodeProfileState::Unknown;
				bAllKnown = false;
			}
			LatestProfileSnapshot.Nodes.Add(NodeProfile);
		}
	}

	LatestProfileSnapshot.Render.ModeledFullGraphGpuMs = ModeledFullGraphMs;
	LatestProfileSnapshot.Render.bHasModeledFullGraphGpuMs = bAllKnown;
	bHasLatestProfileSnapshot = true;
}

void FAqueductGraphExecutor::ForceAllDirty()
{
	if (!CompiledGraph.IsValid())
	{
		return;
	}
	CompiledGraph->PendingDirtyNodes.Reset();
	for (int32 NodeIndex = 0; NodeIndex < CompiledGraph->Nodes.Num(); ++NodeIndex)
	{
		CompiledGraph->PendingDirtyNodes.Add(static_cast<uint32>(NodeIndex));
	}
	CompiledGraph->bHasPendingDirty = CompiledGraph->PendingDirtyNodes.Num() > 0;
}

bool FAqueductGraphExecutor::ResolvePendingProfileQueries(bool bWait)
{
	if (!CompiledGraph.IsValid() || PendingProfileRecords.IsEmpty())
	{
		return false;
	}

	TMap<FString, double> MeasuredMsByNode;
	TMap<FString, FString> KindByNode;
	TMap<FString, uint32> DispatchesByNode;
	TMap<FString, uint32> PassesByNode;
	double ActualGpuMs = 0.0;

	for (const FPendingProfileRecord& Record : PendingProfileRecords)
	{
		uint64 StartUs = 0;
		uint64 EndUs = 0;
		if (!RHIGetRenderQueryResult(Record.StartQuery.GetReference(), StartUs, bWait) ||
			!RHIGetRenderQueryResult(Record.EndQuery.GetReference(), EndUs, bWait))
		{
			return false;
		}
		const double GpuMs = static_cast<double>(EndUs > StartUs ? EndUs - StartUs : 0) / 1000.0;
		ActualGpuMs += GpuMs;
		if (!CompiledGraph->Nodes.IsValidIndex(Record.NodeIndex))
		{
			continue;
		}
		const FCompiledNode& Node = CompiledGraph->Nodes[Record.NodeIndex];
		MeasuredMsByNode.FindOrAdd(Node.NodeId) += GpuMs;
		KindByNode.FindOrAdd(Node.NodeId) = Node.NodeKind;
		DispatchesByNode.FindOrAdd(Node.NodeId) += FMath::Max<uint32>(Record.Dispatches, 1);
		uint32& Passes = PassesByNode.FindOrAdd(Node.NodeId);
		Passes = FMath::Max<uint32>(Passes, FMath::Max<uint32>(Record.TotalPasses, 1));
	}

	for (const TPair<FString, double>& Pair : MeasuredMsByNode)
	{
		FCachedNodeTiming& Cached = ProfileNodeTimings.FindOrAdd(Pair.Key);
		Cached.Kind = KindByNode.FindRef(Pair.Key);
		Cached.GpuMs = Pair.Value;
		Cached.Dispatches = DispatchesByNode.FindRef(Pair.Key);
		Cached.Passes = FMath::Max<uint32>(PassesByNode.FindRef(Pair.Key), 1);
	}

	TSet<FString> MeasuredNodeIds;
	for (const TPair<FString, double>& Pair : MeasuredMsByNode)
	{
		MeasuredNodeIds.Add(Pair.Key);
	}
	BuildProfileSnapshotFromCache(ActualGpuMs, true, MeasuredNodeIds);
	PendingProfileRecords.Empty();
	return true;
}

// ---------------------------------------------------------------------------
// VRAM bookkeeping
// ---------------------------------------------------------------------------

static uint64 AqueductTextureVramBytes(const FTextureRHIRef& Texture)
{
	if (!Texture.IsValid())
	{
		return 0;
	}
	return RHICalcTexturePlatformSize(Texture->GetDesc()).Size;
}

FString AqueductVramNodeOf(const FString& Key)
{
	// Underscore-prefixed keys are engine-internal (shared multipass pair,
	// shared scratch slots) and have no owning node.
	if (Key.StartsWith(TEXT("_")))
	{
		return FString();
	}
	int32 DotIdx;
	return Key.FindChar(TEXT('.'), DotIdx) ? Key.Left(DotIdx) : Key;
}

// Classify a NodeTextures key into the category contract shared with the
// Rust engine (aqueduct/crates/engine/src/vram.rs).
const TCHAR* ClassifyVramTextureKey(const FString& Key, FString& OutNodeId)
{
	if (Key.StartsWith(TEXT("_dither_")))
	{
		// On UE the dithered rgba8 texture IS the node's delivered output
		// (the rgba16float compute texture is the intermediate).
		OutNodeId = AqueductVramNodeOf(Key.Mid(8));
		return TEXT("outputs");
	}
	if (Key.Contains(TEXT(".__host_")))
	{
		OutNodeId = AqueductVramNodeOf(Key);
		return TEXT("host-textures");
	}
	if (Key.StartsWith(TEXT("_pool_")))
	{
		OutNodeId.Reset();
		return TEXT("pool");
	}
	if (Key.EndsWith(TEXT(".__proxy")))
	{
		// Pinned display proxies exist only on interactive engine backends;
		// classified here to keep the category contract mirrored.
		OutNodeId = AqueductVramNodeOf(Key);
		return TEXT("proxy");
	}
	if (Key.EndsWith(TEXT(".ping")) || Key.EndsWith(TEXT(".pong")))
	{
		OutNodeId = AqueductVramNodeOf(Key);
		return TEXT("multipass-scratch");
	}
	OutNodeId = AqueductVramNodeOf(Key);
	return TEXT("outputs");
}

uint64 FAqueductGraphExecutor::ComputeCurrentVramBytes() const
{
	uint64 Total = 0;
	for (const auto& Pair : NodeTextures)
	{
		Total += AqueductTextureVramBytes(Pair.Value.Texture);
	}
	if (PlaceholderTexture.IsValid())
	{
		Total += AqueductTextureVramBytes(PlaceholderTexture);
	}
	if (BlueNoiseTexture.IsValid())
	{
		Total += AqueductTextureVramBytes(BlueNoiseTexture);
	}
	for (const auto& Pair : SharedScratch)
	{
		Total += Pair.Value.SizeBytes;
	}
	if (ScratchPlaceholderBuffer.IsValid())
	{
		Total += ScratchPlaceholderBuffer->GetSize();
	}
	return Total;
}

void FAqueductGraphExecutor::UpdateVramPeak()
{
	PeakVramBytes = FMath::Max(PeakVramBytes, ComputeCurrentVramBytes());
}

FAqueductVramSnapshot FAqueductGraphExecutor::GetVramSnapshot() const
{
	FAqueductVramSnapshot Snapshot;
	Snapshot.Resolution = GetResolution();

	TMap<FString, FAqueductVramCategory> Categories;
	TMap<FString, FAqueductVramNodeUsage> NodeUsage;
	const auto Add = [&Categories, &NodeUsage](
		const TCHAR* Category, const FString& NodeId, uint64 Bytes) {
		FAqueductVramCategory& Cat = Categories.FindOrAdd(Category);
		Cat.Name = Category;
		Cat.Bytes += Bytes;
		Cat.Count++;
		if (!NodeId.IsEmpty())
		{
			FAqueductVramNodeUsage& Node = NodeUsage.FindOrAdd(NodeId);
			Node.NodeId = NodeId;
			Node.Bytes += Bytes;
			Node.Count++;
		}
	};

	// OutputTextures alias NodeTextures entries, so NodeTextures alone is the
	// complete, double-count-free texture set.
	for (const auto& Pair : NodeTextures)
	{
		const uint64 Bytes = AqueductTextureVramBytes(Pair.Value.Texture);
		FString NodeId;
		const TCHAR* Category = ClassifyVramTextureKey(Pair.Key, NodeId);
		Add(Category, NodeId, Bytes);
		Snapshot.TextureBytes += Bytes;
		Snapshot.TextureCount++;
	}
	if (PlaceholderTexture.IsValid())
	{
		const uint64 Bytes = AqueductTextureVramBytes(PlaceholderTexture);
		Add(TEXT("defaults"), FString(), Bytes);
		Snapshot.TextureBytes += Bytes;
		Snapshot.TextureCount++;
	}
	if (BlueNoiseTexture.IsValid())
	{
		const uint64 Bytes = AqueductTextureVramBytes(BlueNoiseTexture);
		Add(TEXT("defaults"), FString(), Bytes);
		Snapshot.TextureBytes += Bytes;
		Snapshot.TextureCount++;
	}

	// Shared scratch pool: one entry per (slot, stride), not per node.
	for (const auto& Pair : SharedScratch)
	{
		const uint64 Bytes = Pair.Value.SizeBytes;
		Add(TEXT("buffers"), FString(), Bytes);
		Snapshot.BufferBytes += Bytes;
		Snapshot.BufferCount++;
	}
	if (ScratchPlaceholderBuffer.IsValid())
	{
		const uint64 Bytes = ScratchPlaceholderBuffer->GetSize();
		Add(TEXT("buffers"), FString(), Bytes);
		Snapshot.BufferBytes += Bytes;
		Snapshot.BufferCount++;
	}

	Snapshot.CurrentBytes = Snapshot.TextureBytes + Snapshot.BufferBytes;
	PeakVramBytes = FMath::Max(PeakVramBytes, Snapshot.CurrentBytes);
	Snapshot.PeakBytes = PeakVramBytes;

	Categories.GenerateValueArray(Snapshot.Categories);
	Snapshot.Categories.Sort([](const FAqueductVramCategory& A, const FAqueductVramCategory& B) {
		return A.Bytes != B.Bytes ? A.Bytes > B.Bytes : A.Name < B.Name;
	});
	NodeUsage.GenerateValueArray(Snapshot.Nodes);
	Snapshot.Nodes.Sort([](const FAqueductVramNodeUsage& A, const FAqueductVramNodeUsage& B) {
		return A.Bytes != B.Bytes ? A.Bytes > B.Bytes : A.NodeId < B.NodeId;
	});

	return Snapshot;
}

FString FAqueductGraphExecutor::VramSnapshotToJson(const FAqueductVramSnapshot& Snapshot)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), Snapshot.Version);
	Root->SetStringField(TEXT("backend"), Snapshot.Backend);
	Root->SetNumberField(TEXT("resolution"), Snapshot.Resolution);
	Root->SetNumberField(TEXT("currentBytes"), static_cast<double>(Snapshot.CurrentBytes));
	Root->SetNumberField(TEXT("peakBytes"), static_cast<double>(Snapshot.PeakBytes));
	Root->SetNumberField(TEXT("textureBytes"), static_cast<double>(Snapshot.TextureBytes));
	Root->SetNumberField(TEXT("bufferBytes"), static_cast<double>(Snapshot.BufferBytes));
	Root->SetNumberField(TEXT("textureCount"), Snapshot.TextureCount);
	Root->SetNumberField(TEXT("bufferCount"), Snapshot.BufferCount);

	TArray<TSharedPtr<FJsonValue>> Categories;
	for (const FAqueductVramCategory& Category : Snapshot.Categories)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Category.Name);
		Obj->SetNumberField(TEXT("bytes"), static_cast<double>(Category.Bytes));
		Obj->SetNumberField(TEXT("count"), Category.Count);
		Categories.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("categories"), Categories);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const FAqueductVramNodeUsage& Node : Snapshot.Nodes)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("nodeId"), Node.NodeId);
		Obj->SetNumberField(TEXT("bytes"), static_cast<double>(Node.Bytes));
		Obj->SetNumberField(TEXT("count"), Node.Count);
		Nodes.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("nodes"), Nodes);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}


} // namespace UE::Aqueduct
