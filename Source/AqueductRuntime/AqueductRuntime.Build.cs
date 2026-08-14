// Aqueduct Runtime module — GPU compute dispatch via UE's shader system.
//
// Shaders are compiled through UE's standard pipeline (.usf → platform bytecode).
// This module is DRM-free and ships with packaged games.
//
// The C++ here is unstable internals with no supported API; the product
// surface is assets, materials, and the live link.

using UnrealBuildTool;

public class AqueductRuntime : ModuleRules
{
	public AqueductRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Strict compilation: catch problems at compile time, not as runtime crashes.
		bWarningsAsErrors = true;
		bUseUnity = false;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Error;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
			"Json",
			"JsonUtilities",
			"Projects",
			"DeveloperSettings",
			"AssetRegistry",
			"ImageCore",
			"AqueductLib"
		});
	}
}
