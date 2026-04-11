#include "GitLink_Subprocess.h"

#include "GitLinkLog.h"

#include <HAL/PlatformProcess.h>
#include <Misc/Paths.h>

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

auto FGitLink_Subprocess::Run(const TArray<FString>& InArgs) -> FGitLink_SubprocessResult
{
	FGitLink_SubprocessResult Result;

	if (!IsValid())
	{
		Result.StdErr = TEXT("FGitLink_Subprocess: no git binary configured");
		return Result;
	}

	const FString ArgsStr = JoinArgs(InArgs);

	UE_LOG(LogGitLink, Verbose,
		TEXT("Subprocess: %s %s  (cwd=%s)"),
		*_GitBinary, *ArgsStr, *_WorkingDirectory);

	int32   ExitCode = -1;
	FString StdOut;
	FString StdErr;

	const bool bSpawned = FPlatformProcess::ExecProcess(
		*_GitBinary,
		*ArgsStr,
		&ExitCode,
		&StdOut,
		&StdErr,
		_WorkingDirectory.IsEmpty() ? nullptr : *_WorkingDirectory);

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
		UE_LOG(LogGitLink, Verbose,
			TEXT("Subprocess: '%s %s' exited with %d: %s"),
			*_GitBinary, *ArgsStr, ExitCode, *Result.Get_CombinedError());
	}

	return Result;
}

auto FGitLink_Subprocess::RunLfs(const TArray<FString>& InArgs) -> FGitLink_SubprocessResult
{
	TArray<FString> LfsArgs;
	LfsArgs.Reserve(InArgs.Num() + 1);
	LfsArgs.Add(TEXT("lfs"));
	LfsArgs.Append(InArgs);
	return Run(LfsArgs);
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
	// We want everything that ends in ': set' (and NOT ': unset' or ': unspecified').
	TArray<FString> Lines;
	Result.StdOut.ParseIntoArrayLines(Lines, /*bCullEmpty=*/ true);

	for (const FString& Line : Lines)
	{
		if (!Line.EndsWith(TEXT(": set")))
		{ continue; }

		// Extract the wildcard from "<pattern>: lockable: set"
		int32 ColonIdx = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonIdx) || ColonIdx <= 0)
		{ continue; }

		FString Pattern = Line.Left(ColonIdx).TrimStartAndEnd();

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

	return Out;
}

auto FGitLink_Subprocess::QueryLfsLocks_Local() -> TMap<FString, FString>
{
	TMap<FString, FString> Out;
	if (!IsValid())
	{ return Out; }

	// `git lfs locks --local` lists locks held by the current user. Output format:
	//   Content/Path/To/File.uasset	Neil Koo	ID:12345
	// Tab-separated: path, owner, id.
	const FGitLink_SubprocessResult Result = RunLfs({ TEXT("locks"), TEXT("--local") });
	if (!Result.IsSuccess())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("QueryLfsLocks_Local: git lfs locks --local failed: %s"),
			*Result.Get_CombinedError());
		return Out;
	}

	TArray<FString> Lines;
	Result.StdOut.ParseIntoArrayLines(Lines, /*bCullEmpty=*/ true);

	for (const FString& Line : Lines)
	{
		// Parse tab-separated fields. git-lfs uses tabs between columns.
		FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{ continue; }

		// The output format is: <path>\t<owner>\t<id>
		// But some versions use variable whitespace. Split on tab first.
		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT("\t"), /*bCullEmpty=*/ true);

		if (Parts.Num() >= 2)
		{
			FString Path  = Parts[0].TrimStartAndEnd();
			FString Owner = Parts[1].TrimStartAndEnd();

			// Normalize to forward slashes
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));

			Out.Add(MoveTemp(Path), MoveTemp(Owner));
		}
	}

	return Out;
}
