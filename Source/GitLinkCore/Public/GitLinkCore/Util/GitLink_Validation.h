#pragma once

#include <CoreMinimal.h>
#include <UObject/Object.h>
#include <UObject/WeakObjectPtrTemplates.h>

// --------------------------------------------------------------------------------------------------------------------
// Self-contained validity helpers.
//
// Thin IsValid / Is_NOT_Valid wrappers for common UE types. GitLink is a standalone plugin with
// no external module dependencies, so we provide our own rather than pulling in a utility library.
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
