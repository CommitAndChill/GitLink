#pragma once

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>
#include <Runtime/Launch/Resources/Version.h>

struct FToolMenuSection;
class FMenuBuilder;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Menu — toolbar buttons (Push, Pull, Revert All, Refresh) in the SCC dropdown
// at the bottom-right of the editor. Registered from FGitLink_Provider::CheckRepositoryStatus,
// unregistered from Close().
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_Menu
{
public:
	auto Register() -> void;
	auto Unregister() -> void;

private:
	auto Push_Clicked() -> void;
	auto Sync_Clicked() -> void;
	auto RevertAll_Clicked() -> void;
	auto Refresh_Clicked() -> void;

	auto Has_RemoteUrl() const -> bool;

#if ENGINE_MAJOR_VERSION >= 5
	auto AddMenuExtension(FToolMenuSection& InSection) -> void;
#else
	auto AddMenuExtension(FMenuBuilder& InBuilder) -> void;
	auto OnExtendLevelEditorViewMenu(const TSharedRef<class FUICommandList> InCommandList)
		-> TSharedRef<class FExtender>;
	FDelegateHandle _ViewMenuExtenderHandle;
#endif

	auto OnOperationComplete(const FSourceControlOperationRef& InOp, ECommandResult::Type InResult) -> void;

	static auto Display_InProgressNotification(const FText& InText) -> void;
	static auto Remove_InProgressNotification() -> void;
	static auto Display_SuccessNotification(const FName& InOpName) -> void;
	static auto Display_FailureNotification(const FName& InOpName) -> void;

	static TWeakPtr<class SNotificationItem> _OperationInProgressNotification;
};
