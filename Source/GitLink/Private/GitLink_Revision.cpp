#include "GitLink/GitLink_Revision.h"

#include "GitLinkLog.h"

// --------------------------------------------------------------------------------------------------------------------
// All three expensive-fetch methods are stubs in v1. They will be wired up to libgit2's
// git_blob_lookup + git_blob_rawcontent flow once the editor features that depend on them
// (diff-against-depot, history view, blame) need to work.
// --------------------------------------------------------------------------------------------------------------------

#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_Revision::Get(FString& /*InOutFilename*/, EConcurrency::Type /*InConcurrency*/) const -> bool
#else
auto FGitLink_Revision::Get(FString& /*InOutFilename*/) const -> bool
#endif
{
	UE_LOG(LogGitLink, Verbose, TEXT("FGitLink_Revision::Get stub called for '%s'"), *_Filename);
	return false;
}

auto FGitLink_Revision::GetAnnotated(TArray<FAnnotationLine>& /*OutLines*/) const -> bool
{
	return false;
}

auto FGitLink_Revision::GetAnnotated(FString& /*InOutFilename*/) const -> bool
{
	return false;
}
