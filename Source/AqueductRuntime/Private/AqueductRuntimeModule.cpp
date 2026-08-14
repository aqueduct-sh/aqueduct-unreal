#include "AqueductRuntimeModule.h"
#include "AqueductGraphExecutor.h"
#include "AqueductPaths.h"
#include "AqueductRuntimeGraphSystem.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ShaderCore.h"
#include "AqueductFfi.h"

namespace UE::Aqueduct
{

#if WITH_EDITOR
/**
 * Cook-time shader registration. The cooker compiles the target platform's
 * global shader map early in the cook; FAqueductDynamicCS permutations only
 * cook if their HLSL is registered in FAqueductNodeShaderStorage at that
 * point. So at cook startup we compile every animated graph in the project,
 * which registers their node shader permutations. UAqueductGraph::PreSave
 * later re-derives the same metadata (RegisterOrReuse dedups by content
 * hash, so indices match) and records it into the cooked assets.
 */
static void RegisterAnimatedGraphShadersForCook()
{
	const FString IliadDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Aqueduct")));
	// Graphs live in per-graph subfolders
	// (Content/Aqueduct/<name>/<name>.aqueduct), so scan recursively. A
	// non-recursive scan misses them entirely, and their node-shader
	// permutations never get registered for cook — the graph would silently
	// fail to animate in a shipped game. FindFilesRecursive returns full paths.
	TArray<FString> GraphFiles;
	IFileManager::Get().FindFilesRecursive(GraphFiles, *IliadDir, TEXT("*.aqueduct"), true, false);
	if (GraphFiles.Num() == 0) return;

	const FString RegistryToml = AqueductNodeAssets::BuiltinRegistryToml();
	if (RegistryToml.IsEmpty())
	{
		UE_LOG(LogTemp, Error,
			TEXT("AqueductRuntime cook: embedded registry unavailable — animated graphs will not ship"));
		return;
	}

	int32 Registered = 0;
	for (const FString& GraphPath : GraphFiles)
	{
		FString GraphToml;
		if (!FFileHelper::LoadFileToString(GraphToml, *GraphPath))
		{
			continue;
		}

		FAqueductGraphExecutor Executor;
		if (!Executor.LoadRegistry(RegistryToml) || !Executor.LoadGraph(GraphToml))
		{
			continue;
		}

		// Animated check without compiling: any time-expression values at t=0?
		Executor.SetTime(0.0f);
		if (!Executor.HasTimeExpressions())
		{
			continue;
		}

		if (Executor.CompileAllShaders())
		{
			Registered++;
		}
		else
		{
			UE_LOG(LogTemp, Error,
				TEXT("AqueductRuntime cook: shader compile failed for %s"), *GraphPath);
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("AqueductRuntime cook: registered node shader permutations for %d animated graph(s)"),
		Registered);
}
#endif // WITH_EDITOR

void FAqueductRuntimeModule::StartupModule()
{
#if PLATFORM_WINDOWS || PLATFORM_MAC
	// The Rust core is a delay-loaded shared library on these platforms;
	// resolve it explicitly from the plugin's own tree so loading never
	// depends on system search paths. Linux links it directly with a baked
	// rpath and needs no preload.
	const FString FfiLibPath = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("Aqueduct"))->GetBaseDir(),
		TEXT("Source/AqueductLib/lib"),
#if PLATFORM_WINDOWS
		TEXT("Win64/aqueduct_ffi.dll"));
#else
		TEXT("Mac/libaqueduct_ffi.dylib"));
#endif
	FfiDllHandle = FPlatformProcess::GetDllHandle(*FfiLibPath);
	checkf(FfiDllHandle, TEXT("Aqueduct: failed to load Rust core %s"), *FfiLibPath);
#endif

	// Register shader source directory so UE can find our .usf files.
	// Must happen before global shader map compilation.
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("Aqueduct"))->GetBaseDir(),
		TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/Aqueduct"), PluginShaderDir);

	const FString Version = UE::Aqueduct::FfiToString(aqueduct_version());
	if (!Version.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("AqueductRuntime loaded — FFI version %s, shaders: %s"), *Version, *PluginShaderDir);
	}

	// Warm the embedded Slang compiler during startup dead time so the first
	// custom-node compile never pays its one-time setup.
	Async(EAsyncExecution::Thread, []()
	{
		if (!aqueduct_compiler_warm())
		{
			UE_LOG(LogTemp, Warning, TEXT("Aqueduct Slang compiler warm-up failed: %s"),
				*UE::Aqueduct::FfiToString(aqueduct_last_error()));
		}
	});

#if WITH_EDITOR
	if (IsRunningCookCommandlet())
	{
		RegisterAnimatedGraphShadersForCook();
	}
#endif

	// Game-runtime animated graph playback (cooked games and -game mode;
	// no-op in the editor, where the MaterialManager owns playback). If the
	// engine is already up (late module load), initialize directly — the
	// delegate would never fire.
	if (GEngine)
	{
		if (!GIsEditor)
		{
			FAqueductRuntimeGraphSystem::Get().Initialize();
		}
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			if (!GIsEditor)
			{
				FAqueductRuntimeGraphSystem::Get().Initialize();
			}
		});
	}
}

void FAqueductRuntimeModule::ShutdownModule()
{
	if (FfiDllHandle)
	{
		FPlatformProcess::FreeDllHandle(FfiDllHandle);
		FfiDllHandle = nullptr;
	}

	// Unregister BEFORE shutdown: a stale delegate firing after module
	// unload would call into unloaded code.
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	FAqueductRuntimeGraphSystem::Get().Shutdown();
}

} // namespace UE::Aqueduct

IMPLEMENT_MODULE(UE::Aqueduct::FAqueductRuntimeModule, AqueductRuntime)
