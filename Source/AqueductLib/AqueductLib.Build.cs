// Third-party external module: links the prebuilt Rust aqueduct-ffi shared
// library. A shared library rather than a static archive: dead code is
// already eliminated (the distributed file stays small), internal Rust
// symbols stay hidden, and every plugin module shares one copy of the Rust
// runtime's state.

using UnrealBuildTool;
using System.IO;

public class AqueductLib : ModuleRules
{
	public AqueductLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Header include path
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

		string LibDir = Path.Combine(ModuleDirectory, "lib");

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			// UBT links the .so by path and, because the path contains
			// "Plugins", bakes an $ORIGIN-relative rpath to its directory;
			// RuntimeDependencies stages it into packaged games at the same
			// plugin-relative location, keeping that rpath valid.
			string So = Path.Combine(LibDir, "Linux", "libaqueduct_ffi.so");
			PublicAdditionalLibraries.Add(So);
			RuntimeDependencies.Add(So);
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// Delay-loaded and resolved by the explicit GetDllHandle preload
			// in AqueductRuntimeModule, so nothing depends on install names
			// or system search paths.
			string Dylib = Path.Combine(LibDir, "Mac", "libaqueduct_ffi.dylib");
			PublicDelayLoadDLLs.Add(Dylib);
			RuntimeDependencies.Add(Dylib);
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(Path.Combine(LibDir, "Win64", "aqueduct_ffi.dll.lib"));
			PublicDelayLoadDLLs.Add("aqueduct_ffi.dll");
			RuntimeDependencies.Add(Path.Combine(LibDir, "Win64", "aqueduct_ffi.dll"));
		}
	}
}
