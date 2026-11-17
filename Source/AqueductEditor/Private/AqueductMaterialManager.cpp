#include "AqueductMaterialManager.h"
#include "AqueductMaterialManagerShared.h"
#include "AqueductTexture.h"
#include "AqueductAssets.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "AqueductGraphAsset.h"
#include "AqueductAnimated.h"
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
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionCustom.h"
#include "MaterialDomain.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "TextureCompiler.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Containers/Ticker.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "EditorValidatorSubsystem.h"
#include "Editor.h"
#include "UObject/ObjectSaveContext.h"
#include "Engine/Engine.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductMaterial, Log, All);

/** One Aqueduct-owned parent material. Indexed by EAqueductParentKind. Fuzz
 *  and anisotropy are wired only on the parents that shade them: Unreal draws
 *  every Default Lit material with a connected Anisotropy input in an extra
 *  mesh pass.
 *
 *  Unreal's Cloth shading model evaluates fuzz but no anisotropy, and Default
 *  Lit evaluates anisotropy but no fuzz; no legacy shading model evaluates
 *  both at one pixel. A graph that binds fuzz_weight therefore shades as
 *  Cloth, and its anisotropy is not shown in Unreal.
 *
 *  Parent selection is structural: it depends on which outputs a graph binds,
 *  never on their values, so no value change can switch parents. */
struct FAqueductParentDef
{
	const TCHAR* PackagePath;
	const TCHAR* AssetName;
	EMaterialShadingModel ShadingModel;
	bool bTranslucent;
	bool bFuzz;
	bool bAnisotropy;
};

static const FAqueductParentDef GParentDefs[] = {
	{TEXT("/Game/Aqueduct/M_Aqueduct_Base"), TEXT("M_Aqueduct_Base"), MSM_DefaultLit, false, false, false},
	{TEXT("/Game/Aqueduct/M_Aqueduct_Anisotropic"), TEXT("M_Aqueduct_Anisotropic"), MSM_DefaultLit, false, false, true},
	{TEXT("/Game/Aqueduct/M_Aqueduct_Cloth"), TEXT("M_Aqueduct_Cloth"), MSM_Cloth, false, true, false},
	{TEXT("/Game/Aqueduct/M_Aqueduct_Translucent"), TEXT("M_Aqueduct_Translucent"), MSM_DefaultLit, true, false, false},
	{TEXT("/Game/Aqueduct/M_Aqueduct_TranslucentCloth"), TEXT("M_Aqueduct_TranslucentCloth"), MSM_Cloth, true, true, false},
};

static bool IsAqueductOwnedParentPackage(const FString& PackageName)
{
	for (const FAqueductParentDef& Def : GParentDefs)
	{
		if (PackageName == Def.PackagePath) return true;
	}
	return false;
}

// Non-realtime editor viewports (material/MI editor previews are realtime
// only while the lighting rig rotates) repaint solely on invalidation. Live
// updates change GPU texture contents in place — no UObject edit, so nothing
// invalidates them: realtime level viewports show every update while an open
// material editor preview freezes on its last painted frame. Whenever pushed
// pixels actually changed, invalidate all editor viewports so non-realtime
// ones repaint (the same mechanism async shader compilation uses when
// compiled materials swap in). Hit proxies don't depend on texture content.
static void InvalidateViewportsForChangedPixels()
{
	if (GEditor)
	{
		GEditor->RedrawAllViewports(/*bInvalidateHitProxies=*/false);
	}
}

// ---------------------------------------------------------------------------
// Output name → material parameter mapping
// ---------------------------------------------------------------------------

// DELIVERY names (the asset-level unit; see BuildDeliverySet) → MIC texture
// parameters. The ORM texture feeds three material inputs through its
// channels: R=AO, G=Roughness, B=Metallic — the UE packing convention.
struct FDeliveryParamDef
{
	FString DeliveryName;
	FName   ParamName;
};

static const FDeliveryParamDef GDeliveryParamMap[] = {
	{TEXT("base_color"),  FName("BaseColor")},
	{TEXT("albedo"),      FName("BaseColor")}, // legacy name; superseded when base_color exists
	{TEXT("normal"),      FName("Normal")},
	{TEXT("orm"),         FName("ORM")},
	{TEXT("height"),      FName("Height")},
	{TEXT("emissive"),    FName("Emissive")},
	{TEXT("opacity"),     FName("Opacity")},
	{TEXT("fuzz_color"),  FName("FuzzColor")},
	{TEXT("fuzz_weight"), FName("FuzzWeight")},
	{TEXT("anisotropy"),  FName("Anisotropy")},
};

static const FName* FindParamNameForDelivery(const FString& DeliveryName)
{
	for (const auto& Def : GDeliveryParamMap)
	{
		if (Def.DeliveryName == DeliveryName) return &Def.ParamName;
	}
	return nullptr;
}

static TSharedPtr<FJsonObject> ParseOutputBindings(
	UE::Aqueduct::FAqueductGraphExecutor* Executor, FString& OutError);
static void ApplyOutputBindingsToMIC(
	const TSharedPtr<FJsonObject>& Outputs, int32 NormalFormat, UMaterialInstanceConstant* MIC);

struct FResolvedOutputBinding
{
	bool bExplicit = false;
	bool bTexture = false;
	float Scalar = 0.0f;
	FLinearColor Vector = FLinearColor::Black;
};

static const TSharedPtr<FJsonValue>* FindOutputBinding(
	const TSharedPtr<FJsonObject>& Outputs, std::initializer_list<const TCHAR*> Names)
{
	if (!Outputs) return nullptr;
	for (const TCHAR* Name : Names)
	{
		if (const TSharedPtr<FJsonValue>* Value = Outputs->Values.Find(Name)) return Value;
	}
	return nullptr;
}

static FResolvedOutputBinding ResolveScalarBinding(
	const TSharedPtr<FJsonObject>& Outputs, std::initializer_list<const TCHAR*> Names, float Default)
{
	FResolvedOutputBinding Result;
	Result.Scalar = Default;
	const TSharedPtr<FJsonValue>* Found = FindOutputBinding(Outputs, Names);
	if (!Found || !Found->IsValid()) return Result;
	Result.bExplicit = true;
	if ((*Found)->Type == EJson::String)
	{
		Result.bTexture = !(*Found)->AsString().IsEmpty();
	}
	else if ((*Found)->Type == EJson::Number)
	{
		Result.Scalar = (float)(*Found)->AsNumber();
	}
	else if ((*Found)->Type == EJson::Array && (*Found)->AsArray().Num() > 0)
	{
		Result.Scalar = (float)(*Found)->AsArray()[0]->AsNumber();
	}
	return Result;
}

static FResolvedOutputBinding ResolveVectorBinding(
	const TSharedPtr<FJsonObject>& Outputs, std::initializer_list<const TCHAR*> Names,
	const FLinearColor& Default)
{
	FResolvedOutputBinding Result;
	Result.Vector = Default;
	const TSharedPtr<FJsonValue>* Found = FindOutputBinding(Outputs, Names);
	if (!Found || !Found->IsValid()) return Result;
	Result.bExplicit = true;
	if ((*Found)->Type == EJson::String)
	{
		Result.bTexture = !(*Found)->AsString().IsEmpty();
	}
	else if ((*Found)->Type == EJson::Number)
	{
		const float Value = (float)(*Found)->AsNumber();
		Result.Vector = FLinearColor(Value, Value, Value, 1.0f);
	}
	else if ((*Found)->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Values = (*Found)->AsArray();
		if (Values.Num() >= 3)
		{
			Result.Vector = FLinearColor(
				(float)Values[0]->AsNumber(), (float)Values[1]->AsNumber(),
				(float)Values[2]->AsNumber(), Values.Num() >= 4 ? (float)Values[3]->AsNumber() : 1.0f);
		}
	}
	return Result;
}

// ---------------------------------------------------------------------------
// Delivery hashing
//
// Per-texture dirty granularity works on DELIVERIES. The ORM texture's hash
// combines the closure hashes of its three contributing outputs (missing
// outputs contribute "-" so adding/removing one changes the hash); all other
// deliveries carry their single output's closure hash directly.
// ---------------------------------------------------------------------------

static FString DeliveryHashFor(const FString& DeliveryName, const TMap<FString, FString>& OutputHashes)
{
	auto Combine = [&OutputHashes](const TCHAR* const* Components, int32 Count) -> FString
	{
		FString Combined;
		bool bAny = false;
		for (int32 i = 0; i < Count; ++i)
		{
			if (i > 0) Combined += TEXT("|");
			if (const FString* H = OutputHashes.Find(Components[i]))
			{
				Combined += *H;
				bAny = true;
			}
			else
			{
				Combined += TEXT("-");
			}
		}
		return bAny ? Combined : FString();
	};
	if (DeliveryName == AqueductTextureWriter::OrmDeliveryName())
	{
		return Combine(AqueductTextureWriter::OrmComponentOutputs(), 3);
	}
	if (DeliveryName == AqueductTextureWriter::AnisotropyDeliveryName())
	{
		return Combine(AqueductTextureWriter::AnisotropyComponentOutputs(), 2);
	}
	return OutputHashes.FindRef(DeliveryName);
}

// At-rest check: wrapper asset current (whole-graph hash) and every output
// texture current (its own closure hash). If all true, the assets are
// self-sufficient — no executor, no compile, zero compute.
static bool TryLoadAtRestAssets(
	const FString& GraphDir, const FString& BaseName, const FString& Hash,
	const TMap<FString, FString>& OutputHashes,
	UAqueductGraph*& OutGraphAsset, TMap<FString, TObjectPtr<UTexture2D>>& OutTextures)
{
	FString PkgName;
	if (!MakeAssetPackageName(GraphDir, BaseName + TEXT("_Graph"), PkgName))
	{
		return false;
	}

	UAqueductGraph* GraphAsset = LoadObject<UAqueductGraph>(
		nullptr, *(PkgName + TEXT(".") + BaseName + TEXT("_Graph")));
	if (!GraphAsset || GraphAsset->ContentHash != Hash)
	{
		return false;
	}

	for (const FString& DeliveryName : GraphAsset->OutputNames)
	{
		FString TexName = TEXT("T_") + BaseName + TEXT("_") + DeliveryName;
		FString TexPkg;
		UTexture2D* Tex = nullptr;
		if (MakeAssetPackageName(GraphDir, TexName, TexPkg))
		{
			Tex = LoadObject<UTexture2D>(nullptr, *(TexPkg + TEXT(".") + TexName));
		}
		UAqueductTextureUserData* Meta = AqueductTexture::GetUserData(Tex, /*bCreate=*/false);
		const FString DeliveryHash = DeliveryHashFor(DeliveryName, OutputHashes);
		if (!Meta || DeliveryHash.IsEmpty() || Meta->GraphContentHash != DeliveryHash)
		{
			return false;
		}
		OutTextures.Add(DeliveryName, Tex);
	}

	OutGraphAsset = GraphAsset;
	return true;
}

// ---------------------------------------------------------------------------
// Asset path helpers
// ---------------------------------------------------------------------------

// The recipe lives next to a graph's assets (per-graph subfolder and legacy
// flat layout alike): derive its path from the asset's own package location,
// never from an assumed root.
static FString RecipePathNextToAsset(const UObject* Asset, const FString& GraphName)
{
	const FString PkgDir = FPaths::GetPath(FPackageName::LongPackageNameToFilename(
		Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()));
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(PkgDir, GraphName + TEXT(".aqueduct")));
}

bool MakeAssetPackageName(const FString& Dir, const FString& AssetName, FString& OutPkgName)
{
	FString DiskPath = FPaths::Combine(Dir, AssetName + FPackageName::GetAssetPackageExtension());
	return FPackageName::TryConvertFilenameToLongPackageName(DiskPath, OutPkgName);
}

