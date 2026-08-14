#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

namespace UE::Aqueduct
{

class FAqueductRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FAqueductRuntimeModule& Get()
	{
		return FModuleManager::GetModuleChecked<FAqueductRuntimeModule>("AqueductRuntime");
	}

private:
	/** Removed at shutdown so a late engine-init can't call unloaded code. */
	FDelegateHandle PostEngineInitHandle;

	/** Delay-load platforms only: the Rust core dylib, resolved explicitly
	 *  from the plugin's own tree before any FFI call. */
	void* FfiDllHandle = nullptr;
};

} // namespace UE::Aqueduct
