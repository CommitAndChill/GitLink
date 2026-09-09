#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::Get_ConfigString / op::Get_HeadSymbolicRefName — in-process equivalents of the two cheapest
// and most frequently needed `git` invocations.
//
// These exist so callers that already hold an open FRepository never have to spawn a `git`
// subprocess to read a config value or the current ref. A process spawn on Windows costs ~20 ms
// measured; a libgit2 config lookup on an open repository is a hash probe against an already-
// parsed config snapshot. At connect time GitLink resolves LFS endpoints for the parent repo plus
// every submodule, so the difference is 3 spawns x 32 repos of blocked game-thread time versus
// nothing measurable.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
    auto Get_ConfigString(FRepository& InRepo, const FString& InKey) -> FString
    {
        FString Out;

#if WITH_LIBGIT2
        if (!InRepo.IsOpen() || InKey.IsEmpty())
        { return Out; }

        FScopeLock Lock(&InRepo.Get_Mutex());

        git_config* RawConfig = nullptr;
        // git_repository_config returns the FULL merged chain (system -> global -> local), which
        // is the same resolution `git config --get <key>` performs. git_repository_config_snapshot
        // would be cheaper for repeated reads but returns a frozen view; we want the live chain so
        // a value written after open is observed, matching the subprocess behaviour we replace.
        if (const int32 Rc = git_repository_config(&RawConfig, InRepo.Get_RawHandle()); Rc < 0 || RawConfig == nullptr)
        {
            if (RawConfig) { git_config_free(RawConfig); }
            UE_LOG(LogGitLinkCore, Verbose, TEXT("Get_ConfigString('%s'): git_repository_config failed: %s"),
                *InKey, *libgit2::Get_LastErrorMessage());
            return Out;
        }

        // No RAII wrapper exists for git_config in FGitLink_Libgit2_Handles.h and adding one for a
        // single call site would be noise; the two exits below are the only paths out.
        git_buf Value = {nullptr, 0, 0};
        const int32 GetRc = git_config_get_string_buf(&Value, RawConfig, TCHAR_TO_UTF8(*InKey));
        if (GetRc == 0 && Value.ptr != nullptr)
        {
            Out = FString(UTF8_TO_TCHAR(Value.ptr));
        }
        else if (GetRc != GIT_ENOTFOUND)
        {
            // GIT_ENOTFOUND is the normal answer for an unset key (`git config --get` exits 1) and
            // must stay quiet — see GitLink v0.3.7, which demoted exactly this case to Verbose on
            // the subprocess path. Anything else is a real failure worth naming.
            UE_LOG(LogGitLinkCore, Verbose, TEXT("Get_ConfigString('%s'): git_config_get_string_buf failed: %s"),
                *InKey, *libgit2::Get_LastErrorMessage());
        }

        git_buf_dispose(&Value);
        git_config_free(RawConfig);
#endif  // WITH_LIBGIT2

        return Out;
    }

    auto Get_HeadSymbolicRefName(FRepository& InRepo) -> FString
    {
        FString Out;

#if WITH_LIBGIT2
        if (!InRepo.IsOpen())
        { return Out; }

        FScopeLock Lock(&InRepo.Get_Mutex());

        git_reference* RawHead = nullptr;
        // git_reference_lookup (not git_repository_head) on purpose: we want HEAD ITSELF, not what
        // it resolves to. `git symbolic-ref --quiet HEAD` prints the symbolic target and exits 1
        // on a detached HEAD; git_repository_head would happily hand back a direct reference there
        // and we would report a ref name for a state that has none.
        if (git_reference_lookup(&RawHead, InRepo.Get_RawHandle(), "HEAD") < 0 || RawHead == nullptr)
        {
            if (RawHead) { git_reference_free(RawHead); }
            UE_LOG(LogGitLinkCore, Verbose, TEXT("Get_HeadSymbolicRefName: HEAD lookup failed: %s"),
                *libgit2::Get_LastErrorMessage());
            return Out;
        }
        libgit2::FReferencePtr Head(RawHead);

        if (git_reference_type(Head.Get()) != GIT_REFERENCE_SYMBOLIC)
        {
            // Detached HEAD. Empty is the correct answer, and the LFS spec marks `ref` optional on
            // /locks/verify, so callers keep working.
            return Out;
        }

        if (const char* Target = git_reference_symbolic_target(Head.Get()))
        {
            Out = FString(UTF8_TO_TCHAR(Target));
        }
#endif  // WITH_LIBGIT2

        return Out;
    }
}
