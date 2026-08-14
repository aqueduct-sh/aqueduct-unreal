#include "AqueductPaths.h"
#include "AqueductFfi.h"

#include "aqueduct.h"

namespace AqueductNodeAssets
{

FString BuiltinRegistryToml()
{
	return UE::Aqueduct::FfiToString(aqueduct_builtin_registry_toml());
}

} // namespace AqueductNodeAssets

namespace AqueductConventions
{

bool IsValidGraphName(const FString& Name)
{
	FTCHARToUTF8 Utf8(*Name);
	return aqueduct_is_valid_graph_name(Utf8.Get());
}

} // namespace AqueductConventions
