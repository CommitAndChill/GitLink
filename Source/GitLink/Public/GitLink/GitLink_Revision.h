#pragma once

#include <CoreMinimal.h>
#include <ISourceControlRevision.h>
#include <Misc/DateTime.h>
#include <Runtime/Launch/Resources/Version.h>

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Revision — thin ISourceControlRevision implementation backed by a commit hash and a
// per-file path. Held by FGitLink_FileState::_History as TSharedRefs so the editor's history
// view and diff-against-depot feature can enumerate per-file revisions.
//
// The expensive fetch methods (Get, GetAnnotated) are stubs in v1; they'll be wired up to
// git_blob_lookup + git_blob_rawcontent once the editor features that depend on them are in play.
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_Revision : public ISourceControlRevision
{
public:
	// ISourceControlRevision -----------------------------------------------------------------------------------------
#if ENGINE_MAJOR_VERSION >= 5
	auto Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const -> bool override;
#else
	auto Get(FString& InOutFilename) const -> bool override;
#endif

	auto GetAnnotated(TArray<FAnnotationLine>& OutLines) const -> bool override;
	auto GetAnnotated(FString& InOutFilename)            const -> bool override;

	auto GetFilename         () const -> const FString&   override { return _Filename;    }
	auto GetRevisionNumber   () const -> int32            override { return _RevisionNumber; }
	auto GetRevision         () const -> const FString&   override { return _ShortCommitId; }
	auto GetDescription      () const -> const FString&   override { return _Description; }
	auto GetUserName         () const -> const FString&   override { return _UserName;    }
	auto GetClientSpec       () const -> const FString&   override { return _ClientSpec;  }
	auto GetAction           () const -> const FString&   override { return _Action;      }
	auto GetBranchSource     () const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override { return nullptr; }
	auto GetDate             () const -> const FDateTime& override { return _Date;        }
	auto GetCheckInIdentifier() const -> int32            override { return _CheckInIdentifier; }
	auto GetFileSize         () const -> int32            override { return _FileSize;    }

	// Public fields so Cmd_UpdateStatus / Cmd_History can populate directly.
	FString    _Filename;
	FString    _CommitId;           // full 40-char hex
	FString    _ShortCommitId;      // 7-char
	FString    _Description;
	FString    _UserName;
	FString    _ClientSpec;
	FString    _Action;
	FDateTime  _Date               = FDateTime::MinValue();
	int32      _RevisionNumber     = 0;
	int32      _CheckInIdentifier  = 0;
	int32      _FileSize           = 0;
};

using FGitLink_RevisionRef = TSharedRef<FGitLink_Revision, ESPMode::ThreadSafe>;
using FGitLink_History     = TArray<FGitLink_RevisionRef>;
