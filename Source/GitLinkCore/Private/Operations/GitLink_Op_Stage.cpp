#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// Index manipulation: Stage / Unstage / StageAll / UnstageAll / DiscardChanges.
//
// Stage/StageAll  -> git_index_add_all  (handles new + modified + deleted within the pathspec)
// Unstage/All     -> git_reset_default  (restores the index entry to match HEAD)
// DiscardChanges  -> git_checkout_head with GIT_CHECKOUT_FORCE  (blows away working tree + index)
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
#if WITH_LIBGIT2
	namespace
	{
		// Builds a git_strarray from a TArray<FString>, owning the UTF-8 backing storage for the
		// lifetime of the FPathSpec instance. Safe because TArray never stores elements inline —
		// moving the outer TArray won't invalidate the inner buffers' GetData() pointers.
		struct FPathSpec
		{
			TArray<TArray<ANSICHAR>> Utf8Buffers;
			TArray<char*>            Pointers;
			git_strarray             Array{};

			explicit FPathSpec(const TArray<FString>& InPaths)
			{
				Utf8Buffers.Reserve(InPaths.Num());
				Pointers.Reserve(InPaths.Num());

				for (const FString& Path : InPaths)
				{
					const FTCHARToUTF8 Conv(*Path);
					TArray<ANSICHAR>& Buf = Utf8Buffers.Emplace_GetRef();
					Buf.Append(Conv.Get(), Conv.Length() + 1);  // include null terminator
					Pointers.Add(Buf.GetData());
				}

				Array.strings = Pointers.GetData();
				Array.count   = static_cast<size_t>(Pointers.Num());
			}

			auto Get() -> git_strarray* { return &Array; }
		};

		// Retrieves the commit object that HEAD currently points at. Caller owns the result.
		// Returns nullptr on detached/unborn HEAD or any failure (sets libgit2 error state).
		auto Resolve_HeadCommit(git_repository* InRepo) -> git_object*
		{
			git_object* HeadObj = nullptr;
			const int32 Rc = git_revparse_single(&HeadObj, InRepo, "HEAD");
			if (Rc < 0)
			{
				if (HeadObj) { git_object_free(HeadObj); }
				return nullptr;
			}
			return HeadObj;
		}
	}