void SaveAssetPackage(UObject* Asset)
{
	UPackage* Pkg = Asset->GetPackage();
	FString Filename = FPackageName::LongPackageNameToFilename(
		Pkg->GetName(), FPackageName::GetAssetPackageExtension());

	FString WriteError;
	if (!FAqueductMaterialManager::EnsureFileWritable(Filename, WriteError))
	{
		UE_LOG(LogAqueductMaterial, Error, TEXT("Cannot save %s: %s"), *Filename, *WriteError);
		return;
	}

	const bool bExistedOnDisk = FPaths::FileExists(Filename);

	// Bakes run in the background while the user is focused on the web
	// editor — nothing here may raise a window. Two engine features would:
	// 1. The save-progress slow task (bSlowTask=false suppresses it).
	// 2. The Data Validation plugin's validate-on-save, which pops a MODAL
	//    "Validating assets" dialog on the editor tick after any save. Our
	//    saves are procedural re-bakes of derived pixels; suppress validation
	//    for them the way the engine does for its own automated saves.
	//    (User-initiated saves still validate normally.)
	FScopedDisableValidateOnSave DisableValidation;

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.bSlowTask = false;
	UPackage::SavePackage(Pkg, Asset, *Filename, SaveArgs);

	// Direct saves bypass the editor's add-on-save behavior; cover it here.
	if (!bExistedOnDisk && FPaths::FileExists(Filename))
	{
		FAqueductMaterialManager::MarkFileForAdd(Filename);
	}
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

FAqueductMaterialManager& FAqueductMaterialManager::Get()
{
	static FAqueductMaterialManager Instance;
	return Instance;
}

// ---------------------------------------------------------------------------
// Lifecycle: at-rest vs re-bake policy per graph
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::EnsureInitialized()
{
	check(IsInGameThread());
	if (bInitialized) return;
	bInitialized = true;

	GetOrCreateBaseMaterial();

	// Lazy-bake trigger: whenever one of our assets is about to be serialized
	// (Ctrl+S, Save All, the exit prompt, cook saves), bake its graph first if
	// the pixels are stale. This is the ONLY interactive bake trigger.
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddRaw(
		this, &FAqueductMaterialManager::OnObjectPreSave);

	// Recipe-dies-with-the-wrapper (see header): PreDelete records the
	// candidates, Removed confirms and acts.
	PreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddRaw(
		this, &FAqueductMaterialManager::OnAssetsPreDelete);
	AssetRemovedHandle = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get().OnAssetRemoved().AddRaw(
		this, &FAqueductMaterialManager::OnWrapperAssetRemoved);

	FString AqueductDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Aqueduct")));
	// Recursive: graphs live in per-graph subfolders (legacy root-level
	// recipes are found the same way and keep their flat layout).
	TArray<FString> GraphFiles;
	IFileManager::Get().FindFilesRecursive(
		GraphFiles, *AqueductDir, TEXT("*.aqueduct"), /*Files=*/true, /*Directories=*/false);

	for (const FString& FileName : GraphFiles)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(FileName);
		const FString GraphDir = FPaths::GetPath(FullPath);
		const FString BaseName = FPaths::GetBaseFilename(FullPath);

		FString GraphToml;
		if (!FFileHelper::LoadFileToString(GraphToml, *FullPath)) continue;

		// Core-computed canonical hashes: the whole-graph hash gates the
		// wrapper asset; per-output dependency-closure hashes gate each
		// texture individually. Parse-only — no executor, no compile.
		FString Hash;
		TMap<FString, FString> OutputHashes;
		if (CachedRegistryToml.IsEmpty())
		{
			CachedRegistryToml = AqueductNodeAssets::BuiltinRegistryToml();
			if (CachedRegistryToml.IsEmpty())
			{
				UE_LOG(LogAqueductMaterial, Error,
					TEXT("EnsureInitialized: embedded registry unavailable"));
				continue;
			}
		}
		if (!UE::Aqueduct::FAqueductGraphExecutor::ComputeContentHashes(
				CachedRegistryToml, GraphToml, Hash, OutputHashes))
		{
			UE_LOG(LogAqueductMaterial, Warning,
				TEXT("EnsureInitialized: %s does not parse, skipping"), *FullPath);
			continue;
		}

		UAqueductGraph* AtRestGraphAsset = nullptr;
		TMap<FString, TObjectPtr<UTexture2D>> AtRestTextures;
		const bool bAtRest =
			TryLoadAtRestAssets(GraphDir, BaseName, Hash, OutputHashes, AtRestGraphAsset, AtRestTextures);

		if (bAtRest)
		{
			UE_LOG(LogAqueductMaterial, Log, TEXT("At rest (hash match, no executor): %s"), *FullPath);

			// Animated graphs enter the residency ladder: queue background
			// shader preparation (Ready tier) so that when something on screen
			// first samples their textures, going Live costs an allocation and
			// a dispatch — never a compile.
			if (AtRestGraphAsset && AtRestGraphAsset->bAnimated)
			{
				FGraphState& State = GraphStates.FindOrAdd(FullPath);
				State.GraphToml = GraphToml;
				State.CurrentHash = Hash;
				State.OutputContentHashes = MoveTemp(OutputHashes);
				State.GraphAsset = AtRestGraphAsset;
				State.OutputTextures = MoveTemp(AtRestTextures);
				State.Tier = FGraphState::EAnimatedTier::ReadyPending;
				EnsureTicker();
				UE_LOG(LogAqueductMaterial, Log,
					TEXT("Animated graph queued for background readiness: %s"), *FullPath);
			}
			continue;
		}

		UE_LOG(LogAqueductMaterial, Log, TEXT("Stale or unbaked (hash mismatch), reloading: %s"), *FullPath);
		ReloadGraphFromFile(FullPath);
	}

	WatchDirectory(AqueductDir);
}

void FAqueductMaterialManager::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (PreSaveHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
		PreSaveHandle.Reset();
	}

	if (PreDeleteHandle.IsValid())
	{
		FEditorDelegates::OnAssetsPreDelete.Remove(PreDeleteHandle);
		PreDeleteHandle.Reset();
	}
	if (AssetRemovedHandle.IsValid())
	{
		if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
		{
			AssetRegistry->OnAssetRemoved().Remove(AssetRemovedHandle);
		}
		AssetRemovedHandle.Reset();
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		IDirectoryWatcher* Watcher = FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(
			TEXT("DirectoryWatcher")).Get();
		if (Watcher)
		{
			for (const auto& Pair : WatchedDirectories)
			{
				Watcher->UnregisterDirectoryChangedCallback_Handle(Pair.Key, Pair.Value);
			}
		}
	}
	WatchedDirectories.Empty();
	// Hand every session texture back to its engine resource before dropping
	// state, so no asset lingers on a plugin-owned override.
	for (auto& Pair : GraphStates)
	{
		RevertLiveResources(Pair.Value);
	}
	GraphStates.Empty();
}

// ---------------------------------------------------------------------------
// Hash
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::StartAssetDownloads(
	const FString& GraphFilePath, const TArray<FString>& MissingUrls)
{
	for (const FString& Url : MissingUrls)
	{
		if (InFlightAssetDownloads.Contains(Url))
		{
			continue;
		}
		FString Hash;
		FString Extension;
		if (!AqueductAssets::ParseAssetUrl(Url, Hash, Extension))
		{
			// The executor already whitelists; this guards manager callers.
			UE_LOG(LogAqueductMaterial, Warning,
				TEXT("Asset download skipped (not a whitelisted URL): %s"), *Url);
			continue;
		}
		InFlightAssetDownloads.Add(Url);
		UE_LOG(LogAqueductMaterial, Log, TEXT("Downloading asset %s"), *Url);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
			FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		// Capture by value and re-resolve the singleton: delegates fire on
		// the game thread (CompleteOnGameThread default), but never assume
		// `this` outlives an async request.
		Request->OnProcessRequestComplete().BindLambda(
			[Url, Hash, Extension, GraphFilePath](
				FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				FAqueductMaterialManager& Manager = FAqueductMaterialManager::Get();
				Manager.InFlightAssetDownloads.Remove(Url);

				if (!bConnected || !Response.IsValid() ||
					Response->GetResponseCode() != 200)
				{
					UE_LOG(LogAqueductMaterial, Error,
						TEXT("Asset download failed (%d): %s"),
						Response.IsValid() ? Response->GetResponseCode() : -1, *Url);
					return;
				}

				FString StoreError;
				if (!AqueductAssets::StoreVerifiedAsset(
						Hash, Extension, Response->GetContent(), StoreError))
				{
					UE_LOG(LogAqueductMaterial, Error,
						TEXT("AqueductAssets write failed: %s"), *StoreError);
					return;
				}

				// New AqueductAssets files are project content: mark for add
				// so they travel with the graph through source control.
				FAqueductMaterialManager::MarkFileForAdd(
					AqueductAssets::AssetFilePath(Hash, Extension));

				UE_LOG(LogAqueductMaterial, Log,
					TEXT("Asset %s.%s stored; reloading %s"), *Hash, *Extension, *GraphFilePath);
				Manager.ReloadGraphFromFile(GraphFilePath);
			});
		Request->ProcessRequest();
	}
}

FString FAqueductMaterialManager::ResolveGraphPath(const FString& GraphName)
{
	check(IsInGameThread());

	// Session state first: covers every loaded graph with zero disk access.
	const FString Suffix = TEXT("/") + GraphName + TEXT(".aqueduct");
	for (const auto& Pair : GraphStates)
	{
		if (Pair.Key.EndsWith(Suffix)) return Pair.Key;
	}

	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Aqueduct")));

	// An existing recipe anywhere under the root keeps its location.
	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(
		Found, *Root, *(GraphName + TEXT(".aqueduct")), /*Files=*/true, /*Directories=*/false);
	if (Found.Num() > 0)
	{
		return FPaths::ConvertRelativePathToFull(Found[0]);
	}

	// New graph: per-graph subfolder holds the recipe and all derived assets.
	return FPaths::Combine(Root, GraphName, GraphName + TEXT(".aqueduct"));
}

