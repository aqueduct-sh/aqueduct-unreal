// AqueductEditor module: material lifecycle and live-link integration.

#include "AqueductEditorModule.h"
#include "AqueductMaterialManager.h"
#include "AqueductAssets.h"
#include "AqueductPaths.h"
#include "Settings/ProjectPackagingSettings.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "AqueductLiveLink.h"
#include "Containers/Ticker.h"

namespace UE::Aqueduct
{
// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * Register AqueductAssets for cooked-build staging
	 * (DirectoriesToAlwaysStageAsUFS is relative to Content). Idempotent:
	 * persists into DefaultGame.ini only when missing, so cooked games can
	 * read AqueductAssets files through the pak via the UFS file API.
	 */
	void EnsureAqueductAssetsStaged()
	{
		UProjectPackagingSettings* Settings = GetMutableDefault<UProjectPackagingSettings>();
		if (!Settings)
		{
			return;
		}
		const FString AssetsPath = AqueductAssets::ContentRelativePath();
		for (const FDirectoryPath& Dir : Settings->DirectoriesToAlwaysStageAsUFS)
		{
			if (Dir.Path == AssetsPath)
			{
				return;
			}
		}
		FDirectoryPath Entry;
		Entry.Path = AssetsPath;
		Settings->DirectoriesToAlwaysStageAsUFS.Add(Entry);
		Settings->TryUpdateDefaultConfigFile();
		UE_LOG(LogTemp, Log, TEXT("Registered %s for cooked staging"), *AssetsPath);
	}

	/**
	 * Register Content/Aqueduct for cooking. Graph wrapper assets are not
	 * referenced by any cooked object (materials reference only the delivery
	 * textures), so without this the cooker drops them and a shipped game
	 * loses its animated graphs. Idempotent, like the staging entry above.
	 */
	void EnsureAqueductGraphsCooked()
	{
		UProjectPackagingSettings* Settings = GetMutableDefault<UProjectPackagingSettings>();
		if (!Settings)
		{
			return;
		}
		const FString CookPath = TEXT("/Game/Aqueduct");
		for (const FDirectoryPath& Dir : Settings->DirectoriesToAlwaysCook)
		{
			if (Dir.Path == CookPath)
			{
				return;
			}
		}
		FDirectoryPath Entry;
		Entry.Path = CookPath;
		Settings->DirectoriesToAlwaysCook.Add(Entry);
		Settings->TryUpdateDefaultConfigFile();
		UE_LOG(LogTemp, Log, TEXT("Registered %s for cooking"), *CookPath);
	}

	/**
	 * Exclude AqueductAssets from the editor's auto-import monitor. The
	 * directory holds raw hash-named .png/.exr files inside Content, which
	 * the default "/Game/" monitor would otherwise surface as "a source
	 * content file has been detected, import it?" — and importing one would
	 * create a junk UTexture2D. The exclusion uses the same wildcard
	 * mechanism the engine's own default config uses for its Localization
	 * folder.
	 * Idempotent; persists via the settings object's own PostEditChangeProperty
	 * (SaveConfig + change broadcast, which makes the running
	 * FAutoReimportManager rebuild its monitors).
	 */
	void EnsureAqueductAssetsAutoImportExcluded()
	{
		UEditorLoadingSavingSettings* Settings = GetMutableDefault<UEditorLoadingSavingSettings>();
		if (!Settings)
		{
			return;
		}
		const FString WildcardStr = FString(AqueductAssets::ContentRelativePath()) + TEXT("/*");

		bool bChanged = false;
		for (FAutoReimportDirectoryConfig& Config : Settings->AutoReimportDirectorySettings)
		{
			if (Config.SourceDirectory != TEXT("/Game/"))
			{
				continue;
			}
			bool bAlreadyExcluded = false;
			for (const FAutoReimportWildcard& Wildcard : Config.Wildcards)
			{
				if (Wildcard.Wildcard == WildcardStr && !Wildcard.bInclude)
				{
					bAlreadyExcluded = true;
					break;
				}
			}
			if (!bAlreadyExcluded)
			{
				FAutoReimportWildcard Exclude;
				Exclude.Wildcard = WildcardStr;
				Exclude.bInclude = false;
				Config.Wildcards.Add(Exclude);
				bChanged = true;
			}
		}

		if (bChanged)
		{
			FProperty* Prop = UEditorLoadingSavingSettings::StaticClass()->FindPropertyByName(
				GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings));
			FPropertyChangedEvent Event(Prop);
			// Redeclared protected on the settings class; invoke through the
			// public UObject declaration (virtual dispatch reaches the
			// override), the same interface the property editor uses.
			static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);
			UE_LOG(LogTemp, Log, TEXT("Excluded %s from auto-import monitoring"), *WildcardStr);
		}
	}
}

void FAqueductEditorModule::StartupModule()
{
	// Defer material manager initialization — SavePackage crashes during module loading.
	DeferredInitTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			EnsureAqueductAssetsStaged();
			EnsureAqueductGraphsCooked();
			EnsureAqueductAssetsAutoImportExcluded();
			FAqueductMaterialManager::Get().EnsureInitialized();
			FAqueductLiveLink::Get().Start();
			return false; // one-shot
		}), 0.0f);

	UE_LOG(LogTemp, Log, TEXT("AqueductEditor loaded"));
}

void FAqueductEditorModule::ShutdownModule()
{
	// If the one-shot init ticker hasn't fired yet, remove it: firing after
	// module unload would call into unloaded code.
	if (DeferredInitTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredInitTickerHandle);
		DeferredInitTickerHandle.Reset();
	}
	FAqueductLiveLink::Get().Stop();
	FAqueductMaterialManager::Get().Shutdown();
}

} // namespace UE::Aqueduct

IMPLEMENT_MODULE(UE::Aqueduct::FAqueductEditorModule, AqueductEditor)
