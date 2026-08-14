#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Containers/Ticker.h"

namespace UE::Aqueduct
{

class FAqueductEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** One-shot deferred-init ticker; removed at shutdown if it never fired
	 *  (firing after module unload would call into unloaded code). */
	FTSTicker::FDelegateHandle DeferredInitTickerHandle;
};

} // namespace UE::Aqueduct
