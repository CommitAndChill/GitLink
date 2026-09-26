#include "Cmd_Push.h"

#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

namespace gitlink::cmd
{
	auto Push(
		FGitLink_Subprocess& InSubprocess,
		const FString&       InRepoRoot,
		double               InTimeoutSec) -> FPushOutcome
	{
		FPushOutcome Outcome;

		if (!InSubprocess.IsValid())
		{
			Outcome.ErrorMessage = TEXT("the git command-line tool was not found");
			return Outcome;
		}

		const FGitLink_SubprocessResult Result = InSubprocess.Run_Bounded({ TEXT("push") }, InRepoRoot, InTimeoutSec);
		if (!Result.IsSuccess())
		{
			Outcome.ErrorMessage = Result.Get_CombinedError();
			UE_LOG(LogGitLink, Warning, TEXT("Cmd_Push: 'git push' failed (exit %d): %s"),
				Result.ExitCode, *Outcome.ErrorMessage);
			return Outcome;
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Push: 'git push' succeeded"));
		Outcome.bOk = true;
		return Outcome;
	}
}
