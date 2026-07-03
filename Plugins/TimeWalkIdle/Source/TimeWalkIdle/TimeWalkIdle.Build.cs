// Copyright TimeWalk. Standalone procedural idle-motion module for MetaHumans.
// Extracted from TimeWalkCharacters and decoupled from Inworld: builds and runs with
// NO Inworld plugins installed (TIMEWALK_WITH_INWORLD=0). If the InworldCharacter plugin
// IS present (this project or the plugin's sibling dir), the define flips to 1 and the
// speech-ramp + emotion-bias code compiles back in.

using System.IO;
using UnrealBuildTool;

public class TimeWalkIdle : ModuleRules
{
	public TimeWalkIdle(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
		PrivateIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AnimGraphRuntime"    // FRotator anim-instance path / AnimGraph runtime types
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// UGroomComponent ready-gate references in the idle path.
			"HairStrandsCore",
			// idle variation pool auto-loads retargeted clips via AssetRegistry.
			"AssetRegistry"
		});

		// ---- Optional Inworld detection --------------------------------------------
		// Default OFF. Auto-enable ONLY if the InworldCharacter plugin exists either as a
		// sibling of this plugin or under the consuming project's Plugins/ folder. When OFF,
		// every Inworld touchpoint is compiled out and the module builds with zero Inworld
		// headers -> pure idle/gaze/blink/camera works standalone.
		bool bInworld = Directory.Exists(Path.Combine(PluginDirectory, "..", "InworldCharacter"))
			|| (Target.ProjectFile != null && Directory.Exists(Path.Combine(
				 Path.GetDirectoryName(Target.ProjectFile.FullName), "Plugins", "InworldCharacter")));
		PublicDefinitions.Add("TIMEWALK_WITH_INWORLD=" + (bInworld ? "1" : "0"));
		if (bInworld)
		{
			PublicDependencyModuleNames.AddRange(new string[] { "InworldRuntime", "InworldCharacter" });
		}
	}
}
