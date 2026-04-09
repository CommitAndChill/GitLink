#include "GitLink_Libgit2_Error.h"

#if WITH_LIBGIT2

#include <git2.h>

namespace gitlink::libgit2
{
	auto Get_LastErrorMessage() -> FString
	{
		const git_error* Err = git_error_last();
		if (Err == nullptr || Err->message == nullptr)
		{ return TEXT("(no libgit2 error set)"); }

		return FString(UTF8_TO_TCHAR(Err->message));
	}

	auto MakeFailResult(const TCHAR* InContext, int32 InErrorCode) -> FResult
	{
		return FResult::Fail(FString::Printf(
			TEXT("%s: libgit2 error %d: %s"),
			InContext,
			InErrorCode,
			*Get_LastErrorMessage()));
	}

	auto CheckResult(const TCHAR* InContext, int32 InErrorCode) -> FResult
	{
		if (InErrorCode >= 0)
		{ return FResult::Ok(); }

		return MakeFailResult(InContext, InErrorCode);
	}
}

#endif  // WITH_LIBGIT2
