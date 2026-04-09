#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// OID <-> FString conversion helpers. Private header — only .cpp files inside GitLinkCore
// should include this, because it pulls in git2.h.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_LIBGIT2

#include <git2/oid.h>

namespace gitlink::libgit2
{
	// Convert a libgit2 OID to a full 40-char hex string.
	FORCEINLINE auto OidToString(const git_oid& InOid) -> FString
	{
		char Buf[GIT_OID_HEXSZ + 1] = {0};
		git_oid_tostr(Buf, sizeof(Buf), &InOid);
		return FString(UTF8_TO_TCHAR(Buf));
	}

	// Convert a libgit2 OID to an abbreviated hex string (default 7 chars).
	FORCEINLINE auto OidToShortString(const git_oid& InOid, int32 InLen = 7) -> FString
	{
		InLen = FMath::Clamp(InLen, 4, GIT_OID_HEXSZ);
		char Buf[GIT_OID_HEXSZ + 1] = {0};
		git_oid_tostr(Buf, InLen + 1, &InOid);
		return FString(UTF8_TO_TCHAR(Buf));
	}

	// Parse a hex SHA string into an OID. Returns true on success.
	FORCEINLINE auto OidFromString(const FString& InHex, git_oid& OutOid) -> bool
	{
		const FTCHARToUTF8 Utf8(*InHex);
		return git_oid_fromstr(&OutOid, Utf8.Get()) == 0;
	}
}

#endif  // WITH_LIBGIT2
