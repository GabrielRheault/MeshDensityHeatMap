using UnrealBuildTool;

public class CameraProfilingRuntime : ModuleRules
{
	public CameraProfilingRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",   // parse camera_positions.json
		});
	}
}
