#include <CoreMinimal.h>
#include <Features/IModularFeatures.h>
#include <HAL/FileManager.h>
#include <HAL/PlatformMisc.h>
#include <ISourceControlModule.h>
#include <ISourceControlProvider.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Guid.h>
#include <Misc/Paths.h>
#include <Modules/ModuleManager.h>
#include <SourceControlOperations.h>

IMPLEMENT_MODULE(FDefaultModuleImpl, SourceControlBenchmark)

// --------------------------------------------------------------------------------------------------------------------
// SourceControlBenchmark.Run — times common source control operations through the generic ISourceControlProvider
// interface, so the identical code measures any provider.
//
// Configuration (environment variables, read by the editor process running the test):
//   SCBENCH_PROVIDER   provider name to benchmark: "GitLink", "Git LFS 2" (Project Borealis) or "Git" (Epic). Required.
//   SCBENCH_ITERATIONS warm repetitions per operation (default 5).
//
// Run in a FRESH editor process whose revision control provider is "None" (no SourceControlSettings.ini), so the
// first Init + Connect is a genuine cold start. One process per provider: Git LFS 2 and Epic's Git plugin share the
// plugin name GitSourceControl and cannot both be loaded. See Docs/BENCHMARKS.md for the full procedure.
//
// Output: one "SCBENCH,<provider>,<operation>,<iteration>,<milliseconds>,<ok>" log line per sample, and the same rows
// appended to <Project>/Saved/SourceControlBenchmark/results.csv.
//
// Operations (the same inputs for every provider):
//   Init                 SetProvider(<name>) — the provider's own Init(false).
//   Connect.Cold         first synchronous FConnect after Init.
//   Connect.Warm         subsequent FConnect calls.
//   Status.100Files      FUpdateStatus on 100 explicit .uasset paths.
//   Status.1000Files     FUpdateStatus on 1000 explicit .uasset paths.
//   Status.Refresh       FUpdateStatus with an empty file list (each provider's "refresh everything").
//   MarkForAdd.100       FMarkForAdd on 100 new small files under Content/__SCBench.
//   Revert.100           FRevert on those same 100 files.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace scbench
{
	struct FRunner
	{
		FAutomationTestBase& Test;
		FString              ProviderName;
		TArray<FString>      CsvRows;

		auto Record(const TCHAR* InOperation, int32 InIteration, double InMs, bool bInOk) -> void
		{
			const FString Row = FString::Printf(TEXT("SCBENCH,%s,%s,%d,%.3f,%d"),
				*ProviderName, InOperation, InIteration, InMs, bInOk ? 1 : 0);
			UE_LOG(LogTemp, Display, TEXT("%s"), *Row);
			CsvRows.Add(Row.RightChop(8));   // drop the "SCBENCH," log prefix

			if (!bInOk)
			{ Test.AddWarning(FString::Printf(TEXT("%s iteration %d did not succeed"), InOperation, InIteration)); }
		}

		// Times one synchronous Execute. Returns success.
		auto Time_Execute(const TCHAR* InOperation, int32 InIteration,
			const FSourceControlOperationRef& InOp, const TArray<FString>& InFiles) -> bool
		{
			ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
			const double Start = FPlatformTime::Seconds();
			const bool bOk = Provider.Execute(InOp, InFiles, EConcurrency::Synchronous) == ECommandResult::Succeeded;
			Record(InOperation, InIteration, (FPlatformTime::Seconds() - Start) * 1000.0, bOk);
			return bOk;
		}

		auto Flush_Csv() const -> void
		{
			const FString Dir  = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SourceControlBenchmark"));
			const FString Path = FPaths::Combine(Dir, TEXT("results.csv"));
			IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/ true);

			FString Text;
			if (!IFileManager::Get().FileExists(*Path))
			{ Text += TEXT("provider,operation,iteration,ms,ok\n"); }
			Text += FString::Join(CsvRows, TEXT("\n")) + TEXT("\n");
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(), FILEWRITE_Append);
		}
	};

	auto Is_ProviderRegistered(const FString& InName) -> bool
	{
		const TArray<ISourceControlProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl"));
		for (const ISourceControlProvider* Provider : Providers)
		{
			if (Provider != nullptr && Provider->GetName().ToString() == InName)
			{ return true; }
		}
		return false;
	}

	// Deterministic sample of existing assets: sorted, so every provider sees the same paths.
	auto Collect_Assets(int32 InCount) -> TArray<FString>
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found,
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()), TEXT("*.uasset"),
			/*Files=*/ true, /*Directories=*/ false);
		Found.Sort();
		if (Found.Num() > InCount)
		{ Found.SetNum(InCount); }
		return Found;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSourceControlBenchmark_Run,
	"SourceControlBenchmark.Run",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceControlBenchmark_Run::RunTest(const FString& /*Parameters*/)
{
	const FString ProviderName = FPlatformMisc::GetEnvironmentVariable(TEXT("SCBENCH_PROVIDER")).TrimStartAndEnd();
	if (ProviderName.IsEmpty())
	{
		AddInfo(TEXT("SCBENCH_PROVIDER not set — benchmark skipped"));
		return true;
	}

	const FString IterText  = FPlatformMisc::GetEnvironmentVariable(TEXT("SCBENCH_ITERATIONS"));
	const int32   Iterations = FMath::Max(1, IterText.IsEmpty() ? 5 : FCString::Atoi(*IterText));

	if (!TestTrue(FString::Printf(TEXT("provider '%s' is registered"), *ProviderName),
		scbench::Is_ProviderRegistered(ProviderName)))
	{ return false; }

	scbench::FRunner Runner{ *this, ProviderName };

	// Cold start: Init, then the first Connect.
	{
		const double Start = FPlatformTime::Seconds();
		ISourceControlModule::Get().SetProvider(FName(*ProviderName));
		Runner.Record(TEXT("Init"), 0, (FPlatformTime::Seconds() - Start) * 1000.0, true);
	}
	if (!TestTrue(TEXT("first Connect succeeds"),
		Runner.Time_Execute(TEXT("Connect.Cold"), 0, ISourceControlOperation::Create<FConnect>(), {})))
	{
		Runner.Flush_Csv();
		return false;
	}

	const TArray<FString> Assets100  = scbench::Collect_Assets(100);
	const TArray<FString> Assets1000 = scbench::Collect_Assets(1000);
	AddInfo(FString::Printf(TEXT("provider=%s iterations=%d assets: %d / %d"),
		*ProviderName, Iterations, Assets100.Num(), Assets1000.Num()));

	for (int32 Iteration = 1; Iteration <= Iterations; ++Iteration)
	{
		Runner.Time_Execute(TEXT("Connect.Warm"),     Iteration, ISourceControlOperation::Create<FConnect>(),      {});
		Runner.Time_Execute(TEXT("Status.100Files"),  Iteration, ISourceControlOperation::Create<FUpdateStatus>(), Assets100);
		Runner.Time_Execute(TEXT("Status.1000Files"), Iteration, ISourceControlOperation::Create<FUpdateStatus>(), Assets1000);
		Runner.Time_Execute(TEXT("Status.Refresh"),   Iteration, ISourceControlOperation::Create<FUpdateStatus>(), {});

		// Mark for add + revert 100 fresh files. They are plain text under Content/__SCBench, so no provider treats
		// them as lockable; the directory is removed afterwards and the index is left as it was found.
		const FString Dir = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
			TEXT("__SCBench"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TArray<FString> NewFiles;
		for (int32 Index = 0; Index < 100; ++Index)
		{
			const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("File_%03d.txt"), Index));
			FFileHelper::SaveStringToFile(FString::Printf(TEXT("scbench %d\n"), Index), *Path);
			NewFiles.Add(Path);
		}

		Runner.Time_Execute(TEXT("MarkForAdd.100"), Iteration, ISourceControlOperation::Create<FMarkForAdd>(), NewFiles);
		Runner.Time_Execute(TEXT("Revert.100"),     Iteration, ISourceControlOperation::Create<FRevert>(),     NewFiles);

		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), /*RequireExists=*/ false, /*Tree=*/ true);
	}

	Runner.Flush_Csv();
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
