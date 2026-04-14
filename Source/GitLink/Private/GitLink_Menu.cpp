#include "GitLink_Menu.h"

#include "GitLink_Module.h"
#include "GitLink/GitLink_Provider.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <ISourceControlModule.h>
#include <SourceControlOperations.h>
#include <Widgets/Notifications/SNotificationList.h>
#include <Framework/Notifications/NotificationManager.h>
#include <Misc/MessageDialog.h>
#include <Async/Async.h>

#if ENGINE_MAJOR_VERSION >= 5
#include <ToolMenus.h>
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include <Styling/AppStyle.h>
#else
#include <EditorStyleSet.h>
#endif

#if ENGINE_MAJOR_VERSION < 5
#include <LevelEditor.h>
#include <Framework/MultiBox/MultiBoxBuilder.h>
#endif

// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "GitLinkMenu"

TWeakPtr<SNotificationItem> FGitLink_Menu::_OperationInProgressNotification;

// --------------------------------------------------------------------------------------------------------------------
// Registration
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_Menu::Register() -> void
{
#if ENGINE_MAJOR_VERSION >= 5
	FToolMenuOwnerScoped OwnerScope(TEXT("GitLinkMenu"));
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		UToolMenu* SourceControlMenu = ToolMenus->ExtendMenu(TEXT("StatusBar.ToolBar.SourceControl"));
		FToolMenuSection& Section = SourceControlMenu->AddSection(
			TEXT("GitLinkActions"),
			LOCTEXT("GitLinkMenuHeading", "Git"),
			FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
		AddMenuExtension(Section);
	}
#else
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditorModule)
	{
		FLevelEditorModule::FLevelEditorMenuExtender Extender =
			FLevelEditorModule::FLevelEditorMenuExtender::CreateRaw(
				this, &FGitLink_Menu::OnExtendLevelEditorViewMenu);
		auto& MenuExtenders = LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders();
		MenuExtenders.Add(Extender);
		_ViewMenuExtenderHandle = MenuExtenders.Last().GetHandle();
	}
#endif

	UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: registered toolbar buttons"));
}

auto FGitLink_Menu::Unregister() -> void
{
#if ENGINE_MAJOR_VERSION >= 5
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->UnregisterOwnerByName(TEXT("GitLinkMenu"));
	}
#else
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditorModule)
	{
		LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll(
			[this](const FLevelEditorModule::FLevelEditorMenuExtender& Extender)
			{
				return Extender.GetHandle() == _ViewMenuExtenderHandle;
			});
	}
#endif
}

// --------------------------------------------------------------------------------------------------------------------
// Menu entries
// --------------------------------------------------------------------------------------------------------------------

#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_Menu::AddMenuExtension(FToolMenuSection& Builder) -> void
#else
auto FGitLink_Menu::AddMenuExtension(FMenuBuilder& Builder) -> void
#endif
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	const FName StyleSetName = FAppStyle::GetAppStyleSetName();
#else
	const FName StyleSetName = FEditorStyle::GetStyleSetName();
#endif

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION >= 5
		"GitLinkPush",
#endif
		LOCTEXT("Push",          "Push"),
		LOCTEXT("PushTooltip",   "Push all pending local commits to the remote."),
		FSlateIcon(StyleSetName, "SourceControl.Actions.Submit"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitLink_Menu::Push_Clicked),
			FCanExecuteAction::CreateRaw(this, &FGitLink_Menu::Has_RemoteUrl)
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION >= 5
		"GitLinkSync",
#endif
		LOCTEXT("Pull",          "Pull"),
		LOCTEXT("PullTooltip",   "Pull latest changes from the remote (fast-forward)."),
		FSlateIcon(StyleSetName, "SourceControl.Actions.Sync"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitLink_Menu::Sync_Clicked),
			FCanExecuteAction::CreateRaw(this, &FGitLink_Menu::Has_RemoteUrl)
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION >= 5
		"GitLinkRevert",
