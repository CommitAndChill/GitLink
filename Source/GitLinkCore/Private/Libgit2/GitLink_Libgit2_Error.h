#pragma once

#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// Helpers for converting libgit2 error codes into gitlink::FResult.
// Only included from .cpp files inside GitLinkCore — keeps git2.h out of public headers.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_LIBGIT2

namespace gitlink::libgit2
{
	// Pull the most recent libgit2 error message into a plain FString.
	// Safe to call any time; returns "(no libgit2 error set)" if none.
	GITLINKCORE_API auto Get_LastErrorMessage() -> FString;

	// Build a failing FResult whose ErrorMessage incorporates the libgit2 last error + a context prefix.
	GITLINKCORE_API auto MakeFailResult(const TCHAR* InContext, int32 InErrorCode) -> FResult;

	// Convenience: given an int returned from a libgit2 call, return Ok() on >=0 or MakeFailResult otherwise.
	GITLINKCORE_API auto CheckResult(const TCHAR* InContext, int32 InErrorCode) -> FResult;
}

#endif  // WITH_LIBGIT2
