using UnrealBuildTool;

public class DQXISAutoPipeline : ModuleRules
{
	public DQXISAutoPipeline(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"AssetTools",
			"AssetRegistry"
		});
	}
}