#endif
		LOCTEXT("RevertAll",        "Revert All"),
		LOCTEXT("RevertAllTooltip", "Revert all modifications in the working tree."),
		FSlateIcon(StyleSetName, "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitLink_Menu::RevertAll_Clicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION >= 5
		"GitLinkRefresh",
#endif
		LOCTEXT("Refresh",        "Refresh"),
		LOCTEXT("RefreshTooltip", "Refresh the revision control status of all files."),
		FSlateIcon(StyleSetName, "SourceControl.Actions.Refresh"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitLink_Menu::Refresh_Clicked),
			FCanExecuteAction()
		)
	);
}

#if ENGINE_MAJOR_VERSION < 5
auto FGitLink_Menu::OnExtendLevelEditorViewMenu(
	const TSharedRef<FUICommandList> /*InCommandList*/) -> TSharedRef<FExtender>
{
	TSharedRef<FExtender> Extender(new FExtender());
	Extender->AddMenuExtension(
		"SourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FGitLink_Menu::AddMenuExtension));
	return Extender;
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// Button handlers
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_Menu::Has_RemoteUrl() const -> bool
{
	const FGitLink_Provider& Provider = FGitLinkModule::Get().Get_Provider();
	return !Provider.Get_RemoteUrl().IsEmpty();
}

auto FGitLink_Menu::Push_Clicked() -> void
{
	if (_OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	Display_InProgressNotification(LOCTEXT("PushInProgress", "Pushing to remote..."));

	UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: Push clicked"));

	// Run git push via subprocess on a background thread — libgit2 push lacks credential wiring.
	Async(EAsyncExecution::TaskGraph, [this]()
	{
		FGitLink_Subprocess* Subprocess = FGitLinkModule::Get().Get_Provider().Get_Subprocess();
		const bool bOk = Subprocess != nullptr && Subprocess->IsValid()
			&& Subprocess->Run({ TEXT("push") }).IsSuccess();

		AsyncTask(ENamedThreads::GameThread, [this, bOk]()
		{
			Remove_InProgressNotification();
			if (bOk)
			{
				UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: Push succeeded"));
				Display_SuccessNotification(TEXT("Push"));
			}
			else
			{
				UE_LOG(LogGitLink, Warning, TEXT("GitLink_Menu: Push failed"));
				Display_FailureNotification(TEXT("Push"));
			}
		});
	});
}

auto FGitLink_Menu::Sync_Clicked() -> void
{
	if (_OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: Pull clicked"));

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	TSharedRef<FSync, ESPMode::ThreadSafe> SyncOp = ISourceControlOperation::Create<FSync>();

#if ENGINE_MAJOR_VERSION >= 5
	const ECommandResult::Type Result = Provider.Execute(
		SyncOp, FSourceControlChangelistPtr(), TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));
#else
	const ECommandResult::Type Result = Provider.Execute(
		SyncOp, TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));
#endif

	if (Result == ECommandResult::Succeeded)
	{ Display_InProgressNotification(SyncOp->GetInProgressString()); }
	else
	{ Display_FailureNotification(SyncOp->GetName()); }
}

auto FGitLink_Menu::RevertAll_Clicked() -> void
{
	if (_OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	const FText DialogText(LOCTEXT("RevertAll_Ask", "Revert all modifications in the working tree?"));
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
	if (Choice != EAppReturnType::Ok)
	{ return; }

	UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: Revert All clicked"));

	// Query status first to discover all modified files, then revert them.
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	const TArray<FString> ContentPaths {
		FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
		FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
	};

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();

#if ENGINE_MAJOR_VERSION >= 5
	Provider.Execute(
		UpdateOp, FSourceControlChangelistPtr(), ContentPaths,
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateLambda(
			[this](const FSourceControlOperationRef& /*InOp*/, ECommandResult::Type InResult)
			{
				if (InResult != ECommandResult::Succeeded)
				{
					Display_FailureNotification(TEXT("Revert"));
					return;
				}

				// Gather all modified files from the cache.
				ISourceControlProvider& Prov = ISourceControlModule::Get().GetProvider();
				TArray<FSourceControlStateRef> States = Prov.GetCachedStateByPredicate(
					[](const FSourceControlStateRef& InState) { return InState->IsModified(); });

				if (States.IsEmpty())
				{
					FNotificationInfo Info(LOCTEXT("RevertAll_NoFiles", "No modified files to revert."));
					FSlateNotificationManager::Get().AddNotification(Info);
					return;
				}

				TArray<FString> FilesToRevert;
				FilesToRevert.Reserve(States.Num());
				for (const FSourceControlStateRef& State : States)
				{ FilesToRevert.Add(State->GetFilename()); }

				TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOp = ISourceControlOperation::Create<FRevert>();
				Prov.Execute(
					RevertOp, FSourceControlChangelistPtr(), FilesToRevert,
					EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));

				Display_InProgressNotification(LOCTEXT("RevertInProgress", "Reverting files..."));
			}));
#else
	Provider.Execute(
		UpdateOp, ContentPaths,
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateLambda(
			[this](const FSourceControlOperationRef& /*InOp*/, ECommandResult::Type InResult)
			{
				if (InResult != ECommandResult::Succeeded)
				{
					Display_FailureNotification(TEXT("Revert"));
					return;
				}

				ISourceControlProvider& Prov = ISourceControlModule::Get().GetProvider();
				TArray<FSourceControlStateRef> States = Prov.GetCachedStateByPredicate(
					[](const FSourceControlStateRef& InState) { return InState->IsModified(); });

				if (States.IsEmpty())
				{
					FNotificationInfo Info(LOCTEXT("RevertAll_NoFiles", "No modified files to revert."));
					FSlateNotificationManager::Get().AddNotification(Info);
					return;
				}

				TArray<FString> FilesToRevert;
				FilesToRevert.Reserve(States.Num());
				for (const FSourceControlStateRef& State : States)
				{ FilesToRevert.Add(State->GetFilename()); }

				TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOp = ISourceControlOperation::Create<FRevert>();
				Prov.Execute(
					RevertOp, FilesToRevert,
					EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));

				Display_InProgressNotification(LOCTEXT("RevertInProgress", "Reverting files..."));
			}));
#endif

	Display_InProgressNotification(LOCTEXT("RevertStatusCheck", "Checking for modified files..."));
}

auto FGitLink_Menu::Refresh_Clicked() -> void
{
	if (_OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("InProgress", "Revision control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	UE_LOG(LogGitLink, Log, TEXT("GitLink_Menu: Refresh clicked"));

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOp = ISourceControlOperation::Create<FUpdateStatus>();

#if ENGINE_MAJOR_VERSION >= 5
	Provider.Execute(
		StatusOp, FSourceControlChangelistPtr(), TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));
#else
	Provider.Execute(
		StatusOp, TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(this, &FGitLink_Menu::OnOperationComplete));
#endif

	Display_InProgressNotification(LOCTEXT("RefreshInProgress", "Refreshing status..."));
}

// --------------------------------------------------------------------------------------------------------------------
// Completion callback
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_Menu::OnOperationComplete(
	const FSourceControlOperationRef& InOp, ECommandResult::Type InResult) -> void
{
	Remove_InProgressNotification();

	if (InResult == ECommandResult::Succeeded)
	{ Display_SuccessNotification(InOp->GetName()); }
	else
	{ Display_FailureNotification(InOp->GetName()); }
}

// --------------------------------------------------------------------------------------------------------------------
// Notifications
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_Menu::Display_InProgressNotification(const FText& InText) -> void
{
	if (!_OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InText);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		_OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (_OperationInProgressNotification.IsValid())
		{
			_OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

auto FGitLink_Menu::Remove_InProgressNotification() -> void
{
	if (_OperationInProgressNotification.IsValid())
	{
		_OperationInProgressNotification.Pin()->ExpireAndFadeout();
		_OperationInProgressNotification.Reset();
	}
}

auto FGitLink_Menu::Display_SuccessNotification(const FName& InOpName) -> void
{
	const FText Text = FText::Format(
		LOCTEXT("Success", "{0} operation was successful!"),
		FText::FromName(InOpName));

	FNotificationInfo Info(Text);
	Info.bUseSuccessFailIcons = true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
#else
	Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
#endif
	FSlateNotificationManager::Get().AddNotification(Info);
}

auto FGitLink_Menu::Display_FailureNotification(const FName& InOpName) -> void
{
	const FText Text = FText::Format(
		LOCTEXT("Failure", "Error: {0} operation failed!"),
		FText::FromName(InOpName));

	FNotificationInfo Info(Text);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogGitLink, Warning, TEXT("%s"), *Text.ToString());
}

#undef LOCTEXT_NAMESPACE
