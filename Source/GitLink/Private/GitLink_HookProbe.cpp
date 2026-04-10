#include "GitLink_HookProbe.h"

#include "GitLinkLog.h"

#include <HAL/PlatformFileManager.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_HookProbe::Probe(const FString& InRepoRoot) -> FGitLink_HookFlags
{
	FGitLink_HookFlags Flags;

	const FString HooksDir = FPaths::Combine(InRepoRoot, TEXT(".git"), TEXT("hooks"));

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*HooksDir))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("HookProbe: no .git/hooks/ directory found at '%s'"), *HooksDir);
		return Flags;
	}

	// Map of hook filename (no extension on Unix; .exe/.bat/.ps1 on Windows, but git for
	// Windows ships hooks as extensionless shell scripts too) to the flag field.
	struct FHookEntry
	{
		const TCHAR* Name;
		bool FGitLink_HookFlags::*Flag;
	};

	static const FHookEntry Hooks[] = {
		{ TEXT("pre-commit"),    &FGitLink_HookFlags::bHasPreCommit    },
		{ TEXT("commit-msg"),    &FGitLink_HookFlags::bHasCommitMsg    },
		{ TEXT("pre-push"),      &FGitLink_HookFlags::bHasPrePush      },
		{ TEXT("post-checkout"), &FGitLink_HookFlags::bHasPostCheckout },
		{ TEXT("post-merge"),    &FGitLink_HookFlags::bHasPostMerge    },
	};

	for (const FHookEntry& Entry : Hooks)
	{
		const FString HookPath = FPaths::Combine(HooksDir, Entry.Name);

		// Check for the exact name (extensionless, standard on all platforms).
		if (PlatformFile.FileExists(*HookPath))
		{
			Flags.*Entry.Flag = true;
			continue;
		}

		// On Windows, husky and other hook managers sometimes drop .sh / .bat / .ps1 variants.
		for (const TCHAR* Ext : { TEXT(".sh"), TEXT(".bat"), TEXT(".ps1"), TEXT(".exe") })
		{
			if (PlatformFile.FileExists(*(HookPath + Ext)))
			{
				Flags.*Entry.Flag = true;
				break;
			}
		}
	}

	if (Flags.HasAnyHooks())
	{
		UE_LOG(LogGitLink, Log,
			TEXT("HookProbe: detected hooks — pre-commit=%s commit-msg=%s pre-push=%s post-checkout=%s post-merge=%s"),
			Flags.bHasPreCommit    ? TEXT("yes") : TEXT("no"),
			Flags.bHasCommitMsg    ? TEXT("yes") : TEXT("no"),
			Flags.bHasPrePush      ? TEXT("yes") : TEXT("no"),
			Flags.bHasPostCheckout ? TEXT("yes") : TEXT("no"),
			Flags.bHasPostMerge    ? TEXT("yes") : TEXT("no"));
	}
	else
	{
		UE_LOG(LogGitLink, Verbose, TEXT("HookProbe: no active hooks found"));
	}

	return Flags;
}