FString FAqueductMaterialManager::ResolveGraphPathBySourceDocumentId(
	const FString& SourceDocumentId)
{
	check(IsInGameThread());
	if (SourceDocumentId.IsEmpty()) return FString();

	for (const auto& Pair : GraphStates)
	{
		if (Pair.Value.SourceDocumentId == SourceDocumentId)
		{
			return Pair.Key;
		}
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();
	AssetRegistry.WaitForCompletion();

	TArray<FAssetData> GraphAssets;
	AssetRegistry.GetAssetsByClass(
		UAqueductGraph::StaticClass()->GetClassPathName(), GraphAssets);
	for (const FAssetData& AssetData : GraphAssets)
	{
		const UAqueductGraph* GraphAsset =
			Cast<UAqueductGraph>(AssetData.GetAsset());
		if (!GraphAsset || GraphAsset->SourceDocumentId != SourceDocumentId)
		{
			continue;
		}

		FString GraphName = GraphAsset->GraphName;
		if (GraphName.IsEmpty())
		{
			GraphName = GraphAsset->GetName();
			GraphName.RemoveFromEnd(TEXT("_Graph"));
		}
		return RecipePathNextToAsset(GraphAsset, GraphName);
	}

	return FString();
}

bool FAqueductMaterialManager::AcquireSourceOwnership(
	const FString& GraphFilePath, const FString& SourceDocumentId, bool bForce)
{
	check(IsInGameThread());

	// Local-file documents carry no id: pure file semantics, never blocked.
	if (SourceDocumentId.IsEmpty()) return true;

	const FString FullPath = FPaths::ConvertRelativePathToFull(GraphFilePath);
	FGraphState* State = GraphStates.Find(FullPath);

	// The stored stamp: session state when live, else the at-rest wrapper.
	FString StoredId;
	if (State && !State->SourceDocumentId.IsEmpty())
	{
		StoredId = State->SourceDocumentId;
	}
	else if (FPaths::FileExists(FullPath))
	{
		const FString BaseName = FPaths::GetBaseFilename(FullPath);
		FString PkgName;
		if (MakeAssetPackageName(FPaths::GetPath(FullPath), BaseName + TEXT("_Graph"), PkgName))
		{
			if (const UAqueductGraph* GraphAsset = LoadObject<UAqueductGraph>(
					nullptr, *(PkgName + TEXT(".") + BaseName + TEXT("_Graph"))))
			{
				StoredId = GraphAsset->SourceDocumentId;
			}
		}
	}

	if (!StoredId.IsEmpty() && StoredId != SourceDocumentId && !bForce)
	{
		return false; // a DIFFERENT document's graph — caller asks first
	}

	// Adopt: new graph, unstamped graph, same document, or forced replace.
	FGraphState& Adopted = GraphStates.FindOrAdd(FullPath);
	Adopted.SourceDocumentId = SourceDocumentId;
	if (Adopted.GraphAsset && Adopted.GraphAsset->SourceDocumentId != SourceDocumentId)
	{
		Adopted.GraphAsset->SourceDocumentId = SourceDocumentId;
		Adopted.GraphAsset->MarkPackageDirty();
	}
	return true;
}

void FAqueductMaterialManager::MarkFileForAdd(const FString& FilePath)
{
	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (!SCModule.IsEnabled()) return;

	TSharedRef<FMarkForAdd> AddOp = ISourceControlOperation::Create<FMarkForAdd>();
	SCModule.GetProvider().Execute(
		AddOp,
		TArray<FString>{FPaths::ConvertRelativePathToFull(FilePath)},
		EConcurrency::Asynchronous);
}

bool FAqueductMaterialManager::EnsureFileWritable(const FString& FilePath, FString& OutError)
{
	if (!FPaths::FileExists(FilePath)) return true; // new file
	if (!IFileManager::Get().IsReadOnly(*FilePath)) return true;

	// Read-only on disk: try a source-control checkout (the standard editor
	// mechanism; Perforce workspaces keep files read-only until p4 edit).
	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (SCModule.IsEnabled())
	{
		ISourceControlProvider& Provider = SCModule.GetProvider();
		TSharedRef<FCheckOut> CheckOutOp = ISourceControlOperation::Create<FCheckOut>();
		Provider.Execute(CheckOutOp, TArray<FString>{FilePath}, EConcurrency::Synchronous);
	}

	if (!IFileManager::Get().IsReadOnly(*FilePath)) return true;

	OutError = FString::Printf(
		TEXT("%s is read-only — check it out in source control first"), *FilePath);
	return false;
}

// ---------------------------------------------------------------------------
// Reload-from-file: the single ingestion path
// ---------------------------------------------------------------------------

bool FAqueductMaterialManager::ReloadGraphFromFile(const FString& GraphFilePath)
{
	check(IsInGameThread());

	const FString FullPath = FPaths::ConvertRelativePathToFull(GraphFilePath);

	FString GraphToml;
	if (!FFileHelper::LoadFileToString(GraphToml, *FullPath))
	{
		UE_LOG(LogAqueductMaterial, Error, TEXT("ReloadGraphFromFile: cannot read %s"), *FullPath);
		return false;
	}

	if (CachedRegistryToml.IsEmpty())
	{
		CachedRegistryToml = AqueductNodeAssets::BuiltinRegistryToml();
		if (CachedRegistryToml.IsEmpty())
		{
			UE_LOG(LogAqueductMaterial, Error,
				TEXT("ReloadGraphFromFile: embedded registry unavailable"));
			return false;
		}
	}

	const double TParse0 = FPlatformTime::Seconds();

	// Structure snapshot BEFORE the reload: if the output set and resolution
	// survive the edit (the param-scrub case), the GPU-side output textures
	// are still valid destinations and nothing below needs to wait.
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor = FindExecutor(FullPath);
	const bool bHadExecutor = Executor.IsValid();
	const uint32 PrevResolution = Executor.IsValid() ? Executor->GetResolution() : 0;

	// NOTE: GetOrCreateMIC/GetOrCreateExecutor mutate GraphStates — never hold
	// an FGraphState reference across calls to them.
	if (!Executor)
	{
		Executor = GetOrCreateExecutor(FullPath, CachedRegistryToml, GraphToml);
		if (!Executor)
		{
			UE_LOG(LogAqueductMaterial, Error, TEXT("ReloadGraphFromFile: compile failed for %s"), *FullPath);
			return false;
		}
	}
	else
	{
		// Incremental path: preserves the shader cache across reloads.
		if (!Executor->LoadGraphIncremental(GraphToml) ||
			!Executor->CompileShadersIncremental())
		{
			UE_LOG(LogAqueductMaterial, Error,
				TEXT("ReloadGraphFromFile: incremental reload failed for %s"), *FullPath);
			return false;
		}
	}

	// Resolve target capability before creating or changing any material or
	// texture asset. Graph-driven physical luminance remains valid in the core,
	// but this normalized Unreal delivery profile cannot preserve it yet.
	FString BindingError;
	const TSharedPtr<FJsonObject> OutputBindings = ParseOutputBindings(Executor.Get(), BindingError);
	if (!OutputBindings)
	{
		UE_LOG(LogAqueductMaterial, Error, TEXT("ReloadGraphFromFile: %s"), *BindingError);
		return false;
	}
	UMaterialInstanceConstant* MIC = GetOrCreateMIC(FullPath);
	if (!MIC)
	{
		UE_LOG(LogAqueductMaterial, Error, TEXT("ReloadGraphFromFile: cannot create material instance for %s"), *FullPath);
		return false;
	}
	// Only switch Aqueduct-owned parents. A user-authored parent is authoritative.
	const FString ParentPackage = MIC->Parent ? MIC->Parent->GetOutermost()->GetName() : FString();
	if (IsAqueductOwnedParentPackage(ParentPackage))
	{
		const FResolvedOutputBinding Opacity = ResolveScalarBinding(OutputBindings, {TEXT("opacity")}, 1.0f);
		const FResolvedOutputBinding FuzzWeight = ResolveScalarBinding(OutputBindings, {TEXT("fuzz_weight")}, 0.0f);
		const FResolvedOutputBinding Aniso = ResolveScalarBinding(OutputBindings, {TEXT("anisotropy")}, 0.0f);
		// Structural selection: a bound output selects its parent whatever its
		// value, so an arbitrarily small value change never switches parents.
		// fuzz_color and tangent alone change nothing, matching the graph
		// diagnostics.
		const bool bOpacity = Opacity.bExplicit;
		const EAqueductParentKind DesiredKind = bOpacity
			? (FuzzWeight.bExplicit ? EAqueductParentKind::TranslucentCloth : EAqueductParentKind::Translucent)
			: FuzzWeight.bExplicit
			? EAqueductParentKind::Cloth
			: Aniso.bExplicit
			? EAqueductParentKind::Anisotropic
			: EAqueductParentKind::Base;
		if (Aniso.bExplicit && DesiredKind != EAqueductParentKind::Anisotropic)
		{
			// Measured in UE 5.7: neither Cloth shading nor translucent surface
			// shading turns its highlight with the tangent.
			UE_LOG(LogAqueductMaterial, Log,
				TEXT("Aqueduct graph %s binds anisotropy together with %s. Unreal shades anisotropy only on opaque Default Lit materials, so this material renders isotropic in Unreal."),
				*FullPath, FuzzWeight.bExplicit ? TEXT("fuzz_weight") : TEXT("opacity"));
		}
		UMaterial* DesiredParent = GetOrCreateBaseMaterial(DesiredKind);
		if (ensure(DesiredParent) && MIC->Parent != DesiredParent)
		{
			MIC->SetParentEditorOnly(DesiredParent);
			// Rebuild the instance's render state against the new parent, as
			// instance creation does. Without it the instance can keep
			// rendering through the previous parent's shaders.
			MIC->PostEditChange();
		}
	}
	ApplyOutputBindingsToMIC(OutputBindings, Executor->GetNormalFormat(), MIC);
	if (FGraphState* State = GraphStates.Find(FullPath))
	{
		State->CachedAnisoConstants = AqueductTextureWriter::ResolveAnisotropyConstantsFromBindingsJson(
			Executor->GetOutputBindingsJson());
	}

	Executor->EvaluateTimeExpressions();
	const double TParse1 = FPlatformTime::Seconds();

	// Did this edit change structure (resolution or the output set)? Computed
	// CPU-side from the parsed graph — no GPU involvement.
	bool bStructureChanged = !bHadExecutor || Executor->GetResolution() != PrevResolution;
	const TMap<FString, FString> NewOutputHashes = Executor->GetOutputContentHashes();
	if (!bStructureChanged)
	{
		const TMap<FString, FTextureRHIRef>& GpuOutputs = Executor->GetOutputTextures();
		TSet<FString> TextureOutputNames;
		for (const auto& Binding : OutputBindings->Values)
		{
			if (Binding.Value && Binding.Value->Type == EJson::String
				&& !Binding.Value->AsString().IsEmpty())
			{
				TextureOutputNames.Add(FString(*Binding.Key));
			}
		}
		if (GpuOutputs.Num() != TextureOutputNames.Num())
		{
			bStructureChanged = true;
		}
		else
		{
			for (const FString& Name : TextureOutputNames)
			{
				if (!GpuOutputs.Contains(Name)) { bStructureChanged = true; break; }
			}
		}
	}

	// Correctness contract: a graph render either uses
	// the exact bytes its sources name or it does not produce output. If any
	// bitmap source cannot resolve yet (asset still downloading), do NOT
	// execute: keep whatever was last delivered, start the downloads, and
	// reload when they land.
	{
		FString BlockReason;
		if (!Executor->CheckBitmapSourcesResolvable(BlockReason))
		{
			UE_LOG(LogAqueductMaterial, Warning,
				TEXT("Graph %s not executed: %s"), *FullPath, *BlockReason);
			StartAssetDownloads(FullPath, Executor->GetMissingAssetUrls());
			EnsureTicker();
			return true;
		}
	}

	// SHARED capture: the executor cannot be destroyed (graph close, state
	// eviction, executor replacement) while this command is in flight.
	ENQUEUE_RENDER_COMMAND(AqueductReload)([Exec = Executor](FRHICommandListImmediate& RHICmdList)
	{
		Exec->Execute(RHICmdList);
	});

	// THE scrub-latency rule: the game thread blocks on rendering ONLY when
	// the edit changed structure (first load / resolution change / output-set
	// change), because only then must new GPU output textures exist before
	// the asset-binding step below can size them. A pure param edit enqueues
	// the execute and the copies — render-thread order guarantees the copies
	// read post-execute pixels — and never waits. This, plus the live link's
	// per-frame coalescing, is what makes scrubbing in Unreal feel like
	// scrubbing in the web editor.
	if (bStructureChanged)
	{
		FlushRenderingCommands();
	}
	const double TExec1 = FPlatformTime::Seconds();

	EnsureOutputTextureAssets(FullPath, Executor.Get(), MIC);
	CopyOutputsToTextures(FullPath, Executor.Get());
	InvalidateViewportsForChangedPixels();
	const double TCopy1 = FPlatformTime::Seconds();

	UE_LOG(LogAqueductMaterial, Verbose,
		TEXT("Reload stages for %s: parse+compile=%.2fms exec%s=%.2fms ensure+copy=%.2fms structureChanged=%d"),
		*FullPath,
		(TParse1 - TParse0) * 1000.0,
		bStructureChanged ? TEXT("+flush") : TEXT("(async)"),
		(TExec1 - TParse1) * 1000.0,
		(TCopy1 - TExec1) * 1000.0,
		bStructureChanged ? 1 : 0);

	// Animated graphs go straight to the Live tier (editing implies
	// attention); a graph that lost its time-dependence drops out entirely.
	const UE::Aqueduct::FCompiledGraph* CG = Executor->GetCompiledGraph();
	const bool bHasTimeExpressions = CG && CG->TimeDepNodes.Num() > 0;
	{
		FGraphState* State = GraphStates.Find(FullPath);
		const bool bWasLive = State && State->Tier == FGraphState::EAnimatedTier::Live;
		if (bHasTimeExpressions && !bWasLive)
		{
			RegisterAnimatedGraph(FullPath, Executor, FPlatformTime::Seconds());
		}
		else if (!bHasTimeExpressions && State && State->Tier != FGraphState::EAnimatedTier::None)
		{
			UnregisterAnimatedGraph(FullPath);
		}
	}

	// Record the new content, then mark dirty PER TEXTURE: a texture is
	// stale only if its output's dependency-closure hash changed — an edit
	// to the normal-map branch dirties exactly T_<graph>_normal, and a
	// reconnect of unchanged content dirties nothing. The wrapper asset
	// follows the whole-graph hash (it stores the TOML). Any staleness ⇒
	// live maintenance (the executor's output is the GPU truth until the
	// bake inside the user's save).
	if (FGraphState* State = GraphStates.Find(FullPath))
	{
		State->GraphToml = GraphToml;
		State->CurrentHash = Executor->GetGraphContentHash();
		State->OutputContentHashes = NewOutputHashes;
		State->LastVisibleTime = FApp::GetCurrentTime();

		bool bAnyStale = State->OutputTextures.Num() == 0; // never baked
		int32 NumMarked = 0;
		for (const auto& TexPair : State->OutputTextures)
		{
			UAqueductTextureUserData* Meta =
				AqueductTexture::GetUserData(TexPair.Value, /*bCreate=*/false);
			// Textures are keyed by DELIVERY; the ORM texture's hash combines
			// its contributing outputs' closure hashes.
			const FString DeliveryHash =
				DeliveryHashFor(TexPair.Key, State->OutputContentHashes);
			if (!Meta || DeliveryHash.IsEmpty() || Meta->GraphContentHash != DeliveryHash)
			{
				bAnyStale = true;
				if (TexPair.Value)
				{
					const bool bMarked = TexPair.Value->MarkPackageDirty();
					NumMarked += bMarked ? 1 : 0;
					if (!bMarked)
					{
						UE_LOG(LogAqueductMaterial, Warning,
							TEXT("Reload: MarkPackageDirty REFUSED for %s"),
							*TexPair.Value->GetName());
					}
				}
			}
		}
		UE_LOG(LogAqueductMaterial, Verbose,
			TEXT("Reload staleness: %d textures, %d stale-marked, anyStale=%d"),
			State->OutputTextures.Num(), NumMarked, bAnyStale ? 1 : 0);

		if (bAnyStale)
		{
			State->bMaintainLiveResource = true;
			if (!State->GraphAsset) State->GraphAsset = GetOrCreateGraphAsset(FullPath);
			State = GraphStates.Find(FullPath); // GetOrCreateGraphAsset may rehash
		}
		if (State->GraphAsset &&
			(!State->GraphAsset->ContentHash.Equals(State->CurrentHash) ||
			 State->GraphAsset->ContentHash.IsEmpty()))
		{
			State->GraphAsset->MarkPackageDirty();
		}
	}
	EnsureTicker();

	UE_LOG(LogAqueductMaterial, Log, TEXT("Reloaded graph from file: %s%s"),
		*FullPath, bHasTimeExpressions ? TEXT(" (animated)") : TEXT(""));
	return true;
}

bool FAqueductMaterialManager::ApplyLiveParameterEdits(
	const FString& GraphFilePath,
	const FString& GraphToml,
	const TArray<FNumericParamEdit>& Edits)
{
	check(IsInGameThread());

	const FString FullPath = FPaths::ConvertRelativePathToFull(GraphFilePath);
	FGraphState* State = GraphStates.Find(FullPath);
	if (!State || !State->Executor || !State->Executor->GetCompiledGraph()
		|| State->OutputTextures.Num() == 0 || Edits.Num() == 0)
	{
		return false;
	}

	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor = State->Executor;
	for (const FNumericParamEdit& Edit : Edits)
	{
		const bool bApplied = Edit.bInteger
			? Executor->SetNodeParam(Edit.NodeId, Edit.ParamName, (int32)Edit.Value)
			: Executor->SetNodeParam(Edit.NodeId, Edit.ParamName, (float)Edit.Value);
		if (!bApplied)
		{
			return false;
		}
	}

	// The compiled replay owns dependency-cone invalidation. Commands and
	// delivery copies stay ordered on the render thread, while the game
	// thread is free to accept the next slider frame.
	ENQUEUE_RENDER_COMMAND(AqueductLiveParameterEdit)(
		[Exec = Executor](FRHICommandListImmediate& RHICmdList)
		{
			Exec->Execute(RHICmdList);
		});
	CopyOutputsToTextures(FullPath, Executor.Get());
	InvalidateViewportsForChangedPixels();

	State = GraphStates.Find(FullPath);
	if (!State) return false;
	State->GraphToml = GraphToml;
	// The executor intentionally retains its parsed topology during the fast
	// path, so its semantic hashes describe the prior TOML. Empty hashes make
	// the exact file authoritative at the next save, where BakeGraph performs
	// one full reload before baking.
	State->CurrentHash.Empty();
	State->OutputContentHashes.Empty();
	State->LastVisibleTime = FApp::GetCurrentTime();
	State->bMaintainLiveResource = true;
	for (const auto& TexPair : State->OutputTextures)
	{
		if (TexPair.Value) TexPair.Value->MarkPackageDirty();
	}
	if (State->GraphAsset) State->GraphAsset->MarkPackageDirty();
	EnsureTicker();
	return true;
}

// ---------------------------------------------------------------------------
// File watching (external edits only — self-writes are suppressed)
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::SuppressNextWatcherEvent(const FString& GraphFilePath)
{
	SuppressedFiles.Add(FPaths::ConvertRelativePathToFull(GraphFilePath));
}

void FAqueductMaterialManager::WatchDirectory(const FString& Directory)
{
	const FString FullDir = FPaths::ConvertRelativePathToFull(Directory);
	if (WatchedDirectories.Contains(FullDir)) return;
	if (!FPaths::DirectoryExists(FullDir)) return;

	IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(
		TEXT("DirectoryWatcher")).Get();
	if (!Watcher) return;

	FDelegateHandle Handle;
	// Subtree included: per-graph subfolders mean recipes live one level
	// down. OnDirectoryChanged filters to .aqueduct, so asset saves and
	// AqueductAssets writes in the same tree never trigger reloads.
	Watcher->RegisterDirectoryChangedCallback_Handle(
		FullDir,
		IDirectoryWatcher::FDirectoryChanged::CreateRaw(
			this, &FAqueductMaterialManager::OnDirectoryChanged),
		Handle,
		/*Flags=*/0);

	WatchedDirectories.Add(FullDir, Handle);
	UE_LOG(LogAqueductMaterial, Log, TEXT("File watcher active: %s"), *FullDir);
}

void FAqueductMaterialManager::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	TArray<FString> ChangedGraphs;
	for (const FFileChangeData& Change : Changes)
	{
		if (FPaths::GetExtension(Change.Filename) != TEXT("aqueduct")) continue;
		if (Change.Action == FFileChangeData::FCA_Removed) continue;

		const FString FullPath = FPaths::ConvertRelativePathToFull(Change.Filename);
		ChangedGraphs.AddUnique(FullPath);
	}
	if (ChangedGraphs.Num() == 0) return;

	AsyncTask(ENamedThreads::GameThread, [this, ChangedGraphs = MoveTemp(ChangedGraphs)]()
	{
		for (const FString& Path : ChangedGraphs)
		{
			// Self-write echo (live link already reloaded synchronously)?
			if (SuppressedFiles.Remove(Path) > 0) continue;
			if (!FPaths::FileExists(Path)) continue;

			UE_LOG(LogAqueductMaterial, Log,
				TEXT("Graph file changed on disk (external edit), reloading: %s"), *Path);
			ReloadGraphFromFile(Path);
		}
	});
}

// ---------------------------------------------------------------------------
// Base material
// ---------------------------------------------------------------------------



