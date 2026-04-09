#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <SourceControlOperations.h>

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Connect — handles FConnect, Unreal's "is the provider usable?" probe.
//
// The repository was already opened in FGitLink_Provider::Init() via CheckRepositoryStatus(),
// so this handler's job is just to verify a real repo handle exists and report a sensible
// error otherwise. The UE editor uses the error text in the Connect dialog.
// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "GitLinkCmdConnect"

namespace gitlink::cmd
{
	auto Connect(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		TSharedRef<FConnect> ConnectOp = StaticCastSharedRef<FConnect>(InOperation);

		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			const FString LastError = gitlink::FRepository::Get_LastOpenError();
			const FText ErrText = FText::Format(
				LOCTEXT("ConnectNoRepo",
					"GitLink: no git repository found. {0}"),
				LastError.IsEmpty()
					? LOCTEXT("ConnectNoRepoGeneric", "Check the repository root override in Editor Preferences > GitLink.")
					: FText::FromString(LastError));

			ConnectOp->SetErrorText(ErrText);
			return FCommandResult::Fail(ErrText);
		}

		const FString BranchName = InCtx.Repository->Get_CurrentBranchName();

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Connect: connected to '%s' on branch '%s'"),
			*InCtx.RepoRootAbsolute,
			BranchName.IsEmpty() ? TEXT("(detached)") : *BranchName);

		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(FText::Format(
			LOCTEXT("ConnectOk", "Connected to git repository '{0}' on branch '{1}'."),
			FText::FromString(InCtx.RepoRootAbsolute),
			FText::FromString(BranchName)));
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
