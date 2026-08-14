#pragma once

#include "CoreMinimal.h"

/**
 * Built-in node assets, served by the Rust FFI from the set embedded in the
 * static library (a dev overlay via AQUEDUCT_DEV_NODES is unioned inside
 * Rust). No registry file or nodes directory exists on disk; cooked games
 * never call this (graphs carry everything they need).
 */
namespace AqueductNodeAssets
{
	/** The built-in node registry TOML. Empty only on FFI failure. */
	AQUEDUCTRUNTIME_API FString BuiltinRegistryToml();
}

/**
 * Cross-consumer conventions, delegated to the Rust core (single source of
 * truth shared with the web editor and CLI). Exposed through this module so
 * dependent modules never link FFI symbols directly.
 */
namespace AqueductConventions
{
	/** Is `name` a valid graph name (file stem / live-link identifier)? */
	AQUEDUCTRUNTIME_API bool IsValidGraphName(const FString& Name);
}
