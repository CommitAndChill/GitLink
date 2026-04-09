#include "GitLink_ChangelistState.h"

#define LOCTEXT_NAMESPACE "GitLinkChangelistState"

auto FGitLink_ChangelistState::GetIconName() const -> FName
{
	return NAME_None;
}

auto FGitLink_ChangelistState::GetSmallIconName() const -> FName
{
	return NAME_None;
}

auto FGitLink_ChangelistState::GetDisplayText() const -> FText
{
	return FText::FromString(_Changelist.Get_Name());
}

auto FGitLink_ChangelistState::GetDescriptionText() const -> FText
{
	return FText::FromString(_Description);
}

auto FGitLink_ChangelistState::GetDisplayTooltip() const -> FText
{
	return LOCTEXT("Tooltip", "GitLink changelist");
}

auto FGitLink_ChangelistState::GetChangelist() const -> FSourceControlChangelistRef
{
	return MakeShared<FGitLink_Changelist, ESPMode::ThreadSafe>(_Changelist);
}

#undef LOCTEXT_NAMESPACE
