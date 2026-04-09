#pragma once

#include <CoreMinimal.h>
#include <Widgets/DeclarativeSyntaxSupport.h>
#include <Widgets/SCompoundWidget.h>

class FGitLink_Provider;

// --------------------------------------------------------------------------------------------------------------------
// Slate widget shown in the Revision Control Login dialog when GitLink is the selected
// provider. Populates its rows from the provider's cached metadata:
//
//   - Backend          : always "libgit2 (in-process)"
//   - Repository Root  : FGitLink_Provider::Get_PathToRepositoryRoot
//   - Current Branch   : FGitLink_Provider::Get_BranchName
//   - Remote URL       : FGitLink_Provider::Get_RemoteUrl
//   - User Name        : read from git config user.name via op::Get_DefaultSignature
//   - E-mail           : read from git config user.email
//   - Uses Git LFS     : bound to UGitLink_Settings::bUseLfsLocking
//
// The widget reads lazily on each paint so values reflect whatever Init() last resolved,
// without needing explicit refresh plumbing.
// --------------------------------------------------------------------------------------------------------------------

class SGitLink_Settings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitLink_Settings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	auto Get_BackendText()       const -> FText;
	auto Get_RepositoryRootText() const -> FText;
	auto Get_BranchText()        const -> FText;
	auto Get_RemoteText()        const -> FText;
	auto Get_UserNameText()      const -> FText;
	auto Get_UserEmailText()     const -> FText;

	auto Get_UsesLfsCheckState() const -> ECheckBoxState;
	auto Handle_UsesLfsChanged(ECheckBoxState InNewState) -> void;
};
