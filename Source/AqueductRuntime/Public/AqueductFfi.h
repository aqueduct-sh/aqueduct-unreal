// FFI call-site helpers: RAII ownership for strings returned by the Rust
// library, plus one-line JSON parsing for the FFI's JSON-shaped returns.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "aqueduct.h"

namespace UE::Aqueduct
{

/** Owns a heap string returned by the FFI; frees it on scope exit. */
class FFfiString
{
public:
	explicit FFfiString(char* InRaw) : Raw(InRaw) {}
	~FFfiString()
	{
		if (Raw)
		{
			aqueduct_string_free(Raw);
		}
	}
	FFfiString(const FFfiString&) = delete;
	FFfiString& operator=(const FFfiString&) = delete;

	bool IsValid() const { return Raw != nullptr; }
	FString ToString() const { return Raw ? FString(UTF8_TO_TCHAR(Raw)) : FString(); }

private:
	char* Raw = nullptr;
};

inline TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	FJsonSerializer::Deserialize(Reader, Obj);
	return Obj;
}

inline TSharedPtr<FJsonValue> ParseJsonValue(const FString& Json)
{
	TSharedPtr<FJsonValue> Value;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	FJsonSerializer::Deserialize(Reader, Value);
	return Value;
}

inline TArray<FString> ParseJsonStringArray(const FString& Json)
{
	TArray<FString> Result;
	TArray<TSharedPtr<FJsonValue>> Values;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (FJsonSerializer::Deserialize(Reader, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			Result.Add(Value->AsString());
		}
	}
	return Result;
}

/** Consume an FFI string as a parsed JSON object (null-safe). */
inline TSharedPtr<FJsonObject> FfiJsonObject(char* Raw)
{
	const FFfiString Owned(Raw);
	return Owned.IsValid() ? ParseJsonObject(Owned.ToString()) : nullptr;
}

/** Consume an FFI string as a parsed JSON string array (null-safe). */
inline TArray<FString> FfiJsonStringArray(char* Raw)
{
	const FFfiString Owned(Raw);
	return Owned.IsValid() ? ParseJsonStringArray(Owned.ToString()) : TArray<FString>();
}

/** Consume an FFI string as an FString (null-safe, empty on null). */
inline FString FfiToString(char* Raw)
{
	const FFfiString Owned(Raw);
	return Owned.ToString();
}

} // namespace UE::Aqueduct
