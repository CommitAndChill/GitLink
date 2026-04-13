using System.IO;
using UnrealBuildTool;

public class GitLink : ModuleRules
{
	public GitLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		// Make the module root (where GitLink_Module.h and GitLinkLog.h live) findable from
		// .cpp files inside Private/.
		PrivateIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SourceControl",
			"Projects",
			"GitLinkCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"DeveloperSettings",
			"EditorStyle",
			"ToolMenus",
			"ContentBrowser",
		});
	}
}
