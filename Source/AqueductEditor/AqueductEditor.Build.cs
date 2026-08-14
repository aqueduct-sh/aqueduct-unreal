// Aqueduct Editor module: material and asset management.
// Contains NO graph editing UI; the only graph editor is the Iliad web app.
//
// The C++ here is unstable internals with no supported API; the product
// surface is assets, materials, and the live link.

using UnrealBuildTool;

public class AqueductEditor : ModuleRules
{
	public AqueductEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Strict compilation: catch problems at compile time, not as runtime crashes.
		bWarningsAsErrors = true;
		bUseUnity = false;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Error;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"RenderCore",
			"RHI",
			"Json",
			"JsonUtilities",
			"ImageCore",
			"DirectoryWatcher",
			"Projects",
			"WebSocketNetworking",
			"SourceControl",
			"HTTP",
			"DeveloperToolSettings",
			"DataValidation",
			"AssetTools",
			"AssetRegistry",
			"AqueductLib",
			"AqueductRuntime"
		});
	}
}
