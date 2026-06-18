using UnrealBuildTool;

public class CameraProfilingEditor : ModuleRules
{
	public CameraProfilingEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"UnrealEd",          // GEditor, actor selection, viewports, PIE
			"LevelEditor",       // ULevelEditorSubsystem (PIE begin/end)
			"ToolMenus",         // Tools -> Yes Chef menu
			"Slate",
			"SlateCore",
			"Json",              // scene_data.json / camera_*.json
			"Projects",          // IPluginManager -> Resources/heatmap_template.html
			"RenderCore",        // FStaticMeshLODResources::GetNumTriangles
			"NavigationSystem",  // ground-Z projection + on-navmesh validation
			"DeveloperSettings", // UCameraProfilingSettings
			"HTTPServer",        // localhost bridge for the heat map
		});
	}
}
