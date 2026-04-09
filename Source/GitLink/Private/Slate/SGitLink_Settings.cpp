#include "SGitLink_Settings.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_Settings.h"
#include "GitLink_Module.h"
#include "GitLinkLog.h"

#include <Widgets/Layout/SBorder.h>
#include <Widgets/Layout/SGridPanel.h>
#include <Widgets/Text/STextBlock.h>
#include <Widgets/Input/SCheckBox.h>
#include <Widgets/SBoxPanel.h>

#define LOCTEXT_NAMESPACE "SGitLinkSettings"

// --------------------------------------------------------------------------------------------------------------------

namespace
{
	// Single place to reach back into the live provider so every accessor stays in sync.
	auto GetProvider() -> FGitLink_Provider&
	{
		return FGitLinkModule::Get().Get_Provider();
	}
}

// --------------------------------------------------------------------------------------------------------------------

void SGitLink_Settings::Construct(const FArguments& /*InArgs*/)
{
	TSharedRef<SGridPanel> Grid = SNew(SGridPanel).FillColumn(1, 1.f);

	int32 Row = 0;
	auto AddRow = [&Grid, &Row](const FText& Label, TAttribute<FText> Value) -> void
	{
		Grid->AddSlot(0, Row)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 4.f, 8.f, 4.f))
		[
			SNew(STextBlock).Text(Label)
		];

		Grid->AddSlot(1, Row)
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 4.f, 0.f, 4.f))
		[
			SNew(STextBlock)
			.Text(Value)
			.ToolTipText(Value)
		];

		++Row;
	};

	AddRow(LOCTEXT("Backend",         "Backend"),            TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_BackendText));
	AddRow(LOCTEXT("RepositoryRoot",  "Root of the repository"), TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_RepositoryRootText));
	AddRow(LOCTEXT("Branch",          "Current branch"),     TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_BranchText));
	AddRow(LOCTEXT("Remote",          "Remote (origin)"),    TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_RemoteText));
	AddRow(LOCTEXT("UserName",        "User Name"),          TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_UserNameText));
	AddRow(LOCTEXT("UserEmail",       "E-mail"),             TAttribute<FText>::CreateSP(this, &SGitLink_Settings::Get_UserEmailText));

	// LFS row — checkbox bound to the settings object, not a provider method.
	Grid->AddSlot(0, Row)
	.HAlign(HAlign_Right)
	.VAlign(VAlign_Center)
	.Padding(FMargin(0.f, 4.f, 8.f, 4.f))
	[
		SNew(STextBlock).Text(LOCTEXT("UsesLfs", "Uses Git LFS"))
	];

	Grid->AddSlot(1, Row)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	.Padding(FMargin(0.f, 4.f, 0.f, 4.f))
	[
		SNew(SCheckBox)
		.IsChecked(this, &SGitLink_Settings::Get_UsesLfsCheckState)
		.OnCheckStateChanged(this, &SGitLink_Settings::Handle_UsesLfsChanged)
	];

	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(8.f))
		[
			Grid
		]
	];
}

// --------------------------------------------------------------------------------------------------------------------
// Accessors — each reads live from the provider so the panel updates after a reconnect
// without any refresh plumbing.
// --------------------------------------------------------------------------------------------------------------------

auto SGitLink_Settings::Get_BackendText() const -> FText
{
	return LOCTEXT("BackendValue", "libgit2 (in-process)");
}

auto SGitLink_Settings::Get_RepositoryRootText() const -> FText
{
	const FString& Path = GetProvider().Get_PathToRepositoryRoot();
	return Path.IsEmpty() ? LOCTEXT("NotConnected", "(not connected)") : FText::FromString(Path);
}

auto SGitLink_Settings::Get_BranchText() const -> FText
{
	const FString& Branch = GetProvider().Get_BranchName();
	return Branch.IsEmpty() ? LOCTEXT("DetachedOrUnborn", "(detached or unborn HEAD)") : FText::FromString(Branch);
}

auto SGitLink_Settings::Get_RemoteText() const -> FText
{
	const FString& Remote = GetProvider().Get_RemoteUrl();
	return Remote.IsEmpty() ? LOCTEXT("NoRemote", "(no remote configured)") : FText::FromString(Remote);
}

auto SGitLink_Settings::Get_UserNameText() const -> FText
{
	const FString& Name = GetProvider().Get_UserName();
	return Name.IsEmpty() ? LOCTEXT("UserNotSet", "(user.name not configured)") : FText::FromString(Name);
}

auto SGitLink_Settings::Get_UserEmailText() const -> FText
{
	const FString& Email = GetProvider().Get_UserEmail();
	return Email.IsEmpty() ? LOCTEXT("EmailNotSet", "(user.email not configured)") : FText::FromString(Email);
}

auto SGitLink_Settings::Get_UsesLfsCheckState() const -> ECheckBoxState
{
	const UGitLink_Settings* Settings = GetDefault<UGitLink_Settings>();
	const bool bEnabled = Settings != nullptr && Settings->bUseLfsLocking;
	return bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

auto SGitLink_Settings::Handle_UsesLfsChanged(ECheckBoxState InNewState) -> void
{
	UGitLink_Settings* Settings = GetMutableDefault<UGitLink_Settings>();
	if (Settings == nullptr)
	{ return; }

	Settings->bUseLfsLocking = (InNewState == ECheckBoxState::Checked);
	Settings->SaveConfig();

	UE_LOG(LogGitLink, Log, TEXT("Settings widget: bUseLfsLocking -> %s"),
		Settings->bUseLfsLocking ? TEXT("true") : TEXT("false"));
}

#undef LOCTEXT_NAMESPACE
