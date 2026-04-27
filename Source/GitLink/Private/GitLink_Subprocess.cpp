#include "GitLink_Subprocess.h"

#include "GitLinkLog.h"

#include <Dom/JsonObject.h>
#include <HAL/FileManager.h>
#include <HAL/PlatformProcess.h>
#include <Misc/FileHelper.h>
#include <Misc/Paths.h>
#include <Serialization/JsonReader.h>
#include <Serialization/JsonSerializer.h>

// --------------------------------------------------------------------------------------------------------------------

namespace
{
	// Quote a single argument if it contains whitespace or a quote character. Matches how
	// Windows CreateProcess parses the command-line string.
	auto QuoteArg(const FString& InArg) -> FString
	{
		const bool bNeedsQuotes = InArg.Contains(TEXT(" ")) || InArg.Contains(TEXT("\"")) || InArg.IsEmpty();
		if (!bNeedsQuotes)
		{ return InArg; }

		FString Escaped = InArg;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	auto JoinArgs(const TArray<FString>& InArgs) -> FString
	{
		TArray<FString> Quoted;
		Quoted.Reserve(InArgs.Num());
		for (const FString& Arg : InArgs)
		{ Quoted.Add(QuoteArg(Arg)); }
		return FString::Join(Quoted, TEXT(" "));
	}
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_SubprocessResult::Get_CombinedError() const -> FString
{
	const FString TrimmedErr = StdErr.TrimStartAndEnd();
	if (!TrimmedErr.IsEmpty())
	{ return TrimmedErr; }

	const FString TrimmedOut = StdOut.TrimStartAndEnd();
	if (!TrimmedOut.IsEmpty())
	{ return TrimmedOut; }

	if (!bSpawned)
	{ return TEXT("(git could not be launched)"); }

	return FString::Printf(TEXT("git exited with code %d"), ExitCode);
}

// --------------------------------------------------------------------------------------------------------------------

FGitLink_Subprocess::FGitLink_Subprocess(FString InGitBinary, FString InWorkingDirectory)
	: _GitBinary(MoveTemp(InGitBinary))
	, _WorkingDirectory(MoveTemp(InWorkingDirectory))
{
}

auto FGitLink_Subprocess::Run(const TArray<FString>& InArgs, const FString& InCwdOverride)
	-> FGitLink_SubprocessResult
{
	FGitLink_SubprocessResult Result;

	if (!IsValid())
	{
		Result.StdErr = TEXT("FGitLink_Subprocess: no git binary configured");
		return Result;
	}

	const FString ArgsStr = JoinArgs(InArgs);
	const FString& EffectiveCwd = InCwdOverride.IsEmpty() ? _WorkingDirectory : InCwdOverride;

	UE_LOG(LogGitLink, Verbose,
		TEXT("Subprocess: %s %s  (cwd=%s)"),
		*_GitBinary, *ArgsStr, *EffectiveCwd);

	int32   ExitCode = -1;
	FString StdOut;
	FString StdErr;

	const bool bSpawned = FPlatformProcess::ExecProcess(
		*_GitBinary,
		*ArgsStr,
		&ExitCode,
		&StdOut,
		&StdErr,
		EffectiveCwd.IsEmpty() ? nullptr : *EffectiveCwd);

	Result.bSpawned = bSpawned;
	Result.ExitCode = ExitCode;
	Result.StdOut   = MoveTemp(StdOut);
	Result.StdErr   = MoveTemp(StdErr);

	if (!bSpawned)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("Subprocess: failed to spawn '%s %s'"),
			*_GitBinary, *ArgsStr);
	}
	else if (ExitCode != 0)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("Subprocess: '%s %s' exited with %d: %s"),
			*_GitBinary, *ArgsStr, ExitCode, *Result.Get_CombinedError());
	}

	return Result;
}

auto FGitLink_Subprocess::RunLfs(const TArray<FString>& InArgs, const FString& InCwdOverride)
	-> FGitLink_SubprocessResult
{
	TArray<FString> LfsArgs;
	LfsArgs.Reserve(InArgs.Num() + 1);
	LfsArgs.Add(TEXT("lfs"));
	LfsArgs.Append(InArgs);
	return Run(LfsArgs, InCwdOverride);
}

auto FGitLink_Subprocess::IsLfsAvailable() -> bool
{
	if (!IsValid())
	{ return false; }

	const FGitLink_SubprocessResult VerCheck = RunLfs({ TEXT("version") });
	return VerCheck.IsSuccess();
}

