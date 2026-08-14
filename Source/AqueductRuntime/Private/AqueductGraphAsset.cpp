#include "AqueductGraphAsset.h"
#include "AqueductGraphExecutor.h"
#include "AqueductPaths.h"
#include "Misc/FileHelper.h"
#include "UObject/ObjectSaveContext.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductGraphAsset, Log, All);

void UAqueductGraph::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

#if WITH_EDITOR
	// Cook-time shader metadata capture. A shipped game cannot compile
	// shaders, so during cook we compile this graph's node shaders (which
	// registers their permutations — the cooker then bakes them into the
	// shipped global shader library via FAqueductDynamicCS dedup) and record
	// the permutation indices + binding metadata the runtime executor needs.
	//
	// Index stability: FAqueductNodeShaderStorage::RegisterOrReuse dedups by
	// HLSL content hash, so the indices recorded here match the ones the
	// module's cook-startup registration produced earlier in this process.
	if (!SaveContext.IsCooking() || GraphToml.IsEmpty() || !bAnimated)
	{
		return;
	}

	FString Registry = RegistryToml;
	if (Registry.IsEmpty())
	{
		Registry = AqueductNodeAssets::BuiltinRegistryToml();
		if (Registry.IsEmpty())
		{
			UE_LOG(LogAqueductGraphAsset, Error,
				TEXT("%s: embedded registry unavailable during cook"), *GetPathName());
			return;
		}
		RegistryToml = Registry;
	}

	UE::Aqueduct::FAqueductGraphExecutor Executor;
	if (!Executor.LoadRegistry(Registry) ||
		!Executor.LoadGraph(GraphToml) ||
		!Executor.CompileAllShaders())
	{
		UE_LOG(LogAqueductGraphAsset, Error,
			TEXT("%s: shader compile failed during cook"), *GetPathName());
		return;
	}

	CookedMetadataJson = Executor.ExportPrecompiledMetadataJson();
	UE_LOG(LogAqueductGraphAsset, Log,
		TEXT("Cooked shader metadata for animated graph %s (%d bytes)"),
		*GraphName, CookedMetadataJson.Len());
#endif // WITH_EDITOR
}
