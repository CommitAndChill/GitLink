#pragma once

#include "GitLink/GitLink_Changelist.h"

#include <CoreMinimal.h>
#include <ISourceControlChangelistState.h>
#include <ISourceControlState.h>
#include <Runtime/Launch/Resources/Version.h>

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_ChangelistState — tracks files and description for a given FGitLink_Changelist.
// Returned from FGitLink_Provider::GetState(changelists).
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_ChangelistState : public ISourceControlChangelistState
{
public:
	explicit FGitLink_ChangelistState(const FGitLink_Changelist& InChangelist, FString InDescription = FString())
		: _Changelist(InChangelist)
		, _Description(MoveTemp(InDescription))
	{
	}

	explicit FGitLink_ChangelistState(FGitLink_Changelist&& InChangelist, FString&& InDescription)
		: _Changelist(MoveTemp(InChangelist))
		, _Description(MoveTemp(InDescription))
	{
	}

	// ISourceControlChangelistState ----------------------------------------------------------------------------------
	auto GetIconName()       const -> FName         override;
	auto GetSmallIconName()  const -> FName         override;
	auto GetDisplayText()    const -> FText         override;
	auto GetDescriptionText() const -> FText        override;
	auto GetDisplayTooltip() const -> FText         override;
	auto GetTimeStamp()      const -> const FDateTime& override { return _TimeStamp; }

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
	auto GetFilesStates()        const -> const TArray<FSourceControlStateRef> override { return _Files; }
	auto GetFilesStatesNum()     const -> int32 override { return _Files.Num(); }
	auto GetShelvedFilesStates() const -> const TArray<FSourceControlStateRef> override { return _ShelvedFiles; }
	auto GetShelvedFilesStatesNum() const -> int32 override { return _ShelvedFiles.Num(); }
#else
	auto GetFilesStates()        const -> const TArray<FSourceControlStateRef>& override { return _Files; }
	auto GetShelvedFilesStates() const -> const TArray<FSourceControlStateRef>& override { return _ShelvedFiles; }
#endif

	auto GetChangelist() const -> FSourceControlChangelistRef override;

	// Public fields — populated by Cmd_UpdateChangelistsStatus
	FGitLink_Changelist             _Changelist;
	FString                         _Description;
	TArray<FSourceControlStateRef>  _Files;
	TArray<FSourceControlStateRef>  _ShelvedFiles;
	FDateTime                       _TimeStamp = FDateTime(0);
};

using FGitLink_ChangelistStateRef = TSharedRef<FGitLink_ChangelistState, ESPMode::ThreadSafe>;
