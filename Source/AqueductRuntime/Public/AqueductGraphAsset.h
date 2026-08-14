#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "AqueductGraphAsset.generated.h"

class UTexture2D;

/**
 * Thin asset wrapper around an Aqueduct graph TOML.
 *
 * The loose .aqueduct file next to this asset remains the editing source of
 * truth (it is what the web editor syncs, what the file watcher watches, and
 * what diffs as text in version control). This wrapper is auto-synced from
 * the file so Unreal's asset machinery sees the graph natively: dependency
 * tracking (Material Instance → AqueductTextures → this), cook staging, and
 * asset-registry visibility.
 */
UCLASS()
class AQUEDUCTRUNTIME_API UAqueductGraph : public UObject
{
	GENERATED_BODY()

public:
	/** Human-readable Unreal destination basename, derived from the recipe filename. */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	FString GraphName;

	/** Full graph TOML, synced from the loose .aqueduct file at bake time. */
	UPROPERTY()
	FString GraphToml;

	/** SHA1 hex of GraphToml — the bake staleness guard compares against this. */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	FString ContentHash;

	/** Output names this graph produces (e.g. base_color, normal, roughness). */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	TArray<FString> OutputNames;

	/** Whether the graph contains time expressions (animated). */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	bool bAnimated = false;

	/** The output texture assets (hard cook dependency: cooking this graph
	 *  asset stages its textures, which stage the material chain). */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	TArray<TSoftObjectPtr<UTexture2D>> OutputTextures;

	/**
	 * Node registry TOML the graph was authored against, captured at bake.
	 * Lets shipped games parse and execute the graph with zero loose files.
	 */
	UPROPERTY()
	FString RegistryToml;

	/**
	 * Cook-time shader metadata: per-node permutation indices into the cooked
	 * global shader map, plus binding semantics. Populated during cook save
	 * (PreSave); empty in uncooked assets. Shipped games execute from this —
	 * they cannot compile shaders.
	 */
	UPROPERTY()
	FString CookedMetadataJson;

	/**
	 * The cloud document id this graph was last synced from (empty for
	 * local-file documents and pre-stamp graphs). It reconnects later syncs
	 * to the established Unreal destination after a cloud display-name
	 * change, and guards that destination against a different document.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Aqueduct")
	FString SourceDocumentId;

	//~ UObject
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
};
