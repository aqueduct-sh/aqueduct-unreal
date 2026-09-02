// AqueductAssets: the project-resident content-addressed asset store
// (content-addressed bitmap assets).
//
// Layout: Content/Aqueduct/AqueductAssets/<sha256>.<ext> — raw normalized
// files, deliberately NOT imported as UTexture2D assets (raw files stay
// byte-identical to the bucket copy and hash-verifiable). The directory is
// project content: checked into source control alongside the graph's
// generated assets, and staged into cooked builds via
// DirectoriesToAlwaysStageAsUFS so the runtime reads it from the pak.
//
// Files are write-once: a file existing under its hash name is valid by
// construction (downloads are hash-verified before an atomic move).
// Deleting files is safe; the editor re-downloads by hash.

#pragma once

#include "CoreMinimal.h"

namespace AqueductAssets
{
	/** AqueductAssets directory inside project content (also the staged UFS path). */
	AQUEDUCTRUNTIME_API FString RootDir();

	/** Content-relative path used for staging registration. */
	AQUEDUCTRUNTIME_API const TCHAR* ContentRelativePath();

	/**
	 * Parse a whitelisted asset URL. Only our storage domains are valid:
	 * storage.aqueduct.sh, storage-dev.aqueduct.sh, storage.iliad.ai, or
	 * storage-dev.iliad.ai, followed by /assets/<64 lowercase hex>.<png|exr>.
	 */
	AQUEDUCTRUNTIME_API bool ParseAssetUrl(const FString& Url, FString& OutHash, FString& OutExtension);

	/** Absolute path of an asset in AqueductAssets. */
	AQUEDUCTRUNTIME_API FString AssetFilePath(const FString& Sha256, const FString& Extension);

	/** True when the bitmap `source` param value is a URL (vs a filepath). */
	AQUEDUCTRUNTIME_API bool IsAssetUrl(const FString& Source);

	/**
	 * Resolve a bitmap `source` param to file bytes. URLs read from
	 * AqueductAssets (never the network; downloads are the editor module's
	 * job); paths resolve relative to the graph directory. On a miss the
	 * canonical URL is appended to OutMissingUrls and false is returned.
	 */
	AQUEDUCTRUNTIME_API bool LoadSourceBytes(
		const FString& Source,
		const FString& GraphDirectory,
		TArray<uint8>& OutBytes,
		FString& OutError,
		TArray<FString>& OutMissingUrls);

	/**
	 * Hash-verify and atomically place downloaded bytes into AqueductAssets
	 * (temp file + move; partial writes never occupy a final name).
	 */
	AQUEDUCTRUNTIME_API bool StoreVerifiedAsset(
		const FString& Sha256,
		const FString& Extension,
		const TArray<uint8>& Bytes,
		FString& OutError);
}
