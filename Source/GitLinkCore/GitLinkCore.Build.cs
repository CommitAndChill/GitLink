using UnrealBuildTool;

public class GitLinkCore : ModuleRules
{
	public GitLinkCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Projects",
			"libgit2",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
