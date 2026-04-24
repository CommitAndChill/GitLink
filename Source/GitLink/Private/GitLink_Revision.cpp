#include "GitLink/GitLink_Revision.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <HAL/FileManager.h>
#include <Misc/FileHelper.h>
#include <Misc/Paths.h>
#include <ISourceControlModule.h>

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Revision::Get — extracts the file content at this revision to a temp file so the
// editor can diff it against the current working copy or another revision.
//
// Uses `git cat-file --filters <commit>:<repo-relative-path>` via subprocess.
// --------------------------------------------------------------------------------------------------------------------

#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_Revision::Get(FString& InOutFilename, EConcurrency::Type /*InConcurrency*/) const -> bool
#else
auto FGitLink_Revision::Get(FString& InOutFilename) const -> bool
#endif
{
	// Access the provider to get the subprocess and repo root.
	FGitLink_Provider& Provider = static_cast<FGitLink_Provider&>(
		ISourceControlModule::Get().GetProvider());
	FGitLink_Subprocess* Subprocess = Provider.Get_Subprocess();

	if (Subprocess == nullptr || !Subprocess->IsValid())
	{
		UE_LOG(LogGitLink, Warning, TEXT("FGitLink_Revision::Get: no subprocess available"));
		return false;
	}

	// Determine which repo root to use — submodule files need the submodule root.
	FString RepoRoot = Provider.Get_PathToRepositoryRoot();
	const FString SubmoduleRoot = Provider.Get_SubmoduleRoot(_Filename);
	if (!SubmoduleRoot.IsEmpty())
	{
		RepoRoot = SubmoduleRoot;
	}

	// Build the repo-relative path.
	FString RelativePath = _Filename;
	FPaths::NormalizeFilename(RelativePath);
	FString NormRoot = RepoRoot;
	FPaths::NormalizeFilename(NormRoot);
	if (!NormRoot.EndsWith(TEXT("/")))
	{ NormRoot += TEXT("/"); }
	if (RelativePath.StartsWith(NormRoot, ESearchCase::IgnoreCase))
	{
		RelativePath.RightChopInline(NormRoot.Len(), EAllowShrinking::No);
	}
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	RelativePath.RemoveFromStart(TEXT("/"));

	// Build temp file path.
	const FString TempDir = FPaths::DiffDir();
	const FString CleanFilename = FPaths::GetCleanFilename(_Filename);
	const FString TempFilename = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(TempDir, FString::Printf(TEXT("temp-%s-%s"), *_ShortCommitId, *CleanFilename)));

	// Reuse if already extracted — but only if the cached file is non-empty. Zero-byte leftovers
	// from an earlier broken extraction would otherwise silently defeat the retry: UE would load
	// the empty file, fail to parse it, and show "Unable to load assets to diff" forever.
	if (FPaths::FileExists(TempFilename))
	{
		const int64 ExistingSize = IFileManager::Get().FileSize(*TempFilename);
		if (ExistingSize > 0)
		{
			InOutFilename = TempFilename;
			return true;
		}
		IFileManager::Get().Delete(*TempFilename);
	}

	// Ensure temp directory exists.
	IFileManager::Get().MakeDirectory(*TempDir, true);

	// Run: git -C <repo-root> cat-file --filters <commit>:<path>
	// Uses RunToFile which pipes binary stdout directly to disk (binary-safe).
	const FString Param = FString::Printf(TEXT("%s:%s"), *_CommitId, *RelativePath);
	TArray<FString> Args;
	Args.Add(TEXT("-C"));
	Args.Add(RepoRoot);
	Args.Add(TEXT("cat-file"));
	Args.Add(TEXT("--filters"));
	Args.Add(Param);

	if (!Subprocess->RunToFile(Args, TempFilename))
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("FGitLink_Revision::Get: git cat-file failed for '%s' at %s"),
			*RelativePath, *_ShortCommitId);
		return false;
	}

	InOutFilename = TempFilename;
	UE_LOG(LogGitLink, Verbose,
		TEXT("FGitLink_Revision::Get: extracted '%s' at %s -> '%s'"),
		*RelativePath, *_ShortCommitId, *TempFilename);
	return true;
}

auto FGitLink_Revision::GetAnnotated(TArray<FAnnotationLine>& /*OutLines*/) const -> bool
{
	return false;
}

auto FGitLink_Revision::GetAnnotated(FString& /*InOutFilename*/) const -> bool
{
	return false;
}
