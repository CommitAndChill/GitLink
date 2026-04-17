using UnrealBuildTool;

public class GitLinkTests : ModuleRules
{
	public GitLinkTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		// Make the module root (where GitLinkTests_Module.h and GitLinkTestsLog.h live) findable
		// from .cpp files inside Private/.
		PrivateIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"GitLinkCore",
			"libgit2",
		});
	}
}
