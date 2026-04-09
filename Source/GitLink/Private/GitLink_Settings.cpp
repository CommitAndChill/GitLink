#include "GitLink/GitLink_Settings.h"

#define LOCTEXT_NAMESPACE "GitLinkSettings"

UGitLink_Settings::UGitLink_Settings() = default;

auto UGitLink_Settings::GetCategoryName() const -> FName
{
	return TEXT("Editor");
}

auto UGitLink_Settings::GetSectionText() const -> FText
{
	return LOCTEXT("SectionText", "GitLink");
}

auto UGitLink_Settings::GetSectionDescription() const -> FText
{
	return LOCTEXT("SectionDescription",
		"Settings for the GitLink source control provider (libgit2-backed).");
}

#undef LOCTEXT_NAMESPACE
