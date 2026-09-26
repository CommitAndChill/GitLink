#include "GitLinkTests_TempRemote.h"

#include "GitLinkTestsLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#include <HAL/FileManager.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Guid.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::tests
{
	namespace
	{
		constexpr double GitTimeoutSec = 60.0;

		auto MakeBase() -> FString
		{
			FString Base = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("GitLinkTests"),
				FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens)));
			FPaths::NormalizeDirectoryName(Base);
			return Base;
		}
	}

	FTempRemote::FTempRemote()
		: _Base(MakeBase())
		, _BareRoot(FPaths::Combine(_Base, TEXT("remote.git")))
		, _Git(MakeUnique<FGitLink_Subprocess>(TEXT("git"), _Base))
	{
		IFileManager::Get().MakeDirectory(*_Base, /*Tree=*/ true);

		if (!Git(_Base, { TEXT("init"), TEXT("--bare"), TEXT("--initial-branch=main"), _BareRoot }).IsSuccess())
		{
			UE_LOG(LogGitLinkTests, Warning, TEXT("FTempRemote: 'git init --bare' failed at '%s'"), *_BareRoot);
			return;
		}

		// Seed one commit on main so clones have history and an upstream to track.
		const FString Seed = FPaths::Combine(_Base, TEXT("seed"));
		const bool bSeeded =
			Git(_Base, { TEXT("init"), TEXT("--initial-branch=main"), Seed }).IsSuccess()
			&& Git(Seed, { TEXT("config"), TEXT("user.name"),  TEXT("GitLinkTests") }).IsSuccess()
			&& Git(Seed, { TEXT("config"), TEXT("user.email"), TEXT("gitlink.tests@example.invalid") }).IsSuccess()
			&& Commit_File(Seed, TEXT("readme.md"), TEXT("seed\n"), TEXT("seed"))
			&& Git(Seed, { TEXT("remote"), TEXT("add"), TEXT("origin"), _BareRoot }).IsSuccess()
			&& Git(Seed, { TEXT("push"), TEXT("-u"), TEXT("origin"), TEXT("main") }).IsSuccess();

		if (!bSeeded)
		{
			UE_LOG(LogGitLinkTests, Warning, TEXT("FTempRemote: seeding the bare remote failed under '%s'"), *_Base);
			return;
		}

		_bValid = true;
	}

	FTempRemote::~FTempRemote()
	{
		_Git.Reset();

		IFileManager& FileMgr = IFileManager::Get();
		if (!_Base.IsEmpty() && FileMgr.DirectoryExists(*_Base))
		{
			if (!FileMgr.DeleteDirectory(*_Base, /*RequireExists=*/ false, /*Tree=*/ true))
			{
				UE_LOG(LogGitLinkTests, Warning, TEXT("FTempRemote: failed to delete '%s' — leaving behind"), *_Base);
			}
		}
	}

	auto FTempRemote::Is_GitAvailable(FAutomationTestBase& InTest) -> bool
	{
		FGitLink_Subprocess Probe(TEXT("git"), FString());
		if (!Probe.Run_Bounded({ TEXT("--version") }, FString(), GitTimeoutSec).IsSuccess())
		{
			InTest.AddWarning(TEXT("git not on PATH — skipping remote-operation test"));
			return false;
		}
		return true;
	}

	auto FTempRemote::Clone(const FString& InName) -> FString
	{
		const FString Root = FPaths::Combine(_Base, InName);
		const bool bOk =
			Git(_Base, { TEXT("clone"), _BareRoot, Root }).IsSuccess()
			&& Git(Root, { TEXT("config"), TEXT("user.name"),  InName }).IsSuccess()
			&& Git(Root, { TEXT("config"), TEXT("user.email"), InName + TEXT("@example.invalid") }).IsSuccess();

		return bOk ? Root : FString();
	}

	auto FTempRemote::Open(const FString& InRoot) -> TUniquePtr<gitlink::FRepository>
	{
		gitlink::FOpenParams Params;
		Params.Path = InRoot;
		return gitlink::FRepository::Open(Params);
	}

	auto FTempRemote::Git(const FString& InCwd, const TArray<FString>& InArgs) -> FGitLink_SubprocessResult
	{
		const FGitLink_SubprocessResult Result = _Git->Run_Bounded(InArgs, InCwd, GitTimeoutSec);
		if (!Result.IsSuccess())
		{
			UE_LOG(LogGitLinkTests, Verbose, TEXT("FTempRemote: git %s (in '%s') failed: %s"),
				*FString::Join(InArgs, TEXT(" ")), *InCwd, *Result.Get_CombinedError());
		}
		return Result;
	}

	auto FTempRemote::Commit_File(const FString& InRepoRoot, const FString& InRelPath,
		const FString& InContents, const FString& InMessage) -> bool
	{
		const FString Abs = FPaths::Combine(InRepoRoot, InRelPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Abs), /*Tree=*/ true);

		return FFileHelper::SaveStringToFile(InContents, *Abs)
			&& Git(InRepoRoot, { TEXT("add"), TEXT("--"), InRelPath }).IsSuccess()
			&& Git(InRepoRoot, { TEXT("-c"), TEXT("commit.gpgsign=false"),
				TEXT("commit"), TEXT("-m"), InMessage }).IsSuccess();
	}

	auto FTempRemote::Get_Sha(const FString& InRepoRoot, const FString& InRev) -> FString
	{
		const FGitLink_SubprocessResult Result = Git(InRepoRoot, { TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), InRev });
		return Result.IsSuccess() ? Result.StdOut.TrimStartAndEnd() : FString();
	}

	auto FTempRemote::Read_File(const FString& InRepoRoot, const FString& InRelPath) -> FString
	{
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(InRepoRoot, InRelPath));
		// A checkout honours the developer's core.autocrlf, so normalise before comparing content.
		return Contents.Replace(TEXT("\r\n"), TEXT("\n"));
	}
}
