#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// Status operation: walks libgit2's git_status_list and splits each change into the Staged /
// Unstaged / Conflicted buckets that Unreal's source control expects.
//
// A single file may appear in BOTH Staged and Unstaged if it has staged changes and then further
// working-tree modifications — that matches how `git status` reports it and the existing
// GitSourceControl plugin.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
#if WITH_LIBGIT2
	namespace
	{
		auto MapDeltaStatus(git_delta_t InDelta) -> EFileStatus
		{
			switch (InDelta)
			{
				case GIT_DELTA_ADDED:      return EFileStatus::Added;
				case GIT_DELTA_DELETED:    return EFileStatus::Deleted;
				case GIT_DELTA_MODIFIED:   return EFileStatus::Modified;
				case GIT_DELTA_RENAMED:    return EFileStatus::Renamed;
				case GIT_DELTA_COPIED:     return EFileStatus::Added;
				case GIT_DELTA_TYPECHANGE: return EFileStatus::TypeChange;
				case GIT_DELTA_UNTRACKED:  return EFileStatus::Untracked;
				case GIT_DELTA_IGNORED:    return EFileStatus::Ignored;
				case GIT_DELTA_CONFLICTED: return EFileStatus::Conflicted;
				default:                   return EFileStatus::Unmodified;
			}
		}

		auto MakeChange(const git_diff_delta& InDelta) -> FFileChange
		{
			FFileChange Change;
			Change.Status = MapDeltaStatus(InDelta.status);

			if (InDelta.new_file.path != nullptr)
			{
				Change.Path = FString(UTF8_TO_TCHAR(InDelta.new_file.path));
			}

			// Populate OldPath on rename/copy, but only when it actually differs.
			if (InDelta.old_file.path != nullptr)
			{
				const FString OldPath = FString(UTF8_TO_TCHAR(InDelta.old_file.path));
				if (!OldPath.Equals(Change.Path))
				{
					Change.OldPath = OldPath;
				}
			}

			Change.bIsSubmodule =
				((InDelta.new_file.mode & GIT_FILEMODE_COMMIT) == GIT_FILEMODE_COMMIT) ||
				((InDelta.old_file.mode & GIT_FILEMODE_COMMIT) == GIT_FILEMODE_COMMIT);

			return Change;
		}
	}
#endif  // WITH_LIBGIT2

	auto ComputeStatus(FRepository& InRepo) -> FStatus
	{
		FStatus Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{
			return Out;
		}

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();
		if (Raw == nullptr)
		{
			return Out;
		}

		git_status_options Opts;
		const int32 OptsInit = git_status_options_init(&Opts, GIT_STATUS_OPTIONS_VERSION);
		if (OptsInit < 0)
		{
			UE_LOG(LogGitLinkCore, Warning,
				TEXT("ComputeStatus: git_status_options_init failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}

		Opts.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
		Opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
		           | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
		           | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX
		           | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;

		git_status_list* RawList = nullptr;
		const int32 ListRc = git_status_list_new(&RawList, Raw, &Opts);
		if (ListRc < 0 || RawList == nullptr)
		{
			UE_LOG(LogGitLinkCore, Warning,
				TEXT("ComputeStatus: git_status_list_new failed (rc=%d): %s"),
				ListRc, *libgit2::Get_LastErrorMessage());
			if (RawList) { git_status_list_free(RawList); }
			return Out;
		}

		libgit2::FStatusListPtr List(RawList);

		const size_t EntryCount = git_status_list_entrycount(List.Get());
		for (size_t EntryIdx = 0; EntryIdx < EntryCount; ++EntryIdx)
		{
			const git_status_entry* Entry = git_status_byindex(List.Get(), EntryIdx);
			if (Entry == nullptr)
			{ continue; }

			// Conflicted trumps everything — report once, in the Conflicted bucket.
			if ((Entry->status & GIT_STATUS_CONFLICTED) != 0)
			{
				const git_diff_delta* Delta =
					Entry->index_to_workdir != nullptr ? Entry->index_to_workdir
				                                       : Entry->head_to_index;

				FFileChange Change;
				Change.Status = EFileStatus::Conflicted;
				if (Delta != nullptr && Delta->new_file.path != nullptr)
				{
					Change.Path = FString(UTF8_TO_TCHAR(Delta->new_file.path));
				}
				Out.Conflicted.Add(MoveTemp(Change));
				continue;
			}

			// Staged side: any INDEX_* bit implies a head->index delta.
			constexpr uint32 StagedMask =
				GIT_STATUS_INDEX_NEW        |
				GIT_STATUS_INDEX_MODIFIED   |
				GIT_STATUS_INDEX_DELETED    |
				GIT_STATUS_INDEX_RENAMED    |
				GIT_STATUS_INDEX_TYPECHANGE;

			if ((Entry->status & StagedMask) != 0 && Entry->head_to_index != nullptr)
			{
				Out.Staged.Add(MakeChange(*Entry->head_to_index));
			}

			// Unstaged side: any WT_* bit implies an index->workdir delta.
			constexpr uint32 UnstagedMask =
				GIT_STATUS_WT_NEW        |
				GIT_STATUS_WT_MODIFIED   |
				GIT_STATUS_WT_DELETED    |
				GIT_STATUS_WT_TYPECHANGE |
				GIT_STATUS_WT_RENAMED    |
				GIT_STATUS_WT_UNREADABLE;

			if ((Entry->status & UnstagedMask) != 0 && Entry->index_to_workdir != nullptr)
			{
				Out.Unstaged.Add(MakeChange(*Entry->index_to_workdir));
			}
		}

		UE_LOG(LogGitLinkCore, Verbose,
			TEXT("ComputeStatus: staged=%d unstaged=%d conflicted=%d"),
			Out.Staged.Num(), Out.Unstaged.Num(), Out.Conflicted.Num());
#endif  // WITH_LIBGIT2

		return Out;
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto Enumerate_TrackedFiles(FRepository& InRepo) -> TArray<FString>
	{
		TArray<FString> Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return Out; }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_index* RawIndex = nullptr;
		if (git_repository_index(&RawIndex, InRepo.Get_RawHandle()) < 0 || RawIndex == nullptr)
		{
			if (RawIndex) { git_index_free(RawIndex); }
			UE_LOG(LogGitLinkCore, Warning, TEXT("Enumerate_TrackedFiles: git_repository_index failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}
		libgit2::FIndexPtr Index(RawIndex);

		const size_t EntryCount = git_index_entrycount(Index.Get());
		Out.Reserve(static_cast<int32>(EntryCount));

		for (size_t EntryIdx = 0; EntryIdx < EntryCount; ++EntryIdx)
		{
			const git_index_entry* Entry = git_index_get_byindex(Index.Get(), EntryIdx);
			if (Entry == nullptr || Entry->path == nullptr)
			{ continue; }

			Out.Add(FString(UTF8_TO_TCHAR(Entry->path)));
		}

		UE_LOG(LogGitLinkCore, Verbose,
			TEXT("Enumerate_TrackedFiles: %d entries"), Out.Num());
#endif  // WITH_LIBGIT2

		return Out;
	}
}
