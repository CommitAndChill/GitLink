#include "GitLinkTests_TempRepo.h"

#include "GitLinkTestsLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#include <HAL/FileManager.h>
#include <HAL/PlatformFileManager.h>
#include <Misc/FileHelper.h>
#include <Misc/Guid.h>
#include <Misc/Paths.h>

#if WITH_LIBGIT2
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::tests
{
	namespace
	{
		// Build the root directory for a fresh temp repo: <ProjectSaved>/GitLinkTests/<guid>/.
		auto MakeRepoRoot() -> FString
		{
			const FString Base = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GitLinkTests"));
			const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
			FString Full = FPaths::Combine(Base, Guid);
			FPaths::NormalizeDirectoryName(Full);
			return Full;
		}

#if WITH_LIBGIT2
		// Seed user.name + user.email on a freshly initialised repo so FCommitParams with empty
		// Author fields can fall through to libgit2's default_signature.
		auto Seed_UserConfig(git_repository* InRepo) -> bool
		{
			git_config* Cfg = nullptr;
			if (git_repository_config(&Cfg, InRepo) != 0 || Cfg == nullptr)
			{ return false; }

			const int NameRc  = git_config_set_string(Cfg, "user.name",  "GitLinkTests");
			const int EmailRc = git_config_set_string(Cfg, "user.email", "gitlink.tests@example.invalid");

			git_config_free(Cfg);
			return NameRc == 0 && EmailRc == 0;
		}
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------

	FTempRepo::FTempRepo()
		: _Root(MakeRepoRoot())
	{
#if WITH_LIBGIT2
		// Ensure the parent directory exists. git_repository_init creates the leaf itself.
		IFileManager& FileMgr = IFileManager::Get();
		FileMgr.MakeDirectory(*_Root, /*Tree=*/ true);

		git_repository* Repo = nullptr;
		const int InitRc = git_repository_init(&Repo, TCHAR_TO_UTF8(*_Root), /*is_bare=*/ 0);
		if (InitRc != 0 || Repo == nullptr)
		{
			UE_LOG(LogGitLinkTests, Warning,
				TEXT("FTempRepo: git_repository_init failed (rc=%d) at '%s'"),
				InitRc, *_Root);
			return;
		}

		const bool bConfig = Seed_UserConfig(Repo);
		git_repository_free(Repo);

		if (!bConfig)
		{
			UE_LOG(LogGitLinkTests, Warning,
				TEXT("FTempRepo: failed to seed user.name/user.email at '%s'"),
				*_Root);
			return;
		}

		_bValid = true;
#else
		UE_LOG(LogGitLinkTests, Warning,
			TEXT("FTempRepo: compiled without WITH_LIBGIT2 — tests cannot run."));
#endif
	}

	FTempRepo::~FTempRepo()
	{
		if (_Root.IsEmpty())
		{ return; }

		IFileManager& FileMgr = IFileManager::Get();
		if (FileMgr.DirectoryExists(*_Root))
		{
			const bool bOk = FileMgr.DeleteDirectory(*_Root, /*RequireExists=*/ false, /*Tree=*/ true);
			if (!bOk)
			{
				UE_LOG(LogGitLinkTests, Warning,
					TEXT("FTempRepo: failed to recursively delete '%s' — leaving behind"),
					*_Root);
			}
		}
	}

	auto FTempRepo::Open() -> TUniquePtr<gitlink::FRepository>
	{
		if (!_bValid)
		{ return nullptr; }

		gitlink::FOpenParams Params;
		Params.Path = _Root;
		return gitlink::FRepository::Open(Params);
	}

	auto FTempRepo::Write_File(const FString& InRelPath, const FString& InContents) -> bool
	{
		const FString Abs = FPaths::Combine(_Root, InRelPath);
		// Ensure subdirectories exist.
		const FString Dir = FPaths::GetPath(Abs);
		if (!Dir.IsEmpty())
		{ IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/ true); }

		return FFileHelper::SaveStringToFile(InContents, *Abs);
	}

	auto FTempRepo::Append_File(const FString& InRelPath, const FString& InContents) -> bool
	{
		const FString Abs = FPaths::Combine(_Root, InRelPath);
		FString Existing;
		FFileHelper::LoadFileToString(Existing, *Abs);
		Existing += InContents;
		return FFileHelper::SaveStringToFile(Existing, *Abs);
	}

	auto FTempRepo::Delete_File(const FString& InRelPath) -> bool
	{
		const FString Abs = FPaths::Combine(_Root, InRelPath);
		return IFileManager::Get().Delete(*Abs, /*RequireExists=*/ false);
	}
}