#endif  // WITH_LIBGIT2

	// ----------------------------------------------------------------------------------------------------------------
	auto StagePaths(FRepository& InRepo, const TArray<FString>& InPaths) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("StagePaths: repository not open")); }

		if (InPaths.IsEmpty())
		{ return FResult::Ok(); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_index* RawIndex = nullptr;
		if (const int32 Rc = git_repository_index(&RawIndex, InRepo.Get_RawHandle()); Rc < 0)
		{
			if (RawIndex) { git_index_free(RawIndex); }
			return libgit2::MakeFailResult(TEXT("StagePaths: git_repository_index"), Rc);
		}
		libgit2::FIndexPtr Index(RawIndex);

		FPathSpec PathSpec(InPaths);

		if (const int32 Rc = git_index_add_all(
				Index.Get(), PathSpec.Get(), GIT_INDEX_ADD_DEFAULT, /*callback=*/nullptr, /*payload=*/nullptr);
			Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("StagePaths: git_index_add_all"), Rc);
		}

		if (const int32 Rc = git_index_write(Index.Get()); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("StagePaths: git_index_write"), Rc);
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("StagePaths: staged %d path(s)"), InPaths.Num());
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("StagePaths: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto UnstagePaths(FRepository& InRepo, const TArray<FString>& InPaths) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("UnstagePaths: repository not open")); }

		if (InPaths.IsEmpty())
		{ return FResult::Ok(); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_object* HeadObj = Resolve_HeadCommit(InRepo.Get_RawHandle());
		if (HeadObj == nullptr)
		{
			// Unborn HEAD: there's nothing to reset back to, so "unstaging" means removing the
			// entries from the index entirely. Matches `git rm --cached` behaviour on a fresh repo.
			git_index* RawIndex = nullptr;
			if (const int32 Rc = git_repository_index(&RawIndex, InRepo.Get_RawHandle()); Rc < 0)
			{
				if (RawIndex) { git_index_free(RawIndex); }
				return libgit2::MakeFailResult(TEXT("UnstagePaths[unborn]: git_repository_index"), Rc);
			}
			libgit2::FIndexPtr Index(RawIndex);

			for (const FString& Path : InPaths)
			{
				const FTCHARToUTF8 Utf8(*Path);
				if (const int32 Rc = git_index_remove_bypath(Index.Get(), Utf8.Get()); Rc < 0 && Rc != GIT_ENOTFOUND)
				{
					return libgit2::MakeFailResult(TEXT("UnstagePaths[unborn]: git_index_remove_bypath"), Rc);
				}
			}

			if (const int32 Rc = git_index_write(Index.Get()); Rc < 0)
			{
				return libgit2::MakeFailResult(TEXT("UnstagePaths[unborn]: git_index_write"), Rc);
			}
			return FResult::Ok();
		}

		FPathSpec PathSpec(InPaths);

		const int32 Rc = git_reset_default(InRepo.Get_RawHandle(), HeadObj, PathSpec.Get());
		git_object_free(HeadObj);

		if (Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("UnstagePaths: git_reset_default"), Rc);
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("UnstagePaths: unstaged %d path(s)"), InPaths.Num());
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("UnstagePaths: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto StageAll(FRepository& InRepo) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("StageAll: repository not open")); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_index* RawIndex = nullptr;
		if (const int32 Rc = git_repository_index(&RawIndex, InRepo.Get_RawHandle()); Rc < 0)
		{
			if (RawIndex) { git_index_free(RawIndex); }
			return libgit2::MakeFailResult(TEXT("StageAll: git_repository_index"), Rc);
		}
		libgit2::FIndexPtr Index(RawIndex);

		// Null pathspec = match everything.
		if (const int32 Rc = git_index_add_all(
				Index.Get(), /*pathspec=*/nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
			Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("StageAll: git_index_add_all"), Rc);
		}

		if (const int32 Rc = git_index_write(Index.Get()); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("StageAll: git_index_write"), Rc);
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("StageAll: staged everything"));
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("StageAll: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto UnstageAll(FRepository& InRepo) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("UnstageAll: repository not open")); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_object* HeadObj = Resolve_HeadCommit(InRepo.Get_RawHandle());
		if (HeadObj == nullptr)
		{
			// Unborn HEAD: clear the index entirely.
			git_index* RawIndex = nullptr;
			if (const int32 Rc = git_repository_index(&RawIndex, InRepo.Get_RawHandle()); Rc < 0)
			{
				if (RawIndex) { git_index_free(RawIndex); }
				return libgit2::MakeFailResult(TEXT("UnstageAll[unborn]: git_repository_index"), Rc);
			}
			libgit2::FIndexPtr Index(RawIndex);

			if (const int32 Rc = git_index_clear(Index.Get()); Rc < 0)
			{
				return libgit2::MakeFailResult(TEXT("UnstageAll[unborn]: git_index_clear"), Rc);
			}
			if (const int32 Rc = git_index_write(Index.Get()); Rc < 0)
			{
				return libgit2::MakeFailResult(TEXT("UnstageAll[unborn]: git_index_write"), Rc);
			}
			return FResult::Ok();
		}

		const int32 Rc = git_reset_default(InRepo.Get_RawHandle(), HeadObj, /*pathspec=*/nullptr);
		git_object_free(HeadObj);

		if (Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("UnstageAll: git_reset_default"), Rc);
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("UnstageAll: unstaged everything"));
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("UnstageAll: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto DiscardChanges(FRepository& InRepo, const TArray<FString>& InPaths) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("DiscardChanges: repository not open")); }

		if (InPaths.IsEmpty())
		{ return FResult::Ok(); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		FPathSpec PathSpec(InPaths);

		git_checkout_options Opts;
		if (const int32 Rc = git_checkout_options_init(&Opts, GIT_CHECKOUT_OPTIONS_VERSION); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("DiscardChanges: git_checkout_options_init"), Rc);
		}
		Opts.checkout_strategy = GIT_CHECKOUT_FORCE;
		Opts.paths             = *PathSpec.Get();

		if (const int32 Rc = git_checkout_head(InRepo.Get_RawHandle(), &Opts); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("DiscardChanges: git_checkout_head"), Rc);
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("DiscardChanges: reverted %d path(s)"), InPaths.Num());
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("DiscardChanges: compiled without libgit2"));
#endif
	}
}
