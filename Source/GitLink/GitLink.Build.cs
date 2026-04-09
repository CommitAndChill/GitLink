using UnrealBuildTool;

public class GitLink : ModuleRules
{
	public GitLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;
		bUseUnity = false;

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
