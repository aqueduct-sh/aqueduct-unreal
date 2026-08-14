// Helpers shared by the AqueductMaterialManager translation units.

#pragma once

#include "CoreMinimal.h"

/** Package name for an asset in a directory; false if the path is invalid. */
bool MakeAssetPackageName(const FString& Dir, const FString& AssetName, FString& OutPkgName);

/** Save an asset's package to disk (validation suppressed, unattended-safe). */
void SaveAssetPackage(UObject* Asset);