/**
 * Neutral default texture per map kind, assigned to the base material's
 * sampler parameters. Two jobs: (1) the expression's texture must MATCH the
 * sampler type or the material fails to compile (the engine default is an
 * sRGB Color texture — wrong for Normal/Masks/Grayscale samplers); (2) a
 * graph without some map leaves that MIC parameter unset, so the default IS
 * what ships for it — it must be shading-neutral: mid-gray base color, flat
 * normal, ORM = (AO 1, Roughness 0.5, Metallic 0), mid height.
 */
static UTexture2D* GetOrCreateDefaultTexture(
	const TCHAR* Suffix, AqueductTextureWriter::EAqueductMapKind Kind)
{
	const FString TexName = FString::Printf(TEXT("T_Aqueduct_Default_%s"), Suffix);
	const FString PkgName = FString::Printf(TEXT("/Game/Aqueduct/%s"), *TexName);

	UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *(PkgName + TEXT(".") + TexName));
	if (Tex) return Tex;

	UPackage* Pkg = CreatePackage(*PkgName);
	if (!Pkg) return nullptr;
	Pkg->FullyLoad();
	Tex = NewObject<UTexture2D>(Pkg, *TexName, RF_Public | RF_Standalone);

	using AqueductTextureWriter::EAqueductMapKind;
	constexpr int32 Size = 4;
	TArray<uint8> Pixels;
	switch (Kind)
	{
	case EAqueductMapKind::Normal:
	{
		// Flat tangent-space normal (0.5, 0.5, 1.0) in 16-bit.
		const uint16 Texel[4] = { 32768, 32768, 65535, 65535 };
		Pixels.SetNumUninitialized(Size * Size * 8);
		for (int32 i = 0; i < Size * Size; ++i)
		{
			FMemory::Memcpy(Pixels.GetData() + i * 8, Texel, 8);
		}
		break;
	}
	case EAqueductMapKind::Gray:
	{
		const uint8 Fill = FCString::Strcmp(Suffix, TEXT("Opacity")) == 0 ? 255
			: FCString::Strcmp(Suffix, TEXT("FuzzWeight")) == 0 ? 0
			: 128;
		Pixels.Init(Fill, Size * Size);
		break;
	}
	case EAqueductMapKind::Orm:
	{
		// BGRA bytes: B=Metallic 0, G=Roughness 0.5, R=AO 1.
		const uint8 Texel[4] = { 0, 128, 255, 255 };
		Pixels.SetNumUninitialized(Size * Size * 4);
		for (int32 i = 0; i < Size * Size; ++i)
		{
			FMemory::Memcpy(Pixels.GetData() + i * 4, Texel, 4);
		}
		break;
	}
	case EAqueductMapKind::Anisotropy:
	{
		// BGRA bytes: B=0, G=128 (w=0), R=128 (u=0), A=255 -> exactly isotropic;
		// the packed vector is centered on code 128.
		const uint8 Texel[4] = { 0, 128, 128, 255 };
		Pixels.SetNumUninitialized(Size * Size * 4);
		for (int32 i = 0; i < Size * Size; ++i)
		{
			FMemory::Memcpy(Pixels.GetData() + i * 4, Texel, 4);
		}
		break;
	}
	default:  // Color: white for FuzzColor, mid-gray sRGB otherwise
	{
		const uint8 C = FCString::Strcmp(Suffix, TEXT("FuzzColor")) == 0 ? 255 : 128;
		const uint8 Texel[4] = { C, C, C, 255 };
		Pixels.SetNumUninitialized(Size * Size * 4);
		for (int32 i = 0; i < Size * Size; ++i)
		{
			FMemory::Memcpy(Pixels.GetData() + i * 4, Texel, 4);
		}
		break;
	}
	}

	AqueductTexture::InitFromPixels(Tex, Size, Kind, /*bDynamicWritable=*/false, Pixels.GetData());
	FAssetRegistryModule::AssetCreated(Tex);
	Pkg->MarkPackageDirty();
	SaveAssetPackage(Tex);
	return Tex;
}

static float AqueductSrgbToLinear(float Value)
{
	return Value <= 0.04045f
		? Value / 12.92f
		: FMath::Pow((Value + 0.055f) / 1.055f, 2.4f);
}

static TSharedPtr<FJsonObject> ParseOutputBindings(
	UE::Aqueduct::FAqueductGraphExecutor* Executor, FString& OutError)
{
	if (!Executor)
	{
		OutError = TEXT("Aqueduct graph executor is unavailable");
		return nullptr;
	}
	const FString Json = Executor->GetOutputBindingsJson();
	TSharedPtr<FJsonObject> Outputs;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Outputs) || !Outputs)
	{
		OutError = TEXT("Aqueduct core returned invalid output-binding JSON");
		return nullptr;
	}
	// Unreal recognizes emission_luminance only as a constant. A map bound to it
	// is still a valid output, so delivery proceeds on the default nits value
	// rather than refusing to package the rest of the material.
	const TSharedPtr<FJsonValue>* Luminance = Outputs->Values.Find(TEXT("emission_luminance"));
	if (Luminance && Luminance->IsValid() && (*Luminance)->Type == EJson::String
		&& !(*Luminance)->AsString().IsEmpty())
	{
		UE_LOG(LogAqueductMaterial, Warning,
			TEXT("Aqueduct 'emission_luminance' is bound to a map, which Unreal material packaging does not interpret. Delivering the default 100 cd/m^2. Bind a constant to control emission luminance in Unreal."));
	}
	return Outputs;
}

static FLinearColor DecodeAqueductColor(const FLinearColor& Encoded)
{
	return FLinearColor(
		AqueductSrgbToLinear(Encoded.R), AqueductSrgbToLinear(Encoded.G),
		AqueductSrgbToLinear(Encoded.B), Encoded.A);
}

static FLinearColor DecodeAqueductNormal(const FLinearColor& Encoded, int32 NormalFormat)
{
	// Unreal's normal sampler convention is DirectX. Keep constant and texture
	// bindings identical by applying the same OpenGL green-channel flip as the
	// GPU delivery path before the encoded vector becomes a tangent normal.
	const float Green = NormalFormat == 0 ? 1.0f - Encoded.G : Encoded.G;
	FVector3f Normal(Encoded.R * 2.0f - 1.0f, Green * 2.0f - 1.0f, Encoded.B * 2.0f - 1.0f);
	Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
	return FLinearColor(Normal.X, Normal.Y, Normal.Z, Encoded.A);
}

static FLinearColor DecodeAqueductTangent(const FLinearColor& Encoded)
{
	// Encoded is OpenPBR/Aqueduct 0.5 + 0.5 * (tx, ty) in R, G (+Y = up).
	// Negate Y when writing UE tangent space because Unreal's bitangent follows +V (down).
	const float R = FMath::IsFinite(Encoded.R) ? Encoded.R : 1.0f;
	const float G = FMath::IsFinite(Encoded.G) ? Encoded.G : 0.5f;
	const FVector2f Raw(R * 2.0f - 1.0f, G * 2.0f - 1.0f);
	const float Len = Raw.Size();
	const FVector2f Dir = (FMath::IsFinite(Len) && Len > 1e-5f) ? (Raw / Len) : FVector2f(1.0f, 0.0f);
	return FLinearColor(Dir.X, -Dir.Y, 0.0f, 1.0f);
}

static FLinearColor DecodeAqueductColorClamped(const FLinearColor& Encoded, const FLinearColor& Fallback)
{
	auto Ch = [](float V, float Def) -> float
	{
		return FMath::IsFinite(V) ? AqueductSrgbToLinear(FMath::Clamp(V, 0.0f, 1.0f)) : Def;
	};
	return FLinearColor(Ch(Encoded.R, Fallback.R), Ch(Encoded.G, Fallback.G), Ch(Encoded.B, Fallback.B), 1.0f);
}

static float ClampUnitScalar(float Value, float Fallback)
{
	return FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0f, 1.0f) : Fallback;
}

static void ApplyOutputBindingsToMIC(
	const TSharedPtr<FJsonObject>& Outputs, int32 NormalFormat, UMaterialInstanceConstant* MIC)
{
	if (!Outputs || !MIC) return;
	const float BaseEncoded = 180.0f / 255.0f;
	const FResolvedOutputBinding Base = ResolveVectorBinding(
		Outputs, {TEXT("base_color"), TEXT("albedo")},
		FLinearColor(BaseEncoded, BaseEncoded, BaseEncoded, 1.0f));
	const FResolvedOutputBinding Normal = ResolveVectorBinding(
		Outputs, {TEXT("normal")}, FLinearColor(0.5f, 0.5f, 1.0f, 1.0f));
	const FResolvedOutputBinding Height = ResolveScalarBinding(Outputs, {TEXT("height")}, 0.5f);
	const FResolvedOutputBinding Roughness = ResolveScalarBinding(Outputs, {TEXT("roughness")}, 0.5f);
	const FResolvedOutputBinding Metallic = ResolveScalarBinding(Outputs, {TEXT("metallic")}, 0.0f);
	const FResolvedOutputBinding Ao = ResolveScalarBinding(Outputs, {TEXT("ao")}, 1.0f);
	const FResolvedOutputBinding Opacity = ResolveScalarBinding(Outputs, {TEXT("opacity")}, 1.0f);
	const FResolvedOutputBinding Emissive = ResolveVectorBinding(
		Outputs, {TEXT("emissive")}, FLinearColor::Black);
	const FResolvedOutputBinding Luminance = ResolveScalarBinding(
		Outputs, {TEXT("emission_luminance")}, 100.0f);
	const FResolvedOutputBinding FuzzWeight = ResolveScalarBinding(
		Outputs, {TEXT("fuzz_weight")}, 0.0f);
	const FResolvedOutputBinding FuzzColor = ResolveVectorBinding(
		Outputs, {TEXT("fuzz_color")}, FLinearColor::White);
	const FResolvedOutputBinding Aniso = ResolveScalarBinding(
		Outputs, {TEXT("anisotropy")}, 0.0f);
	const FResolvedOutputBinding Tangent = ResolveVectorBinding(
		Outputs, {TEXT("tangent")}, FLinearColor(1.0f, 0.5f, 0.5f, 1.0f));
	if (Emissive.bExplicit || Luminance.bExplicit)
	{
		static bool bWarnedLegacyExposure = false;
		const IConsoleVariable* ExtendedRange = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"));
		if (!bWarnedLegacyExposure && ExtendedRange && ExtendedRange->GetInt() == 0)
		{
			bWarnedLegacyExposure = true;
			UE_LOG(LogAqueductMaterial, Warning,
				TEXT("Aqueduct emission_luminance is authored in cd/m^2, but this project uses Unreal's deprecated legacy auto-exposure luminance range. Enable extended luminance range for physically calibrated preview; Aqueduct will not rescale or mutate the project setting."));
		}
	}

	auto SetScalar = [MIC](const TCHAR* Name, float Value)
	{
		MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(Name)), Value);
	};
	auto SetVector = [MIC](const TCHAR* Name, const FLinearColor& Value)
	{
		MIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(Name)), Value);
	};
	SetVector(TEXT("BaseColorConstant"), DecodeAqueductColor(Base.Vector));
	SetScalar(TEXT("BaseColorUseTexture"), Base.bTexture ? 1.0f : 0.0f);
	SetVector(TEXT("NormalConstant"), DecodeAqueductNormal(Normal.Vector, NormalFormat));
	SetScalar(TEXT("NormalUseTexture"), Normal.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("HeightConstant"), Height.Scalar);
	SetScalar(TEXT("HeightUseTexture"), Height.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("RoughnessConstant"), Roughness.Scalar);
	SetScalar(TEXT("RoughnessUseTexture"), Roughness.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("MetallicConstant"), Metallic.Scalar);
	SetScalar(TEXT("MetallicUseTexture"), Metallic.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("AoConstant"), Ao.Scalar);
	SetScalar(TEXT("AoUseTexture"), Ao.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("OpacityConstant"), ClampUnitScalar(Opacity.Scalar, 1.0f));
	SetScalar(TEXT("OpacityUseTexture"), Opacity.bTexture ? 1.0f : 0.0f);
	SetVector(TEXT("EmissiveConstant"), DecodeAqueductColor(Emissive.Vector));
	SetScalar(TEXT("EmissiveUseTexture"), Emissive.bTexture ? 1.0f : 0.0f);
	SetScalar(TEXT("EmissionLuminanceNits"), Luminance.Scalar);
	// Fuzz and anisotropy parameters exist only on the parents that shade
	// them; other instances carry no overrides for them.
	float ExistingScalar = 0.0f;
	if (MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(FName("FuzzWeightConstant")), ExistingScalar))
	{
		SetVector(TEXT("FuzzColorConstant"), DecodeAqueductColorClamped(FuzzColor.Vector, FLinearColor::White));
		SetScalar(TEXT("FuzzColorUseTexture"), FuzzColor.bTexture ? 1.0f : 0.0f);
		SetScalar(TEXT("FuzzWeightConstant"), ClampUnitScalar(FuzzWeight.Scalar, 0.0f));
		SetScalar(TEXT("FuzzWeightUseTexture"), FuzzWeight.bTexture ? 1.0f : 0.0f);
	}
	if (MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(FName("AnisotropyConstant")), ExistingScalar))
	{
		SetScalar(TEXT("AnisotropyConstant"), ClampUnitScalar(Aniso.Scalar, 0.0f));
		SetVector(TEXT("TangentConstant"), DecodeAqueductTangent(Tangent.Vector));
		SetScalar(TEXT("AnisotropyUseTexture"), (Aniso.bTexture || Tangent.bTexture) ? 1.0f : 0.0f);
	}
	MIC->MarkPackageDirty();
}

template <typename TExpression>
static TExpression* FindParameterExpression(UMaterial* Mat, const FName& Name)
{
	for (UMaterialExpression* Expression : Mat->GetExpressions())
	{
		if (TExpression* Parameter = Cast<TExpression>(Expression))
		{
			if (Parameter->ParameterName == Name) return Parameter;
		}
	}
	return nullptr;
}

template <typename TExpression>
static TExpression* AddMaterialExpression(UMaterial* Mat, int32 X, int32 Y)
{
	TExpression* Expression = NewObject<TExpression>(Mat);
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Mat->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}

/** Add or repair the Aqueduct-owned material interface without replacing the
 *  material asset or its instances. The schema marker makes the operation
 *  idempotent; only the exact owned material calls this function. */
