#include "CameraProfilingEditorModule.h"
#include "CameraProfilingTools.h"
#include "CameraProfilingHttpServer.h"
#include "CameraProfilingPanel.h"

#include "Misc/App.h"
#include "ToolMenus.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "CameraProfiling"

IMPLEMENT_MODULE(FCameraProfilingEditorModule, CameraProfilingEditor)

void FCameraProfilingEditorModule::StartupModule()
{
	// Compile-time stamp so you can confirm in the Output Log which binary is actually loaded
	// (rules out a stale DLL — e.g. built while the editor was open, or a junction-shared binary).
	UE_LOG(LogTemp, Log, TEXT("[CameraProfiling] editor module loaded (built %s %s)"), TEXT(__DATE__), TEXT(__TIME__));

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCameraProfilingEditorModule::RegisterMenus));
	}

	// Dockable Camera Profiling panel (settings + action buttons).
	CameraProfilingPanel::Register();

	// Localhost bridge so the heat map's "Go to / inspect cell" buttons can drive the editor.
	FCameraProfilingHttpServer::Get().Start();
}

void FCameraProfilingEditorModule::ShutdownModule()
{
	FCameraProfilingHttpServer::Get().Stop();
	CameraProfilingPanel::Unregister();

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

	// Tools -> Camera Profiling : a single entry that opens the dockable panel (all actions live there).
	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("CameraProfiling");
	Section.AddMenuEntry(
		"CameraProfiling",
		LOCTEXT("CameraProfiling", "Camera Profiling"),
		LOCTEXT("CameraProfilingTip", "Open the Camera Profiling panel (grid, bounds, placement, and actions)."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&CameraProfilingPanel::Open)));
}

#undef LOCTEXT_NAMESPACE
