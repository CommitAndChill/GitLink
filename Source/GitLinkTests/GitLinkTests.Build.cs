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

		// Subprocess-level tests (Test_Subprocess_RunToFile.cpp) need to #include the internal
		// FGitLink_Subprocess header. It's kept Private by design (internal impl detail), so
		// expose it to the test module via an include-path rather than promoting it to Public/.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "GitLink", "Private"));

		// Cmd_Shared.h transitively #includes GitLinkLog.h which lives at the GitLink module
		// root (alongside Module.cpp). Tests that pull in Cmd_Shared.h to exercise the helpers
		// (Test_BoundedConcurrency.cpp) need the root on the include path too.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "GitLink"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"GitLinkCore",
			"libgit2",
		});

		// Tests that exercise the editor-side FGitLink_FileState predicates need the GitLink
		// module plus its ISourceControlState parent from the SourceControl module.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"GitLink",
			"SourceControl",
		});
	}
}