static bool ReconcileAqueductBaseMaterial(
	UMaterial* Mat, UMaterialExpressionMultiply* TilingMul, const FAqueductParentDef& Def)
{
	// The plain parents keep schema 3. Parents that shade fuzz or anisotropy
	// carry their own, newer graph revision.
	const float SchemaVersion = (Def.bFuzz || Def.bAnisotropy) ? 7.0f : 3.0f;
	UMaterialExpressionScalarParameter* ExistingSchema =
		FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("AqueductSchemaVersion"));
	const bool bRequiredParametersPresent =
		FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName("BaseColorConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("BaseColorUseTexture"))
		&& FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName("NormalConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("NormalUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("HeightConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("HeightUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("RoughnessConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("RoughnessUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("MetallicConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("MetallicUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("AoConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("AoUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("OpacityConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("OpacityUseTexture"))
		&& FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName("EmissiveConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("EmissiveUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("EmissionLuminanceNits"));
	const bool bFuzzParametersPresent =
		FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName("FuzzColorConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("FuzzColorUseTexture"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("FuzzWeightConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("FuzzWeightUseTexture"));
	const bool bAnisotropyParametersPresent =
		FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("AnisotropyConstant"))
		&& FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName("TangentConstant"))
		&& FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName("AnisotropyUseTexture"));
	UMaterialEditorOnlyData* ExistingEditorData = Mat->GetEditorOnlyData();
	// A parent's fuzz and anisotropy inputs are connected exactly when it
	// shades that feature.
	const bool bFuzzWired = ExistingEditorData
		&& ExistingEditorData->SubsurfaceColor.Expression && ExistingEditorData->ClearCoat.Expression;
	const bool bFuzzUnwired = ExistingEditorData
		&& !ExistingEditorData->SubsurfaceColor.Expression && !ExistingEditorData->ClearCoat.Expression;
	const bool bAnisotropyWired = ExistingEditorData
		&& ExistingEditorData->Anisotropy.Expression && ExistingEditorData->Tangent.Expression;
	const bool bAnisotropyUnwired = ExistingEditorData
		&& !ExistingEditorData->Anisotropy.Expression && !ExistingEditorData->Tangent.Expression;
	const bool bShadingModelWiringMatches = ExistingEditorData
		&& ExistingEditorData->ShadingModelFromMaterialExpression.Expression == nullptr;
	if (ExistingSchema && ExistingSchema->DefaultValue >= SchemaVersion
		&& bRequiredParametersPresent && ExistingEditorData
		&& ExistingEditorData->EmissiveColor.Expression && ExistingEditorData->Opacity.Expression
		&& bShadingModelWiringMatches
		&& (Def.bFuzz ? (bFuzzParametersPresent && bFuzzWired) : bFuzzUnwired)
		&& (Def.bAnisotropy ? (bAnisotropyParametersPresent && bAnisotropyWired) : bAnisotropyUnwired))
	{
		return false;
	}

	using AqueductTextureWriter::EAqueductMapKind;
	auto EnsureTexture = [Mat, TilingMul](const TCHAR* Name, EMaterialSamplerType Sampler,
		EAqueductMapKind Kind, int32 Y)
	{
		if (auto* Existing = FindParameterExpression<UMaterialExpressionTextureSampleParameter2D>(Mat, FName(Name)))
		{
			return Existing;
		}
		auto* Parameter = AddMaterialExpression<UMaterialExpressionTextureSampleParameter2D>(Mat, -400, Y);
		Parameter->ParameterName = FName(Name);
		Parameter->SamplerType = Sampler;
		Parameter->Texture = GetOrCreateDefaultTexture(Name, Kind);
		Parameter->Group = FName("Aqueduct");
		if (TilingMul) Parameter->Coordinates.Connect(0, TilingMul);
		return Parameter;
	};
	auto Vector = [Mat](const TCHAR* Name, const FLinearColor& Default, int32 Y)
	{
		if (auto* Existing = FindParameterExpression<UMaterialExpressionVectorParameter>(Mat, FName(Name)))
		{
			Existing->DefaultValue = Default;
			return Existing;
		}
		auto* Parameter = AddMaterialExpression<UMaterialExpressionVectorParameter>(Mat, -200, Y);
		Parameter->ParameterName = FName(Name);
		Parameter->DefaultValue = Default;
		Parameter->Group = FName("Aqueduct");
		return Parameter;
	};
	auto Scalar = [Mat](const TCHAR* Name, float Default, int32 Y)
	{
		if (auto* Existing = FindParameterExpression<UMaterialExpressionScalarParameter>(Mat, FName(Name)))
		{
			Existing->DefaultValue = Default;
			return Existing;
		}
		auto* Parameter = AddMaterialExpression<UMaterialExpressionScalarParameter>(Mat, -200, Y);
		Parameter->ParameterName = FName(Name);
		Parameter->DefaultValue = Default;
		Parameter->Group = FName("Aqueduct");
		return Parameter;
	};
	auto Lerp = [Mat](UMaterialExpression* Constant, int32 ConstantOutput,
		UMaterialExpression* Texture, int32 TextureOutput,
		UMaterialExpression* UseTexture, int32 Y)
	{
		auto* Result = AddMaterialExpression<UMaterialExpressionLinearInterpolate>(Mat, 50, Y);
		Result->A.Connect(ConstantOutput, Constant);
		Result->B.Connect(TextureOutput, Texture);
		Result->Alpha.Connect(0, UseTexture);
		return Result;
	};

	auto* BaseTexture = EnsureTexture(TEXT("BaseColor"), SAMPLERTYPE_Color, EAqueductMapKind::Color, 0);
	auto* NormalTexture = EnsureTexture(TEXT("Normal"), SAMPLERTYPE_Normal, EAqueductMapKind::Normal, 150);
	auto* OrmTexture = EnsureTexture(TEXT("ORM"), SAMPLERTYPE_LinearColor, EAqueductMapKind::Orm, 300);
	EnsureTexture(TEXT("Height"), SAMPLERTYPE_LinearGrayscale, EAqueductMapKind::Gray, 450);
	auto* EmissiveTexture = EnsureTexture(TEXT("Emissive"), SAMPLERTYPE_Color, EAqueductMapKind::Color, 600);

	const float BaseDefault = AqueductSrgbToLinear(180.0f / 255.0f);
	auto* Base = Lerp(
		Vector(TEXT("BaseColorConstant"), FLinearColor(BaseDefault, BaseDefault, BaseDefault, 1.0f), 0), 0,
		BaseTexture, 0, Scalar(TEXT("BaseColorUseTexture"), 0.0f, 40), 0);
	auto* Normal = Lerp(
		Vector(TEXT("NormalConstant"), FLinearColor(0.0f, 0.0f, 1.0f, 1.0f), 150), 0,
		NormalTexture, 0, Scalar(TEXT("NormalUseTexture"), 0.0f, 190), 150);
	auto* Ao = Lerp(Scalar(TEXT("AoConstant"), 1.0f, 300), 0, OrmTexture, 1,
		Scalar(TEXT("AoUseTexture"), 0.0f, 340), 300);
	auto* Roughness = Lerp(Scalar(TEXT("RoughnessConstant"), 0.5f, 390), 0, OrmTexture, 2,
		Scalar(TEXT("RoughnessUseTexture"), 0.0f, 430), 390);
	auto* Metallic = Lerp(Scalar(TEXT("MetallicConstant"), 0.0f, 480), 0, OrmTexture, 3,
		Scalar(TEXT("MetallicUseTexture"), 0.0f, 520), 480);
	Scalar(TEXT("HeightConstant"), 0.5f, 570);
	Scalar(TEXT("HeightUseTexture"), 0.0f, 610);

	auto* Emissive = Lerp(
		Vector(TEXT("EmissiveConstant"), FLinearColor::Black, 660), 0,
		EmissiveTexture, 0, Scalar(TEXT("EmissiveUseTexture"), 0.0f, 700), 660);
	auto* EmissiveNonnegative = AddMaterialExpression<UMaterialExpressionMax>(Mat, 260, 660);
	EmissiveNonnegative->A.Connect(0, Emissive);
	EmissiveNonnegative->ConstB = 0.0f;
	auto* Luminance = Scalar(TEXT("EmissionLuminanceNits"), 100.0f, 760);
	auto* LuminanceNonnegative = AddMaterialExpression<UMaterialExpressionMax>(Mat, 50, 760);
	LuminanceNonnegative->A.Connect(0, Luminance);
	LuminanceNonnegative->ConstB = 0.0f;
	auto* Emission = AddMaterialExpression<UMaterialExpressionMultiply>(Mat, 470, 700);
	Emission->A.Connect(0, EmissiveNonnegative);
	Emission->B.Connect(0, LuminanceNonnegative);

	auto Clamp01 = [Mat](UMaterialExpression* InputExpr, int32 X, int32 Y)
	{
		auto* ClampExpr = AddMaterialExpression<UMaterialExpressionClamp>(Mat, X, Y);
		ClampExpr->Input.Connect(0, InputExpr);
		ClampExpr->MinDefault = 0.0f;
		ClampExpr->MaxDefault = 1.0f;
		return ClampExpr;
	};
	auto* OpacityTexture = EnsureTexture(TEXT("Opacity"), SAMPLERTYPE_LinearGrayscale, EAqueductMapKind::Gray, 950);
	auto* Opacity = Lerp(Scalar(TEXT("OpacityConstant"), 1.0f, 950), 0, OpacityTexture, 1,
		Scalar(TEXT("OpacityUseTexture"), 0.0f, 990), 950);
	auto* OpacityClamped = Clamp01(Opacity, 260, 950);

	UMaterialExpression* FuzzColorClamped = nullptr;
	UMaterialExpression* FuzzWeightClamped = nullptr;
	if (Def.bFuzz)
	{
		auto* FuzzColorTexture = EnsureTexture(TEXT("FuzzColor"), SAMPLERTYPE_Color, EAqueductMapKind::Color, 1080);
		auto* FuzzColor = Lerp(Vector(TEXT("FuzzColorConstant"), FLinearColor::White, 1080), 0,
			FuzzColorTexture, 0, Scalar(TEXT("FuzzColorUseTexture"), 0.0f, 1120), 1080);
		FuzzColorClamped = Clamp01(FuzzColor, 260, 1080);

		auto* FuzzWeightTexture = EnsureTexture(TEXT("FuzzWeight"), SAMPLERTYPE_LinearGrayscale, EAqueductMapKind::Gray, 1200);
		auto* FuzzWeight = Lerp(Scalar(TEXT("FuzzWeightConstant"), 0.0f, 1200), 0,
			FuzzWeightTexture, 1, Scalar(TEXT("FuzzWeightUseTexture"), 0.0f, 1240), 1200);
		FuzzWeightClamped = Clamp01(FuzzWeight, 260, 1200);
	}

	UMaterialExpression* AnisoClamped = nullptr;
	UMaterialExpression* TangentTS = nullptr;
	if (Def.bAnisotropy)
	{
		auto* AnisotropyTexture = EnsureTexture(TEXT("Anisotropy"), SAMPLERTYPE_LinearColor, EAqueductMapKind::Anisotropy, 1340);
		UMaterialExpressionCustom* DecodeAniso = nullptr;
		for (UMaterialExpression* Expr : Mat->GetExpressions())
		{
			if (auto* Custom = Cast<UMaterialExpressionCustom>(Expr))
			{
				if (Custom->Description == TEXT("AqueductDecodeAnisotropy"))
				{
					DecodeAniso = Custom;
					break;
				}
			}
		}
		if (!DecodeAniso)
		{
			DecodeAniso = AddMaterialExpression<UMaterialExpressionCustom>(Mat, -150, 1340);
			DecodeAniso->Description = TEXT("AqueductDecodeAnisotropy");
		}
		DecodeAniso->OutputType = CMOT_Float1;
		DecodeAniso->Inputs.SetNum(1);
		DecodeAniso->Inputs[0].InputName = FName("PackedRG");
		DecodeAniso->Inputs[0].Input.Connect(0, AnisotropyTexture);
		DecodeAniso->AdditionalOutputs.SetNum(1);
		DecodeAniso->AdditionalOutputs[0].OutputName = FName("OutTangentTS");
		DecodeAniso->AdditionalOutputs[0].OutputType = CMOT_Float3;
		DecodeAniso->RebuildOutputs();
		DecodeAniso->Code = TEXT(
			"float2 uw = (PackedRG.rg * 255.0f - 128.0f) / 127.0f;\n"
			"float len = length(uw);\n"
			"if (len > 1e-5f)\n"
			"{\n"
			"    float2 d2 = uw / len;\n"
			"    float tx = sqrt(max(0.5f * (1.0f + d2.x), 0.0f));\n"
			"    float ty = (d2.y >= 0.0f ? 1.0f : -1.0f) * sqrt(max(0.5f * (1.0f - d2.x), 0.0f));\n"
			"    OutTangentTS = float3(tx, -ty, 0.0f);\n"
			"}\n"
			"else\n"
			"{\n"
			"    OutTangentTS = float3(1.0f, 0.0f, 0.0f);\n"
			"}\n"
			"return saturate(len);\n");

		auto* AnisoUseTex = Scalar(TEXT("AnisotropyUseTexture"), 0.0f, 1460);
		auto* Aniso = Lerp(Scalar(TEXT("AnisotropyConstant"), 0.0f, 1340), 0, DecodeAniso, 0, AnisoUseTex, 1340);
		AnisoClamped = Clamp01(Aniso, 260, 1340);
		// bTangentSpaceNormal defaults to true, so UE auto-transforms MP_Tangent to world space.
		TangentTS = Lerp(Vector(TEXT("TangentConstant"), FLinearColor(1.0f, 0.0f, 0.0f, 1.0f), 1400), 0,
			DecodeAniso, 1, AnisoUseTex, 1420);
	}

	auto* Schema = Scalar(TEXT("AqueductSchemaVersion"), SchemaVersion, 850);
	Schema->DefaultValue = SchemaVersion;

	if (UMaterialEditorOnlyData* EditorData = Mat->GetEditorOnlyData())
	{
		EditorData->BaseColor.Connect(0, Base);
		EditorData->Normal.Connect(0, Normal);
		EditorData->AmbientOcclusion.Connect(0, Ao);
		EditorData->Roughness.Connect(0, Roughness);
		EditorData->Metallic.Connect(0, Metallic);
		EditorData->EmissiveColor.Connect(0, Emission);
		EditorData->Opacity.Connect(0, OpacityClamped);
		EditorData->ShadingModelFromMaterialExpression.Expression = nullptr;
		if (Def.bFuzz)
		{
			EditorData->SubsurfaceColor.Connect(0, FuzzColorClamped);
			// MP_CustomData0 is stored in EditorData->ClearCoat (labeled "Cloth" on MSM_Cloth).
			EditorData->ClearCoat.Connect(0, FuzzWeightClamped);
		}
		else
		{
			EditorData->SubsurfaceColor.Expression = nullptr;
			EditorData->ClearCoat.Expression = nullptr;
		}
		if (Def.bAnisotropy)
		{
			EditorData->Anisotropy.Connect(0, AnisoClamped);
			EditorData->Tangent.Connect(0, TangentTS);
		}
		else
		{
			EditorData->Anisotropy.Expression = nullptr;
			EditorData->Tangent.Expression = nullptr;
		}
	}
	return true;
}

UMaterial* FAqueductMaterialManager::GetOrCreateBaseMaterial(EAqueductParentKind ParentKind)
{
	static_assert(UE_ARRAY_COUNT(GParentDefs) == static_cast<int32>(EAqueductParentKind::Count),
		"GParentDefs must have one entry per EAqueductParentKind");
	const FAqueductParentDef& Def = GParentDefs[static_cast<int32>(ParentKind)];
	UMaterial*& CachedMaterial = ParentMaterials[static_cast<int32>(ParentKind)];
	if (CachedMaterial) return CachedMaterial;
	const TCHAR* MaterialPath = Def.PackagePath;
	const TCHAR* AssetName = Def.AssetName;
	const EMaterialShadingModel DesiredShading = Def.ShadingModel;

	UMaterial* Mat = LoadObject<UMaterial>(nullptr, MaterialPath);
	if (Mat)
	{
		bool bChanged = false;
		const bool bShadingMatches = !Mat->IsShadingModelFromMaterialExpression()
			&& Mat->GetShadingModels().HasOnlyShadingModel(DesiredShading);
		if (!bShadingMatches)
		{
			Mat->SetShadingModel(DesiredShading);
			bChanged = true;
		}
		auto* BaseTexture = FindParameterExpression<UMaterialExpressionTextureSampleParameter2D>(
			Mat, FName("BaseColor"));
		auto* ExistingTiling = BaseTexture
			? Cast<UMaterialExpressionMultiply>(BaseTexture->Coordinates.Expression)
			: nullptr;
		if (ReconcileAqueductBaseMaterial(Mat, ExistingTiling, Def) || bChanged)
		{
			Mat->PreEditChange(nullptr);
			Mat->PostEditChange();
			Mat->MarkPackageDirty();
			SaveAssetPackage(Mat);
			UE_LOG(LogAqueductMaterial, Log, TEXT("Reconciled base material schema: %s"), MaterialPath);
		}
		CachedMaterial = Mat;
		return Mat;
	}

	FString DiskDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Aqueduct"));
	IFileManager::Get().MakeDirectory(*DiskDir, true);

	UPackage* Pkg = CreatePackage(MaterialPath);
	if (!Pkg) return nullptr;
	Pkg->FullyLoad();

	const bool bTranslucent = Def.bTranslucent;
	Mat = NewObject<UMaterial>(Pkg, AssetName, RF_Public | RF_Standalone);
	Mat->MaterialDomain = MD_Surface;
	Mat->BlendMode = bTranslucent ? BLEND_Translucent : BLEND_Opaque;
	Mat->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
	Mat->TwoSided = bTranslucent;
	Mat->SetShadingModel(DesiredShading);

	// Tiling: TexCoord × scalar parameter → shared UVs for all samplers
	auto* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
	TexCoord->CoordinateIndex = 0;
	TexCoord->MaterialExpressionEditorX = -700;
	TexCoord->MaterialExpressionEditorY = 0;
	Mat->GetExpressionCollection().AddExpression(TexCoord);

	auto* TilingParam = NewObject<UMaterialExpressionScalarParameter>(Mat);
	TilingParam->ParameterName = FName("Tiling");
	TilingParam->DefaultValue = 1.0f;
	TilingParam->SliderMin = 0.1f;
	TilingParam->SliderMax = 20.0f;
	TilingParam->Group = FName("Aqueduct");
	TilingParam->MaterialExpressionEditorX = -700;
	TilingParam->MaterialExpressionEditorY = 100;
	Mat->GetExpressionCollection().AddExpression(TilingParam);

	auto* TilingMul = NewObject<UMaterialExpressionMultiply>(Mat);
	TilingMul->A.Connect(0, TexCoord);
	TilingMul->B.Connect(0, TilingParam);
	TilingMul->MaterialExpressionEditorX = -550;
	TilingMul->MaterialExpressionEditorY = 0;
	Mat->GetExpressionCollection().AddExpression(TilingMul);

	// Texture parameters — one per delivery, each with the sampler type the
	// engine derives from the bound texture's settings (so the MI editor
	// never warns) and a neutral default texture of matching settings (a
	// mismatched expression texture is a compile ERROR, and the default is
	// what ships for maps a graph doesn't output). The Normal sampler
	// unpacks [0,1]→[-1,1] itself; the ORM sampler's R/G/B channels feed
	// AO/Roughness/Metallic.
	using AqueductTextureWriter::EAqueductMapKind;
	auto MakeTexParam = [Mat, TilingMul](const TCHAR* Name, EMaterialSamplerType SamplerType,
		EAqueductMapKind Kind, int32 YPos)
	{
		auto* Param = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
		Param->ParameterName = FName(Name);
		Param->SamplerType = SamplerType;
		Param->Texture = GetOrCreateDefaultTexture(Name, Kind);
		Param->Group = FName("Aqueduct");
		Param->MaterialExpressionEditorX = -400;
		Param->MaterialExpressionEditorY = YPos;
		Param->Coordinates.Connect(0, TilingMul);
		Mat->GetExpressionCollection().AddExpression(Param);
		return Param;
	};

	auto* BaseColorParam = MakeTexParam(TEXT("BaseColor"), SAMPLERTYPE_Color, EAqueductMapKind::Color, 0);
	auto* NormalParam    = MakeTexParam(TEXT("Normal"), SAMPLERTYPE_Normal, EAqueductMapKind::Normal, 150);
	auto* OrmParam       = MakeTexParam(TEXT("ORM"), SAMPLERTYPE_LinearColor, EAqueductMapKind::Orm, 300);
	auto* HeightParam    = MakeTexParam(TEXT("Height"), SAMPLERTYPE_LinearGrayscale, EAqueductMapKind::Gray, 450);
	(void)HeightParam;  // present for artists (POM etc.); not wired by default

	// Wire to material inputs. Sampler outputs: 0=RGB, 1=R, 2=G, 3=B, 4=A.
	UMaterialEditorOnlyData* EditorData = Mat->GetEditorOnlyData();
	if (EditorData)
	{
		EditorData->BaseColor.Connect(0, BaseColorParam);
		EditorData->Normal.Connect(0, NormalParam);
		EditorData->AmbientOcclusion.Connect(1, OrmParam);  // R
		EditorData->Roughness.Connect(2, OrmParam);         // G
		EditorData->Metallic.Connect(3, OrmParam);          // B
	}
	ReconcileAqueductBaseMaterial(Mat, TilingMul, Def);

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();

	FAssetRegistryModule::AssetCreated(Mat);
	Pkg->MarkPackageDirty();
	SaveAssetPackage(Mat);

	UE_LOG(LogAqueductMaterial, Log, TEXT("Created base material: %s"), MaterialPath);
	CachedMaterial = Mat;
	return Mat;
}

// ---------------------------------------------------------------------------
// MIC management
// ---------------------------------------------------------------------------

UMaterialInstanceConstant* FAqueductMaterialManager::GetOrCreateMIC(const FString& TomlPath)
{
	UMaterial* BaseMat = GetOrCreateBaseMaterial();
	if (!BaseMat) return nullptr;

	FString Dir = FPaths::GetPath(TomlPath);
	FString BaseName = FPaths::GetBaseFilename(TomlPath);
	FString MaterialName = TEXT("MI_") + BaseName;

	FString PkgName;
	if (!MakeAssetPackageName(Dir, MaterialName, PkgName))
		return nullptr;

	FString ObjectPath = PkgName + TEXT(".") + MaterialName;
	UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	if (MIC)
	{
		FGraphState& State = GraphStates.FindOrAdd(TomlPath);
		State.MIC = MIC;
		return MIC;
	}

	UPackage* Pkg = CreatePackage(*PkgName);
	if (!Pkg) return nullptr;
	Pkg->FullyLoad();

	MIC = NewObject<UMaterialInstanceConstant>(Pkg, *MaterialName, RF_Public | RF_Standalone);
	MIC->SetParentEditorOnly(BaseMat);
	MIC->PreEditChange(nullptr);
	MIC->PostEditChange();

	FAssetRegistryModule::AssetCreated(MIC);
	Pkg->MarkPackageDirty();
	SaveAssetPackage(MIC);

	UE_LOG(LogAqueductMaterial, Log, TEXT("Created material instance: %s"), *PkgName);

	FGraphState& State = GraphStates.FindOrAdd(TomlPath);
	State.MIC = MIC;
	return MIC;
}

// ---------------------------------------------------------------------------
// Executor management
// ---------------------------------------------------------------------------

TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> FAqueductMaterialManager::GetOrCreateExecutor(
	const FString& GraphFilePath,
	const FString& RegistryToml,
	const FString& GraphToml)
{
	FGraphState& State = GraphStates.FindOrAdd(GraphFilePath);

	if (!State.Executor)
	{
		State.Executor = MakeShared<UE::Aqueduct::FAqueductGraphExecutor>();
		State.Executor->SetGraphDirectory(FPaths::GetPath(GraphFilePath));
		if (!State.Executor->LoadRegistry(RegistryToml) ||
			!State.Executor->LoadGraph(GraphToml) ||
			!State.Executor->CompileAllShaders())
		{
			State.Executor.Reset();
			return nullptr;
		}
	}

	return State.Executor;
}

TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> FAqueductMaterialManager::FindExecutor(const FString& GraphFilePath)
{
	if (FGraphState* State = GraphStates.Find(GraphFilePath))
		return State->Executor;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Graph wrapper asset
// ---------------------------------------------------------------------------

UAqueductGraph* FAqueductMaterialManager::GetOrCreateGraphAsset(const FString& GraphFilePath)
{
	FString Dir = FPaths::GetPath(GraphFilePath);
	FString BaseName = FPaths::GetBaseFilename(GraphFilePath);
	FString AssetName = BaseName + TEXT("_Graph");

	FString PkgName;
	if (!MakeAssetPackageName(Dir, AssetName, PkgName)) return nullptr;

	FString ObjectPath = PkgName + TEXT(".") + AssetName;
	UAqueductGraph* GraphAsset = LoadObject<UAqueductGraph>(nullptr, *ObjectPath);
	if (GraphAsset) return GraphAsset;

	UPackage* Pkg = CreatePackage(*PkgName);
	if (!Pkg) return nullptr;
	Pkg->FullyLoad();

	GraphAsset = NewObject<UAqueductGraph>(Pkg, *AssetName, RF_Public | RF_Standalone);
	if (const FGraphState* State = GraphStates.Find(FPaths::ConvertRelativePathToFull(GraphFilePath)))
	{
		GraphAsset->SourceDocumentId = State->SourceDocumentId;
	}
	FAssetRegistryModule::AssetCreated(GraphAsset);
	return GraphAsset;
}

// ---------------------------------------------------------------------------
// Output texture assets: permanent MIC bindings, GPU contents overwritten
// in place during live editing / animation
// ---------------------------------------------------------------------------

TArray<FAqueductMaterialManager::FDeliveryDesc>
FAqueductMaterialManager::BuildDeliverySet(UE::Aqueduct::FAqueductGraphExecutor* Executor)
{
	using namespace AqueductTextureWriter;

	TArray<FDeliveryDesc> Deliveries;
	const TMap<FString, FTextureRHIRef>& Outputs = Executor->GetOutputTextures();
	const TMap<FString, FString> OutputHashes = Executor->GetOutputContentHashes();

	bool bAnyOrm = false;
	uint32 OrmResolution = 0;
	bool bAnyAniso = false;
	uint32 AnisoResolution = 0;
	for (const auto& Pair : Outputs)
	{
		if (!Pair.Value.IsValid()) continue;
		if (IsOrmComponentOutput(Pair.Key))
		{
			bAnyOrm = true;
			OrmResolution = Pair.Value->GetSizeX();
			continue;
		}
		if (IsAnisotropyComponentOutput(Pair.Key))
		{
			bAnyAniso = true;
			AnisoResolution = Pair.Value->GetSizeX();
			continue;
		}
		FDeliveryDesc& D = Deliveries.AddDefaulted_GetRef();
		D.Name = Pair.Key;
		D.Kind = ClassifyOutput(Pair.Key, Pair.Value->GetFormat());
		D.OutputNames.Add(Pair.Key);
		D.CombinedHash = DeliveryHashFor(Pair.Key, OutputHashes);
		D.Resolution = Pair.Value->GetSizeX();
	}

	if (bAnyOrm)
	{
		FDeliveryDesc& D = Deliveries.AddDefaulted_GetRef();
		D.Name = OrmDeliveryName();
		D.Kind = EAqueductMapKind::Orm;
		const TCHAR* const* Components = OrmComponentOutputs();
		for (int32 i = 0; i < 3; ++i)
		{
			if (Outputs.Contains(Components[i])) D.OutputNames.Add(Components[i]);
		}
		D.CombinedHash = DeliveryHashFor(OrmDeliveryName(), OutputHashes);
		D.Resolution = OrmResolution;
	}

	if (bAnyAniso)
	{
		FDeliveryDesc& D = Deliveries.AddDefaulted_GetRef();
		D.Name = AnisotropyDeliveryName();
		D.Kind = EAqueductMapKind::Anisotropy;
		const TCHAR* const* Components = AnisotropyComponentOutputs();
		for (int32 i = 0; i < 2; ++i)
		{
			if (Outputs.Contains(Components[i])) D.OutputNames.Add(Components[i]);
		}
		D.CombinedHash = DeliveryHashFor(AnisotropyDeliveryName(), OutputHashes);
		D.Resolution = AnisoResolution;
	}

	return Deliveries;
}

void FAqueductMaterialManager::EnsureOutputTextureAssets(
	const FString& GraphFilePath,
	UE::Aqueduct::FAqueductGraphExecutor* Executor,
	UMaterialInstanceConstant* MIC)
{
	if (!Executor || !MIC) return;

	const TArray<FDeliveryDesc> Deliveries = BuildDeliverySet(Executor);
	if (Deliveries.Num() == 0) return;

	const UE::Aqueduct::FCompiledGraph* CG = Executor->GetCompiledGraph();
	const bool bAnimated = CG && CG->TimeDepNodes.Num() > 0;

	FString Dir = FPaths::GetPath(GraphFilePath);
	FString BaseName = FPaths::GetBaseFilename(GraphFilePath);

	// base_color supersedes the legacy albedo output name on the BaseColor
	// slot: when a graph defines both, albedo still delivers its texture
	// asset but must not bind the material parameter.
	const bool bHasBaseColorDelivery = Deliveries.ContainsByPredicate(
		[](const FDeliveryDesc& D) { return D.Name == TEXT("base_color"); });

	TArray<UTexture*> NewlyCreated;

	for (const FDeliveryDesc& Delivery : Deliveries)
	{
		FString TexName = TEXT("T_") + BaseName + TEXT("_") + Delivery.Name;
		FString PkgName;
		if (!MakeAssetPackageName(Dir, TexName, PkgName)) continue;
		FString ObjectPath = PkgName + TEXT(".") + TexName;

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		if (!Tex)
		{
			UPackage* Pkg = CreatePackage(*PkgName);
			if (!Pkg) continue;
			Pkg->FullyLoad();
			Tex = NewObject<UTexture2D>(Pkg, *TexName, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Tex);

			// Zero-filled Source so the asset is a valid, saveable texture
			// from the moment it exists (with the standard per-map settings:
			// compression class, sRGB, LOD group, mips). The Source is
			// BAKE-time data: live edits (including resolution changes)
			// never touch it — the session's pixels reach materials through
			// the writer's delivery textures, and the bake re-initializes
			// the Source at the graph's then-current resolution.
			AqueductTexture::InitFromPixels(Tex, Delivery.Resolution, Delivery.Kind, bAnimated, nullptr);
			AqueductTexture::GetUserData(Tex, /*bCreate=*/true)->OutputName = Delivery.Name;
			NewlyCreated.Add(Tex);
		}
		else
		{
			const ETextureSourceFormat ExpectedSource = AqueductTexture::SourceFormatForKind(Delivery.Kind);
			const bool bExpectedSrgb =
				Delivery.Kind == AqueductTextureWriter::EAqueductMapKind::Color;
			if (Tex->Source.GetFormat() != ExpectedSource || Tex->SRGB != bExpectedSrgb)
			{
				// Role semantics changed for an older generated asset (notably a
				// pre-role custom `emissive` delivered as gray). Rebuild only this
				// generated texture through its current delivery kind; the live copy
				// below immediately supplies the new pixels.
				AqueductTexture::InitFromPixels(
					Tex, Delivery.Resolution, Delivery.Kind, bAnimated, nullptr);
				if (UAqueductTextureUserData* Meta =
					AqueductTexture::GetUserData(Tex, /*bCreate=*/true))
				{
					Meta->OutputName = Delivery.Name;
					Meta->GraphContentHash.Empty();
				}
				NewlyCreated.Add(Tex);
			}
			else
			{
				AqueductTexture::ApplyDeliverySettings(Tex, Delivery.Kind, bAnimated);
			}
		}

		// Bind to the MIC parameter (idempotent; persisted at bake).
		const bool bSuperseded = bHasBaseColorDelivery && Delivery.Name == TEXT("albedo");
		if (bSuperseded)
		{
			UE_LOG(LogAqueductMaterial, Log,
				TEXT("%s: delivery '%s' superseded by base_color; no parameter bound"),
				*MIC->GetName(), *Delivery.Name);
		}
		else if (const FName* ParamName = FindParamNameForDelivery(Delivery.Name))
		{
			FMaterialParameterInfo Info(*ParamName);
			UTexture* Bound = nullptr;
			MIC->GetTextureParameterValue(Info, Bound);
			if (Bound != Tex)
			{
				MIC->SetTextureParameterValueEditorOnly(Info, Tex);
			}
			UE_LOG(LogAqueductMaterial, Log,
				TEXT("%s: delivery '%s' bound to material parameter '%s'"),
				*MIC->GetName(), *Delivery.Name, *ParamName->ToString());
		}

		FGraphState& State = GraphStates.FindOrAdd(GraphFilePath);
		State.OutputTextures.Add(Delivery.Name, Tex);
	}

	// Cooked static assets use Unreal's own normal-variance composite: the
	// texture builder folds the normal map's Toksvig lengths into the ORM's
	// roughness channel per mip (CompositeTexture, CTM_NormalRoughnessToGreen,
	// Epic's formula), matching what the live delivery composites on the GPU.
	{
		FGraphState& State = GraphStates.FindOrAdd(GraphFilePath);
		if (UTexture2D* Orm =
			State.OutputTextures.FindRef(AqueductTextureWriter::OrmDeliveryName()))
		{
			UTexture2D* NormalTex = State.OutputTextures.FindRef(TEXT("normal"));
			const ECompositeTextureMode WantMode =
				NormalTex ? CTM_NormalRoughnessToGreen : CTM_Disabled;
			if (Orm->GetCompositeTexture() != NormalTex
				|| Orm->CompositeTextureMode != WantMode)
			{
				// PreEditChange blocks the in-flight initial build; mutating a
				// texture property while it builds corrupts the compile manager
				// (fatal in FTextureCompilingManager::AddTextures).
				Orm->PreEditChange(nullptr);
				Orm->SetCompositeTexture(NormalTex);
				Orm->CompositeTextureMode = WantMode;
				Orm->CompositePower = 1.0f;
				Orm->PostEditChange();
				Orm->MarkPackageDirty();
			}
		}
	}

	// Prune deliveries this graph no longer produces (e.g. the user removed an
	// output branch): hand the texture back to its engine (baked) resource and
	// drop the stale tracking, so it doesn't freeze on an orphaned live override
	// for the rest of the session.
	if (FGraphState* State = GraphStates.Find(GraphFilePath))
	{
		TSet<FString> Current;
		for (const FDeliveryDesc& D : Deliveries) Current.Add(D.Name);

		TArray<FString> Removed;
		for (const auto& Pair : State->OutputTextures)
		{
			if (!Current.Contains(Pair.Key)) Removed.Add(Pair.Key);
		}
		for (const FString& Name : Removed)
		{
			if (FTextureResource** Res = State->LiveResources.Find(Name))
			{
				AqueductLiveTexture::RevertLiveResource(State->OutputTextures.FindRef(Name), *Res);
				State->LiveResources.Remove(Name);
			}
			State->OutputTextures.Remove(Name);
		}
	}

	// The GPU resources build asynchronously from Source. Don't block (the
	// engine's wait shows a modal, focus-stealing progress dialog): copies
	// into not-yet-built resources are skipped, and the ticker re-copies
	// dirty graphs every tick until the bake, so pixels land as soon as each
	// resource exists. Unattended runs may block — no dialogs there.
	if (NewlyCreated.Num() > 0 && FApp::IsUnattended())
	{
		FTextureCompilingManager::Get().FinishCompilation(NewlyCreated);
	}
}

void FAqueductMaterialManager::CopyOutputsToTextures(
	const FString& GraphFilePath,
	UE::Aqueduct::FAqueductGraphExecutor* Executor)
{
	using namespace AqueductTextureWriter;

	FGraphState* State = GraphStates.Find(GraphFilePath);
	if (!State || !Executor || State->OutputTextures.Num() == 0) return;

	const TMap<FString, FTextureRHIRef>& Outputs = Executor->GetOutputTextures();

	// Each session texture is backed by a plugin-owned uncompressed
	// FAqueductLiveTextureResource (installed below); the delivery chain packs +
	// mips the executor outputs straight into it and repoints the asset's RHI
	// reference at it — so every surface samples live, decoupled from the
	// engine's async build pipeline.
	TArray<FDeliveryTarget> Targets;
	for (const auto& TexPair : State->OutputTextures)
	{
		if (!TexPair.Value) continue;
		if (!TexPair.Value->TextureReference.TextureReferenceRHI.IsValid()) continue;

		FDeliveryTarget T;
		T.Name = TexPair.Key;

		// Classify the delivery and find its source(s) + resolution first — the
		// live resource is sized/formatted from them.
		FTextureRHIRef SizeSrc;
		if (TexPair.Key == OrmDeliveryName())
		{
			T.Kind = EAqueductMapKind::Orm;
			const TCHAR* const* Components = OrmComponentOutputs();
			bool bAny = false;
			for (int32 i = 0; i < 3; ++i)
			{
				T.OrmSrc[i] = Outputs.FindRef(Components[i]);
				if (T.OrmSrc[i].IsValid() && !SizeSrc.IsValid()) SizeSrc = T.OrmSrc[i];
				bAny |= T.OrmSrc[i].IsValid();
			}
			if (!bAny) continue;
		}
		else if (TexPair.Key == AnisotropyDeliveryName())
		{
			T.Kind = EAqueductMapKind::Anisotropy;
			const TCHAR* const* Components = AnisotropyComponentOutputs();
			T.AnisoSrc[0] = Outputs.FindRef(Components[0]);
			T.AnisoSrc[1] = Outputs.FindRef(Components[1]);
			T.AnisotropyDefault = State->CachedAnisoConstants.Anisotropy;
			T.TangentDefault = State->CachedAnisoConstants.TangentDir;
			SizeSrc = T.AnisoSrc[0].IsValid() ? T.AnisoSrc[0] : T.AnisoSrc[1];
			if (!SizeSrc.IsValid()) continue;
		}
		else
		{
			T.Src = Outputs.FindRef(TexPair.Key);
			if (!T.Src.IsValid()) continue;
			T.Kind = ClassifyOutput(TexPair.Key, T.Src->GetFormat());
			T.bFlipNormalGreen =
				(T.Kind == EAqueductMapKind::Normal && Executor->GetNormalFormat() == 0) ? 1 : 0;
			SizeSrc = T.Src;
		}

		// Install/maintain the uncompressed live-texture resource on the asset
		// so EVERY surface samples live: the reference (viewport + material
		// preview) and GetResource()->TextureRHI (texture editor + thumbnails).
		EnsureLiveResource(*State, TexPair.Key, TexPair.Value, T.Kind, SizeSrc->GetSizeX());

		// Pin the asset resource's RHI texture NOW (game thread): the render
		// command must never dereference a raw FTextureResource* that the engine
		// could release before execution. EnsureLiveResource above installed our
		// live resource SYNCHRONOUSLY, so this is its uncompressed texture (the
		// delivery destination). If the engine is still async-building the asset,
		// EnsureLiveResource left the engine resource in place and the writer
		// skips this target (format mismatch) until the build completes.
		if (FTextureResource* Resource = TexPair.Value->GetResource())
		{
			T.DstTexture = Resource->TextureRHI;
		}
		T.TextureReference = TexPair.Value->TextureReference.TextureReferenceRHI;

		Targets.Add(MoveTemp(T));
	}

	// Delivery is UNCONDITIONAL — see the header. The install is synchronous but
	// the texture starts EMPTY; this delivery is what fills it, and re-delivering
	// every tick refills + re-asserts the texture reference after any engine
	// disturbance (resource reverts on texture-editor / material-preview opens).
	// Scratch is render-thread-owned; the game thread only holds the map shell.
	EnqueueDeliverOutputs(MoveTemp(Targets), &State->DeliveryScratch);
}

void FAqueductMaterialManager::EnsureLiveResource(
	FGraphState& State, const FString& DeliveryName, UTexture2D* Tex,
	AqueductTextureWriter::EAqueductMapKind Kind, uint32 Size)
{
	AqueductTextureWriter::EnsureLiveDeliveryResource(
		Tex, Kind, Size, State.LiveResources.FindOrAdd(DeliveryName));
}

void FAqueductMaterialManager::RevertLiveResources(FGraphState& State)
{
	for (auto& Pair : State.LiveResources)
	{
		UTexture2D* Tex = State.OutputTextures.FindRef(Pair.Key);
		AqueductLiveTexture::RevertLiveResource(Tex, Pair.Value);
	}
	State.LiveResources.Empty();
}

// ---------------------------------------------------------------------------
// Lazy bake: the pre-save hook
//
// Live edits never bake — they mark packages dirty. The exact moment baked
// pixels matter is when one of our assets is about to be serialized: the
// engine broadcasts OnObjectPreSave before writing each object, and PreSave
// is the designed window for refreshing derived data. The first stale object
// of a graph triggers a blocking bake of the whole graph (saves already
// block, by user intent); every other object of that graph then sees a fresh
// hash and skips. Covers Ctrl+S, Save All, and the exit prompt with one
// mechanism.
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
	// Cooking: there is no executor-capable RHI in the cook commandlet, so a
	// bake is impossible; UAqueductGraph::PreSave covers the cook's needs
	// (precompiled shader metadata) and the startup hash guard covers
	// staleness. Procedural saves (our own unattended saves after a bake)
	// see fresh hashes and fall through the staleness check below.
	if (SaveContext.IsCooking() || !bInitialized) return;

	static bool bInPreSaveBake = false;
	if (bInPreSaveBake) return;

	// Resolve the graph file this object belongs to. A texture or wrapper
	// that has never been baked has no recorded graph reference yet — those
	// are exactly the ones that MUST bake, so fall back to a reverse lookup
	// through the live session state.
	FString GraphPath;
	FString AssetHash;
	FString OutputName; // set for textures: per-output staleness
	if (UTexture2D* Tex = Cast<UTexture2D>(Object))
	{
		UAqueductTextureUserData* Meta = AqueductTexture::GetUserData(Tex, /*bCreate=*/false);
		if (!Meta) return; // not one of ours
		AssetHash = Meta->GraphContentHash;
		OutputName = Meta->OutputName;
		if (Meta->SourceGraph && !Meta->SourceGraph->GraphName.IsEmpty())
		{
			GraphPath = RecipePathNextToAsset(Object, Meta->SourceGraph->GraphName);
		}
		else
		{
			for (const auto& Pair : GraphStates)
			{
				for (const auto& TexPair : Pair.Value.OutputTextures)
				{
					if (TexPair.Value == Tex) { GraphPath = Pair.Key; break; }
				}
				if (!GraphPath.IsEmpty()) break;
			}
		}
	}
	else if (const UAqueductGraph* GraphAsset = Cast<UAqueductGraph>(Object))
	{
		AssetHash = GraphAsset->ContentHash;
		if (!GraphAsset->GraphName.IsEmpty())
		{
			GraphPath = RecipePathNextToAsset(Object, GraphAsset->GraphName);
		}
		else
		{
			for (const auto& Pair : GraphStates)
			{
				if (Pair.Value.GraphAsset == GraphAsset) { GraphPath = Pair.Key; break; }
			}
		}
	}
	else
	{
		return;
	}

	if (GraphPath.IsEmpty()) return;
	GraphPath = FPaths::ConvertRelativePathToFull(GraphPath);
	if (!FPaths::FileExists(GraphPath)) return;

	// Staleness: per-output closure hash for textures, whole-graph hash for
	// the wrapper. Session state has current hashes; otherwise parse-only
	// via the core (no executor, no compile).
	FString CurrentWholeHash;
	TMap<FString, FString> CurrentOutputHashes;
	if (const FGraphState* State = GraphStates.Find(GraphPath))
	{
		CurrentWholeHash = State->CurrentHash;
		CurrentOutputHashes = State->OutputContentHashes;
	}
	if (CurrentWholeHash.IsEmpty())
	{
		FString GraphToml;
		if (!FFileHelper::LoadFileToString(GraphToml, *GraphPath)) return;
		if (CachedRegistryToml.IsEmpty())
		{
			CachedRegistryToml = AqueductNodeAssets::BuiltinRegistryToml();
			if (CachedRegistryToml.IsEmpty())
			{
				return;
			}
		}
		if (!UE::Aqueduct::FAqueductGraphExecutor::ComputeContentHashes(
				CachedRegistryToml, GraphToml, CurrentWholeHash, CurrentOutputHashes))
		{
			return;
		}
	}

	if (!OutputName.IsEmpty())
	{
		// OutputName is the DELIVERY name; the ORM texture's hash combines
		// its three contributing outputs' closure hashes.
		const FString DeliveryHash = DeliveryHashFor(OutputName, CurrentOutputHashes);
		UE_LOG(LogAqueductMaterial, Verbose,
			TEXT("Pre-save check: %s delivery=%s assetHash=%s currentHash=%s"),
			*Object->GetName(), *OutputName, *AssetHash.Left(10),
			DeliveryHash.IsEmpty() ? TEXT("<none>") : *DeliveryHash.Left(10));
		if (!DeliveryHash.IsEmpty() && AssetHash == DeliveryHash) return; // this texture is fresh
	}
	else
	{
		UE_LOG(LogAqueductMaterial, Verbose,
			TEXT("Pre-save check: %s (wrapper) assetHash=%s currentHash=%s"),
			*Object->GetName(), *AssetHash.Left(10), *CurrentWholeHash.Left(10));
		if (AssetHash == CurrentWholeHash)
		{
			return; // wrapper is fresh
		}
	}

	UE_LOG(LogAqueductMaterial, Log,
		TEXT("Pre-save: %s is stale, baking before save: %s"),
		*Object->GetName(), *GraphPath);

	bInPreSaveBake = true;
	BakeGraph(GraphPath, /*bAllowUnattendedSave=*/false);
	bInPreSaveBake = false;
}

// ---------------------------------------------------------------------------
// Recipe-dies-with-the-wrapper (the editor-delete handshake; see header)
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::OnAssetsPreDelete(const TArray<UObject*>& Objects)
{
	// One pending set per delete attempt: a cancelled dialog's recordings
	// must not linger into a later, unrelated removal.
	PendingRecipeDeletes.Empty();

	for (UObject* Object : Objects)
	{
		const UAqueductGraph* GraphAsset = Cast<UAqueductGraph>(Object);
		if (!GraphAsset) continue;

		// The recipe lives next to the wrapper. GraphName is authoritative
		// once baked; an unbaked wrapper falls back to its asset name.
		FString GraphName = GraphAsset->GraphName;
		if (GraphName.IsEmpty())
		{
			GraphName = GraphAsset->GetName();
			GraphName.RemoveFromEnd(TEXT("_Graph"));
		}
		PendingRecipeDeletes.Add(
			FSoftObjectPath(GraphAsset), RecipePathNextToAsset(GraphAsset, GraphName));
	}
}

void FAqueductMaterialManager::OnWrapperAssetRemoved(const FAssetData& AssetData)
{
	if (PendingRecipeDeletes.Num() == 0) return;

	FString RecipePath;
	if (!PendingRecipeDeletes.RemoveAndCopyValue(AssetData.GetSoftObjectPath(), RecipePath))
	{
		return;
	}
	if (!FPaths::FileExists(RecipePath)) return;

	// The graph is gone: drop its session first so nothing keeps executing
	// or re-delivering into deleted textures.
	if (FGraphState* State = GraphStates.Find(RecipePath))
	{
		FlushRenderingCommands();
		RevertLiveResources(*State);
		GraphStates.Remove(RecipePath);
	}

	// Source-control delete (Perforce: mark-for-delete removes the local
	// file too); plain delete is the fallback for untracked files.
	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (SCModule.IsEnabled())
	{
		TSharedRef<FDelete> DeleteOp = ISourceControlOperation::Create<FDelete>();
		SCModule.GetProvider().Execute(
			DeleteOp, TArray<FString>{RecipePath}, EConcurrency::Synchronous);
	}
	if (FPaths::FileExists(RecipePath))
	{
		IFileManager::Get().Delete(*RecipePath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
	}

	// Leave no husk: the engine deletes the siblings' package files AFTER
	// this broadcast (CleanupAfterSuccessfulDelete), so the folder cannot be
	// judged yet. Sweep on a later tick: if the delete emptied it, remove
	// it; if anything legitimate remains, leave it alone.
	const FString Dir = FPaths::GetPath(RecipePath);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Dir](float) -> bool
		{
			TArray<FString> Remaining;
			IFileManager::Get().FindFilesRecursive(Remaining, *Dir, TEXT("*"), true, false);
			if (Remaining.Num() == 0)
			{
				IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
			}
			return false; // one-shot
		}), 1.0f);

	UE_LOG(LogAqueductMaterial, Log,
		TEXT("Wrapper asset deleted in editor; deleted its recipe: %s"), *RecipePath);
}

// ---------------------------------------------------------------------------
// Animated-graph residency
// ---------------------------------------------------------------------------

bool FAqueductMaterialManager::IsGraphVisible(const FGraphState& State)
{
	// A texture's LastRenderTime advances whenever a draw actually samples it
	// (the same signal the engine's texture streamer keys off). Fresh within
	// a few frames → something on screen is using this graph.
	const double Now = FApp::GetCurrentTime();
	for (const auto& Pair : State.OutputTextures)
	{
		if (!Pair.Value) continue;
		if (Now - Pair.Value->GetLastRenderTimeForStreaming() < 0.3) return true;
	}
	return false;
}

void FAqueductMaterialManager::PrepareOneReadyPendingGraph()
{
	for (auto& Pair : GraphStates)
	{
		if (Pair.Value.Tier != FGraphState::EAnimatedTier::ReadyPending) continue;

		const FString Path = Pair.Key;
		const FString GraphToml = Pair.Value.GraphToml;

		if (CachedRegistryToml.IsEmpty())
		{
			CachedRegistryToml = AqueductNodeAssets::BuiltinRegistryToml();
			if (CachedRegistryToml.IsEmpty())
			{
				Pair.Value.Tier = FGraphState::EAnimatedTier::None;
				return;
			}
		}

		const double T0 = FPlatformTime::Seconds();
		TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor =
			GetOrCreateExecutor(Path, CachedRegistryToml, GraphToml);
		FGraphState* State = GraphStates.Find(Path); // rehash-safe
		if (!State) return;
		if (!Executor)
		{
			UE_LOG(LogAqueductMaterial, Error,
				TEXT("Background readiness compile failed: %s"), *Path);
			State->Tier = FGraphState::EAnimatedTier::None;
			return;
		}
		State->Tier = FGraphState::EAnimatedTier::Ready;
		UE_LOG(LogAqueductMaterial, Log,
			TEXT("Animated graph ready (compiled, zero working VRAM, %.1f ms): %s"),
			(FPlatformTime::Seconds() - T0) * 1000.0, *Path);
		return; // one per tick — readiness never stalls the editor
	}
}

void FAqueductMaterialManager::PromoteToLive(const FString& GraphFilePath)
{
	FGraphState* State = GraphStates.Find(GraphFilePath);
	if (!State || !State->Executor) return;

	const double T0 = FPlatformTime::Seconds();
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor = State->Executor;

	UMaterialInstanceConstant* MIC = State->MIC ? State->MIC.Get() : GetOrCreateMIC(GraphFilePath);
	State = GraphStates.Find(GraphFilePath); // rehash-safe

	// Clock continuity across Live ↔ Ready transitions: animation time is
	// wall-clock from first promotion, like world time.
	if (State->AnimationStartTime == 0.0)
	{
		State->AnimationStartTime = FPlatformTime::Seconds();
	}
	Executor->SetTime((float)(FPlatformTime::Seconds() - State->AnimationStartTime));
	Executor->EvaluateTimeExpressions();

	// Ready → Live: allocate working VRAM + one dispatch. Never a compile.
	ENQUEUE_RENDER_COMMAND(AqueductPromote)([Exec = Executor](FRHICommandListImmediate& RHICmdList)
	{
		Exec->Execute(RHICmdList);
	});
	FlushRenderingCommands();

	EnsureOutputTextureAssets(GraphFilePath, Executor.Get(), MIC);
	State = GraphStates.Find(GraphFilePath);
	if (!State) return;
	State->CachedAnisoConstants = AqueductTextureWriter::ResolveAnisotropyConstantsFromBindingsJson(
		Executor->GetOutputBindingsJson());
	CopyOutputsToTextures(GraphFilePath, Executor.Get());
	InvalidateViewportsForChangedPixels();

	State->Tier = FGraphState::EAnimatedTier::Live;
	State->LastVisibleTime = FApp::GetCurrentTime();

	UE_LOG(LogAqueductMaterial, Log,
		TEXT("Animated graph live (warm-up %.1f ms, visibility to frame submitted): %s"),
		(FPlatformTime::Seconds() - T0) * 1000.0, *GraphFilePath);
}

// ---------------------------------------------------------------------------
// Ticker: animated-graph residency + post-edit resource copies
// ---------------------------------------------------------------------------

void FAqueductMaterialManager::RegisterAnimatedGraph(
	const FString& GraphFilePath,
	TSharedPtr<UE::Aqueduct::FAqueductGraphExecutor> Executor,
	double AnimationStartTime)
{
	FGraphState& State = GraphStates.FindOrAdd(GraphFilePath);
	State.Executor = Executor;
	if (State.AnimationStartTime == 0.0)
	{
		State.AnimationStartTime = AnimationStartTime;
	}
	State.Tier = FGraphState::EAnimatedTier::Live;
	State.LastVisibleTime = FApp::GetCurrentTime();
	EnsureTicker();
}

void FAqueductMaterialManager::UnregisterAnimatedGraph(const FString& GraphFilePath)
{
	if (FGraphState* State = GraphStates.Find(GraphFilePath))
		State->Tier = FGraphState::EAnimatedTier::None;
}

void FAqueductMaterialManager::EnsureTicker()
{
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FAqueductMaterialManager::Tick), 0.0f);
	}
}