auto FGitLink_Subprocess::ProbeLockableExtensions(const TArray<FString>& InWildcards) -> TArray<FString>
{
	TArray<FString> Out;
	if (!IsValid() || InWildcards.IsEmpty())
	{ return Out; }

	// git check-attr lockable <files...>
	TArray<FString> Args;
	Args.Reserve(InWildcards.Num() + 2);
	Args.Add(TEXT("check-attr"));
	Args.Add(TEXT("lockable"));
	Args.Append(InWildcards);

	const FGitLink_SubprocessResult Result = Run(Args);
	if (!Result.IsSuccess())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("ProbeLockableExtensions: git check-attr failed: %s"),
			*Result.Get_CombinedError());
		return Out;
	}

	// Output lines look like:
	//   *.uasset: lockable: set
	//   *.umap: lockable: set
	//   *.txt: lockable: unspecified
	// We want everything whose value is literally "set" (and NOT "unset" or "unspecified").
	UE_LOG(LogGitLink, Verbose,
		TEXT("ProbeLockableExtensions: raw stdout (%d bytes):\n%s"),
		Result.StdOut.Len(), *Result.StdOut);

	TArray<FString> Lines;
	Result.StdOut.ParseIntoArrayLines(Lines, /*bCullEmpty=*/ true);

	for (FString Line : Lines)
	{
		// Trim whitespace / stray CR that ParseIntoArrayLines can leave behind depending
		// on git's line-ending behavior on Windows.
		Line.TrimStartAndEndInline();

		// Expected format: "<pattern>: <attr>: <value>". Split on ": " and inspect the
		// trailing value; anything that's not "set" (e.g. "unset", "unspecified") is skipped.
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(": "), /*CullEmpty=*/ false);
		if (Parts.Num() < 3)
		{ continue; }

		const FString Value = Parts.Last().TrimStartAndEnd();
		if (!Value.Equals(TEXT("set"), ESearchCase::IgnoreCase))
		{ continue; }

		// Pattern is everything before the trailing ": <attr>: <value>" — rebuild from the
		// leading parts to tolerate patterns that might themselves contain ": ".
		TArray<FString> PatternParts;
		PatternParts.Reserve(Parts.Num() - 2);
		for (int32 Idx = 0; Idx < Parts.Num() - 2; ++Idx)
		{ PatternParts.Add(Parts[Idx]); }
		FString Pattern = FString::Join(PatternParts, TEXT(": ")).TrimStartAndEnd();

		// Strip leading "*" so we have just the extension (".uasset").
		if (Pattern.StartsWith(TEXT("*")))
		{
			Pattern.RightChopInline(1, EAllowShrinking::No);
		}

		if (!Pattern.IsEmpty() && Pattern.StartsWith(TEXT(".")))
		{
			Out.Add(MoveTemp(Pattern));
		}
	}

	if (Out.IsEmpty())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("ProbeLockableExtensions: no 'lockable: set' entries parsed from git check-attr output. ")
			TEXT("Either .gitattributes has no lockable patterns, or git returned an unexpected format."));
	}

	return Out;
}

auto FGitLink_Subprocess::QueryLfsLocks_Verified(const FString& InCwdOverride) -> FLfsLocksSnapshot
{
	FLfsLocksSnapshot Out;
	if (!IsValid())
	{ return Out; }

	// `git lfs locks --verify --json` is a network call that asks the LFS server which of
	// the listed locks the current user actually owns. The server is the source of truth
	// — `git config user.name` may differ from the LFS server's recorded owner identity,
	// so any owner-comparison done client-side is unreliable. The `--local` flag (used by
	// earlier versions of this code) is also unreliable — its cache contents include locks
	// from other users after any prior remote query, despite the docs saying otherwise.
	//
	// JSON shape (git-lfs 3.x):
	//   { "ours":   [{"id":"..","path":"..","owner":{"name":".."}, "locked_at":".."}],
	//     "theirs": [{"id":"..","path":"..","owner":{"name":".."}, "locked_at":".."}] }
	const FGitLink_SubprocessResult Result = RunLfs(
		{ TEXT("locks"), TEXT("--verify"), TEXT("--json") },
		InCwdOverride);

	if (!Result.IsSuccess())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("QueryLfsLocks_Verified: git lfs locks --verify --json failed: %s"),
			*Result.Get_CombinedError());
		return Out;
	}

	Out = Parse_LfsVerifyJson(Result.StdOut);

	if (!Out.bSuccess)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("QueryLfsLocks_Verified: could not parse JSON output (%d bytes)"),
			Result.StdOut.Len());
		return Out;
	}

	UE_LOG(LogGitLink, Verbose,
		TEXT("QueryLfsLocks_Verified (cwd='%s'): ours=%d, theirs=%d"),
		InCwdOverride.IsEmpty() ? *_WorkingDirectory : *InCwdOverride,
		Out.OursPaths.Num(),
		Out.AllLocks.Num() - Out.OursPaths.Num());

	return Out;
}

