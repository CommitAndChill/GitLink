#pragma once

#include <CoreMinimal.h>
#include <UObject/Object.h>
#include <UObject/WeakObjectPtrTemplates.h>

// --------------------------------------------------------------------------------------------------------------------
// Self-contained validity helpers.
//
// GitLink is a standalone plugin — it deliberately does not depend on CkCore/CkFoundation, so we
// provide our own thin IsValid / Is_NOT_Valid wrappers here. They read the same as the Ck helpers
// so code style matches, but no header from any Ck module is required.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	template <typename T>
	FORCEINLINE auto IsValid(const T* InPtr) -> bool
	{
		return InPtr != nullptr;
	}

	template <typename T>
	FORCEINLINE auto IsValid(const TSharedPtr<T>& InPtr) -> bool
	{
		return InPtr.IsValid();
	}

	template <typename T>
	FORCEINLINE auto IsValid(const TSharedRef<T>& /*InRef*/) -> bool
	{
		return true;
	}

	template <typename T>
	FORCEINLINE auto IsValid(const TWeakPtr<T>& InPtr) -> bool
	{
		return InPtr.IsValid();
	}

	template <typename T>
	FORCEINLINE auto IsValid(const TWeakObjectPtr<T>& InPtr) -> bool
	{
		return InPtr.IsValid();
	}

	FORCEINLINE auto IsValid(const UObject* InObject) -> bool
	{
		return ::IsValid(InObject);
	}

	FORCEINLINE auto IsValid(const FString& InString) -> bool
	{
		return !InString.IsEmpty();
	}

	FORCEINLINE auto IsValid(const FName& InName) -> bool
	{
		return !InName.IsNone();
	}

	template <typename T>
	FORCEINLINE auto Is_NOT_Valid(const T& InValue) -> bool
	{
		return !gitlink::IsValid(InValue);
	}
}