bool FAqueductMaterialManager::Tick(float DeltaTime)
{
	// Background readiness: at most one shader preparation per tick.
	PrepareOneReadyPendingGraph();

	const float IdleRelease = AqueductAnimated::IdleReleaseSeconds;
	const double Now = FApp::GetCurrentTime();
	bool bKeepTicking = false;
	TArray<FString> ToPromote;

	for (auto& Pair : GraphStates)
	{
		FGraphState& State = Pair.Value;

		// Live-resource maintenance: while a graph has unsaved edits, re-assert
		// the live resource and re-deliver every tick. The engine reverts our
		// live resource on routine editor actions (opening the texture editor or
		// a material preview calls UpdateResource); EnsureLiveResource then
		// re-installs it (synchronously) and the unconditional delivery refills
		// it and re-points the texture reference — so any such disturbance is
		// repaired on the next tick. A Live animated graph maintains itself
		// through its animation branch below, so it is excluded here. Maintenance
		// ends at the bake (which clears the flag): from then on Source holds the
		// real pixels and the engine's built resource is the truth.
		if (State.bMaintainLiveResource && State.Executor &&
			State.Tier != FGraphState::EAnimatedTier::Live)
		{
			// NO viewport invalidation: these re-copies rewrite identical pixels,
			// so invalidating would force every non-realtime editor viewport to
			// repaint while edits are unsaved.
			CopyOutputsToTextures(Pair.Key, State.Executor.Get());
			bKeepTicking = true;
		}

		switch (State.Tier)
		{
		case FGraphState::EAnimatedTier::None:
			continue;

		case FGraphState::EAnimatedTier::ReadyPending:
			bKeepTicking = true;
			continue;

		case FGraphState::EAnimatedTier::Ready:
			bKeepTicking = true;
			if (IsGraphVisible(State))
			{
				ToPromote.Add(Pair.Key); // PromoteToLive mutates GraphStates
			}
			continue;

		case FGraphState::EAnimatedTier::Live:
		{
			bKeepTicking = true;
			if (!State.Executor) continue;

			if (IsGraphVisible(State))
			{
				State.LastVisibleTime = Now;
			}
			else if (Now - State.LastVisibleTime > IdleRelease)
			{
				// Nothing on screen samples this graph: stop paying for it.
				// Fence first: prior ticks' execute/delivery commands hold the
				// executor and &DeliveryScratch — release only after the render
				// thread has drained (matches every sibling release site;
				// structural guarantee, not an idle-threshold accident).
				FlushRenderingCommands();
				State.Executor->ReleaseGraphResources();
				State.DeliveryScratch.Empty();
				// Honor the Ready tier's zero-working-VRAM invariant: hand the
				// textures back to their engine resources, freeing the live
				// uncompressed textures. Promotion back to Live reinstalls them.
				RevertLiveResources(State);
				State.Tier = FGraphState::EAnimatedTier::Ready;
				UE_LOG(LogAqueductMaterial, Log,
					TEXT("Animated graph idle %.0fs, released working VRAM: %s"),
					IdleRelease, *Pair.Key);
				continue;
			}
			else
			{
				// Unsampled but within the idle grace window: don't execute
				// (nothing displays the result), keep resources warm.
				continue;
			}

			float Time = (float)(FPlatformTime::Seconds() - State.AnimationStartTime);
			State.Executor->SetTime(Time);
			State.Executor->EvaluateTimeExpressions();

			ENQUEUE_RENDER_COMMAND(AqueductPersistentTick)([Exec = State.Executor](FRHICommandListImmediate& RHICmdList)
			{
				Exec->Execute(RHICmdList);
			});

			CopyOutputsToTextures(Pair.Key, State.Executor.Get());
			InvalidateViewportsForChangedPixels();
			continue;
		}
		}
	}

	for (const FString& Path : ToPromote)
	{
		PromoteToLive(Path);
	}

	if (!bKeepTicking && ToPromote.Num() == 0)
	{
		TickerHandle.Reset();
		return false;
	}
	return true;
}
