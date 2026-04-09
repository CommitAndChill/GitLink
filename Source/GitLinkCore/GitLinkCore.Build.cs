using System.IO;
using UnrealBuildTool;

public class GitLinkCore : ModuleRules
{
	public GitLinkCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;
		bUseUnity = false;

		// Make the module root (where GitLinkCore_Module.h and GitLinkCoreLog.h live) findable
		// from .cpp files inside Private/. UBT only adds Public/ and Private/ automatically.
		PrivateIncludePaths.Add(ModuleDirectory);

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
