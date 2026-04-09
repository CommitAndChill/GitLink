// ThirdParty wrapper for libgit2 (https://libgit2.org). See README.md for the build recipe.

using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

public class libgit2 : ModuleRules
{
	public libgit2(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string IncludeDir = Path.Combine(ModuleDirectory, "include");
		PublicSystemIncludePaths.Add(IncludeDir);

		// WITH_LIBGIT2 is defined to 1 only when the prebuilt binaries for the current
		// platform are actually present on disk. This lets GitLink compile cleanly before
		// libgit2 has been dropped in, and automatically lights up once it has.
		//
		// NOTE: UBT does not track File.Exists() calls as build inputs, so if you drop
		// new binaries into this folder you must touch this Build.cs file (or delete
		// Plugins/GitLink/Intermediate) to force UBT to re-evaluate.
		bool bHasBinaries = false;

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string LibDir = Path.Combine(ModuleDirectory, "lib", "Win64");
			string BinDir = Path.Combine(ModuleDirectory, "bin", "Win64");
			string LibPath = Path.Combine(LibDir, "git2.lib");
			string DllPath = Path.Combine(BinDir, "git2.dll");
			string HeaderPath = Path.Combine(IncludeDir, "git2.h");

			bool bLibExists    = File.Exists(LibPath);
			bool bDllExists    = File.Exists(DllPath);
			bool bHeaderExists = File.Exists(HeaderPath);

			Log.TraceInformation(
				"GitLink libgit2: lib={0} dll={1} header={2} (paths: {3} | {4} | {5})",
				bLibExists, bDllExists, bHeaderExists, LibPath, DllPath, HeaderPath);

			if (bLibExists && bDllExists && bHeaderExists)
			{
				PublicAdditionalLibraries.Add(LibPath);

				// Delay load so a missing DLL at runtime becomes a graceful disable of
				// the provider rather than a module-load crash.
				PublicDelayLoadDLLs.Add("git2.dll");

				// Ship the DLL next to the editor/target executable.
				RuntimeDependencies.Add(Path.Combine("$(TargetOutputDir)", "git2.dll"), DllPath);

				// libgit2 built with USE_HTTPS=WinHTTP pulls in these Windows system libs.
				PublicSystemLibraries.AddRange(new string[] {
					"winhttp.lib",
					"rpcrt4.lib",
					"crypt32.lib",
					"ole32.lib",
				});

				bHasBinaries = true;
			}
		}

		Log.TraceInformation("GitLink libgit2: WITH_LIBGIT2={0}", bHasBinaries ? 1 : 0);
		PublicDefinitions.Add("WITH_LIBGIT2=" + (bHasBinaries ? "1" : "0"));
	}
}
