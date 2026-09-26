using UnrealBuildTool;

public class SourceControlBenchmark : ModuleRules
{
	public SourceControlBenchmark(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately depends on the generic source control interface only — never on a specific provider — so the
		// same code times GitLink, Git LFS 2 (Project Borealis) and Epic's Git plugin.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"SourceControl",
		});
	}
}
