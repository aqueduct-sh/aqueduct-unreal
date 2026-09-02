#include "AqueductAssets.h"

#include "aqueduct.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	// Our storage domains, one per environment. URLs are minted from each
	// environment's PUBLIC_STORAGE_URL config; the whitelist accepts all of
	// ours so graphs resolve wherever the content-addressed object exists.
	constexpr const TCHAR* AssetUrlPrefixes[] = {
		TEXT("https://storage.aqueduct.sh/assets/"),
		TEXT("https://storage-dev.aqueduct.sh/assets/"),
		TEXT("https://storage.iliad.ai/assets/"),
		TEXT("https://storage-dev.iliad.ai/assets/"),
	};

	bool IsLowerHex64(const FString& S)
	{
		if (S.Len() != 64)
		{
			return false;
		}
		for (TCHAR C : S)
		{
			const bool bDigit = C >= TEXT('0') && C <= TEXT('9');
			const bool bLower = C >= TEXT('a') && C <= TEXT('f');
			if (!bDigit && !bLower)
			{
				return false;
			}
		}
		return true;
	}
}

namespace AqueductAssets
{
	const TCHAR* ContentRelativePath()
	{
		return TEXT("Aqueduct/AqueductAssets");
	}

	FString RootDir()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), ContentRelativePath());
	}

	bool IsAssetUrl(const FString& Source)
	{
		return Source.StartsWith(TEXT("https://")) || Source.StartsWith(TEXT("http://"));
	}

	bool ParseAssetUrl(const FString& Url, FString& OutHash, FString& OutExtension)
	{
		const TCHAR* MatchedPrefix = nullptr;
		for (const TCHAR* Prefix : AssetUrlPrefixes)
		{
			if (Url.StartsWith(Prefix))
			{
				MatchedPrefix = Prefix;
				break;
			}
		}
		if (!MatchedPrefix)
		{
			return false;
		}
		const FString Rest = Url.Mid(FCString::Strlen(MatchedPrefix));
		FString Hash;
		FString Extension;
		if (!Rest.Split(TEXT("."), &Hash, &Extension))
		{
			return false;
		}
		if (!IsLowerHex64(Hash))
		{
			return false;
		}
		if (Extension != TEXT("png") && Extension != TEXT("exr"))
		{
			return false;
		}
		OutHash = MoveTemp(Hash);
		OutExtension = MoveTemp(Extension);
		return true;
	}

	FString AssetFilePath(const FString& Sha256, const FString& Extension)
	{
		return FPaths::Combine(RootDir(), Sha256 + TEXT(".") + Extension);
	}

	bool LoadSourceBytes(
		const FString& Source,
		const FString& GraphDirectory,
		TArray<uint8>& OutBytes,
		FString& OutError,
		TArray<FString>& OutMissingUrls)
	{
		if (IsAssetUrl(Source))
		{
			FString Hash;
			FString Extension;
			if (!ParseAssetUrl(Source, Hash, Extension))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a valid asset URL (only <whitelisted storage domain>/assets/<sha256>.<png|exr> is allowed)"),
					*Source);
				return false;
			}
			const FString FilePath = AssetFilePath(Hash, Extension);
			if (FFileHelper::LoadFileToArray(OutBytes, *FilePath))
			{
				return true;
			}
			OutMissingUrls.AddUnique(Source);
			OutError = FString::Printf(
				TEXT("asset %s.%s is not in the project's AqueductAssets yet"), *Hash, *Extension);
			return false;
		}

		FString FilePath = Source;
		if (FPaths::IsRelative(FilePath) && !GraphDirectory.IsEmpty())
		{
			FilePath = FPaths::Combine(GraphDirectory, FilePath);
		}
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		if (FFileHelper::LoadFileToArray(OutBytes, *FilePath))
		{
			return true;
		}
		OutError = FString::Printf(TEXT("failed to read '%s'"), *FilePath);
		return false;
	}

	bool StoreVerifiedAsset(
		const FString& Sha256,
		const FString& Extension,
		const TArray<uint8>& Bytes,
		FString& OutError)
	{
		FTCHARToUTF8 HashUtf8(*Sha256);
		if (Bytes.Num() == 0 ||
			aqueduct_asset_verify(Bytes.GetData(), Bytes.Num(), HashUtf8.Get()) != 1)
		{
			OutError = FString::Printf(
				TEXT("downloaded bytes do not hash to %s; refusing to store"), *Sha256);
			return false;
		}

		const FString FinalPath = AssetFilePath(Sha256, Extension);
		IFileManager& FM = IFileManager::Get();
		if (FM.FileExists(*FinalPath))
		{
			return true; // write-once: already present
		}
		const FString TempPath = FinalPath + FString::Printf(TEXT(".%u.tmp"), FPlatformProcess::GetCurrentProcessId());
		if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
		{
			OutError = FString::Printf(TEXT("failed to write %s"), *TempPath);
			return false;
		}
		if (!FM.Move(*FinalPath, *TempPath, /*Replace*/ false, /*EvenIfReadOnly*/ false))
		{
			FM.Delete(*TempPath);
			// A concurrent writer winning the race is success.
			if (FM.FileExists(*FinalPath))
			{
				return true;
			}
			OutError = FString::Printf(TEXT("failed to move %s into AqueductAssets"), *TempPath);
			return false;
		}
		return true;
	}
}