auto FGitLink_Subprocess::Parse_LfsVerifyJson(const FString& InJsonText) -> FLfsLocksSnapshot
{
	FLfsLocksSnapshot Out;

	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InJsonText);
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// bSuccess stays false — caller distinguishes "couldn't query" from "no locks".
		return Out;
	}

	// JSON parsed → poll succeeded. Even an empty result (no locks) counts as success.
	Out.bSuccess = true;

	// Helper: pull out path + owner.name from a single lock object and add to the snapshot.
	auto IngestArray = [&](const TArray<TSharedPtr<FJsonValue>>* InArray, bool bIsOurs)
	{
		if (InArray == nullptr) { return; }
		for (const TSharedPtr<FJsonValue>& Val : *InArray)
		{
			const TSharedPtr<FJsonObject>* AsObj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(AsObj) || !AsObj || !AsObj->IsValid())
			{ continue; }

			FString Path;
			if (!(*AsObj)->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
			{ continue; }

			FString OwnerName;
			const TSharedPtr<FJsonObject>* OwnerObj = nullptr;
			if ((*AsObj)->TryGetObjectField(TEXT("owner"), OwnerObj) && OwnerObj && OwnerObj->IsValid())
			{
				(*OwnerObj)->TryGetStringField(TEXT("name"), OwnerName);
			}

			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			Out.AllLocks.Add(Path, OwnerName);
			if (bIsOurs)
			{ Out.OursPaths.Add(Path); }
		}
	};

	const TArray<TSharedPtr<FJsonValue>>* OursArr   = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* TheirsArr = nullptr;
	Root->TryGetArrayField(TEXT("ours"),   OursArr);
	Root->TryGetArrayField(TEXT("theirs"), TheirsArr);
	IngestArray(OursArr,   /*bIsOurs=*/ true);
	IngestArray(TheirsArr, /*bIsOurs=*/ false);

	return Out;
}

auto FGitLink_Subprocess::RunToFile(const TArray<FString>& InArgs, const FString& InOutputFile,
	const FString& InWorkingDirOverride) -> bool
{
	if (!IsValid())
	{ return false; }

	const FString ArgsStr = JoinArgs(InArgs);
	const FString& WorkDir = InWorkingDirOverride.IsEmpty() ? _WorkingDirectory : InWorkingDirOverride;

	UE_LOG(LogGitLink, Verbose,
		TEXT("RunToFile: %s %s -> %s"), *_GitBinary, *ArgsStr, *InOutputFile);

	// Create the process with piped stdout so we can capture binary output.
	void* ReadPipe  = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	// UE's CreateProc parameters are named from the CHILD process's perspective:
	//   PipeWriteChild = the handle the CHILD writes to → we pass WritePipe here to capture
	//                    the child's stdout (we then read from the matching ReadPipe).
	//   PipeReadChild  = the handle the CHILD reads from (its stdin) → nullptr, we don't send
	//                    anything to git's stdin for cat-file.
	// Prior versions had these swapped, causing 0-byte extractions because git's stdout was
	// dropped on the floor while its stdin was hooked to our unused WritePipe.
	FProcHandle Proc = FPlatformProcess::CreateProc(
		*_GitBinary,
		*ArgsStr,
		/*bLaunchDetached=*/ false,
		/*bLaunchHidden=*/ true,
		/*bLaunchReallyHidden=*/ true,
		/*OutProcessID=*/ nullptr,
		/*InPriority=*/ 0,
		WorkDir.IsEmpty() ? nullptr : *WorkDir,
		/*PipeWriteChild=*/ WritePipe,
		/*PipeReadChild=*/  nullptr);

	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		UE_LOG(LogGitLink, Warning, TEXT("RunToFile: failed to spawn git"));
		return false;
	}

	// Read binary data from the pipe into a byte array.
	TArray<uint8> FileContent;
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		TArray<uint8> Chunk;
		FPlatformProcess::ReadPipeToArray(ReadPipe, Chunk);
		if (Chunk.Num() > 0)
		{
			FileContent.Append(Chunk);
		}
		FPlatformProcess::Sleep(0.01f);
	}

	// Read any remaining data after process exit.
	{
		TArray<uint8> Chunk;
		FPlatformProcess::ReadPipeToArray(ReadPipe, Chunk);
		if (Chunk.Num() > 0)
		{
			FileContent.Append(Chunk);
		}
	}

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	if (ReturnCode != 0)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("RunToFile: git exited with code %d"), ReturnCode);
		return false;
	}

	// Treat zero-byte output as failure. Real .uasset extractions are always non-empty, and a
	// zero-byte "success" has historically masked pipe-wiring bugs (see the PipeWriteChild /
	// PipeReadChild comment above). Callers that legitimately want to capture empty output can
	// check the file size themselves.
	if (FileContent.Num() == 0)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("RunToFile: git exited 0 but produced no output — check args and that stdout "
				 "was actually captured"));
		return false;
	}

	// Write to disk.
	if (!FFileHelper::SaveArrayToFile(FileContent, *InOutputFile))
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("RunToFile: failed to write '%s' (%d bytes)"), *InOutputFile, FileContent.Num());
		return false;
	}

	UE_LOG(LogGitLink, Verbose,
		TEXT("RunToFile: wrote %d bytes to '%s'"), FileContent.Num(), *InOutputFile);
	return true;
}
