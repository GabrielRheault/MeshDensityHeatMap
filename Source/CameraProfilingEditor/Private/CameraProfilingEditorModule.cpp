#include "CameraProfilingEditorModule.h"
#include "CameraProfilingTools.h"
#include "CameraProfilingHttpServer.h"

#include "Misc/App.h"
#include "ToolMenus.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "CameraProfiling"

IMPLEMENT_MODULE(FCameraProfilingEditorModule, CameraProfilingEditor)

namespace
{
	/** Grid-size presets shown under "1. Generate Cameras". (label, n); n<=0 => use settings. */
	struct FGridPreset { const TCHAR* Label; int32 N; };
	static const FGridPreset GGridPresets[] = {
		{ TEXT("Test (2 x 2)"), 2 },
		{ TEXT("4 x 4"), 4 },
		{ TEXT("6 x 6"), 6 },
		{ TEXT("8 x 8"), 8 },
		{ TEXT("10 x 10"), 10 },
		{ TEXT("15 x 15"), 15 },
		{ TEXT("20 x 20"), 20 },
		{ TEXT("30 x 30"), 30 },
		{ TEXT("40 x 40"), 40 },
		{ TEXT("From Settings (GridResolution)"), 0 },
	};

	void BuildGenerateSubmenu(UToolMenu* Menu)
	{
		FToolMenuSection& Section = Menu->AddSection("Presets", LOCTEXT("Presets", "Grid Size"));
		for (const FGridPreset& Preset : GGridPresets)
		{
			const int32 N = Preset.N;
			Section.AddMenuEntry(
				FName(Preset.Label),
				FText::FromString(Preset.Label),
				FText::GetEmpty(),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([N]() { FCameraProfilingTools::GenerateCameras(N, N); })));
		}
	}

	void BuildCameraProfilingSubmenu(UToolMenu* Menu)
	{
		FToolMenuSection& Section = Menu->AddSection("Actions", LOCTEXT("Actions", "Camera Profiling"));

		Section.AddSubMenu(
			"GenerateCameras",
			LOCTEXT("Generate", "1. Generate Cameras"),
			LOCTEXT("GenerateTip", "Export scene data + top-down render, build the grid, spawn cameras."),
			FNewToolMenuDelegate::CreateStatic(&BuildGenerateSubmenu));

		Section.AddMenuEntry(
			"ProfileCameras",
			LOCTEXT("Profile", "2. Profile from Cameras"),
			LOCTEXT("ProfileTip", "Run the per-camera trace + screenshot profiling pass in PIE."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FCameraProfilingTools::ProfileFromCameras)));

		Section.AddMenuEntry(
			"SeeDensityMap",
			LOCTEXT("SeeMap", "3. See Density Map"),
			LOCTEXT("SeeMapTip", "(Re)build and open the density heat map in the browser."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { FCameraProfilingTools::WriteHeatmap(/*bOpenBrowser=*/true); })));

		Section.AddMenuEntry(
			"RefreshData",
			LOCTEXT("Refresh", "Refresh Heat Map Data (rescan level, keep cameras)"),
			LOCTEXT("RefreshTip", "Re-scan the level into the heat map without regenerating cameras."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FCameraProfilingTools::RefreshHeatmapData)));
	}
}

void FCameraProfilingEditorModule::StartupModule()
{
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		MenuStartupHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCameraProfilingEditorModule::RegisterMenus));
	}

	// Localhost bridge so the heat map's "Go to / inspect cell" buttons can drive the editor.
	FCameraProfilingHttpServer::Get().Start();
}

void FCameraProfilingEditorModule::ShutdownModule()
{
	FCameraProfilingHttpServer::Get().Stop();

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FCameraProfilingEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}

	// Tools -> <ProjectName> -> Camera Profiling -> actions.
	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("YesChef");
	Section.AddSubMenu(
		"ProjectTools",
		FText::FromString(FApp::GetProjectName()),
		LOCTEXT("ProjectToolsTip", "Project tools"),
		FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			FToolMenuSection& Sub = Menu->AddSection("Tools");
			Sub.AddSubMenu(
				"CameraProfiling",
				LOCTEXT("CameraProfiling", "Camera Profiling"),
				LOCTEXT("CameraProfilingTip", "NavMesh-aware camera-grid performance profiler."),
				FNewToolMenuDelegate::CreateStatic(&BuildCameraProfilingSubmenu));
		}));
}

#undef LOCTEXT_NAMESPACE
