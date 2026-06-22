#include "CameraProfilingPanel.h"
#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyEditorDelegates.h"
#include "IDetailsView.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"

#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "CameraProfilingPanel"

namespace
{
	const FName GPanelTabName(TEXT("CameraProfilingPanel"));

	/** Advanced/rarely-touched settings hidden from the panel (still editable in Project Settings). */
	bool IsSettingVisibleInPanel(const FPropertyAndParent& PropertyAndParent)
	{
		static const TSet<FName> Hidden = {
			"Jitter", "Padding", "AimFraction", "RandomSeed", "BaseRotation",
			"TraceExtraHeight", "TraceDepth", "ClusterCellSize", "MinClusterWeight",
			"HeatmapSubdiv", "bExportInstances", "GotoPort", "WarmupTicks", "SettleTicks"
		};
		return !Hidden.Contains(PropertyAndParent.Property.GetFName());
	}

	/** A full-width action button that runs Fn on click. */
	TSharedRef<SWidget> ActionButton(const FText& Label, const FText& Tip, TFunction<void()> Fn)
	{
		return SNew(SButton)
			.ToolTipText(Tip)
			.HAlign(HAlign_Center)
			.ContentPadding(FMargin(6.f, 4.f))
			.OnClicked_Lambda([Fn]() { Fn(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label)
			];
	}

	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs&)
	{
		// Details view bound to the settings CDO -> auto-generates the right widget per property
		// (grid resolution, the Bounds Source dropdown, height above ground, etc.).
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		FDetailsViewArgs Args;
		Args.bAllowSearch = true;
		Args.bHideSelectionTip = true;
		Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		TSharedRef<IDetailsView> Details = PropertyEditor.CreateDetailView(Args);
		Details->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateStatic(&IsSettingVisibleInPanel));
		Details->SetObject(GetMutableDefault<UCameraProfilingSettings>());
		// Persist edits to the per-user config (the tools read GetDefault<>, the same object).
		Details->OnFinishedChangingProperties().AddLambda([](const FPropertyChangedEvent&)
		{
			GetMutableDefault<UCameraProfilingSettings>()->SaveConfig();
		});

		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					Details
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(8.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
						[
							ActionButton(LOCTEXT("Generate", "1. Generate Data (cameras + JSON)"),
								LOCTEXT("GenerateTip", "Scan the level into scene_data.json (density incl. lights) + a top-down render, build the camera grid at the resolution above, and spawn the GridCam cameras."),
								[]() { FCameraProfilingTools::GenerateCameras(0, 0); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
						[
							ActionButton(LOCTEXT("Profile", "2. Run Camera Profiling"),
								LOCTEXT("ProfileTip", "Fly the view through the cameras: per-camera Unreal Insights trace + screenshot + frame timing."),
								[]() { FCameraProfilingTools::ProfileFromCameras(); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
						[
							ActionButton(LOCTEXT("OpenMap", "3. Open Heat Map"),
								LOCTEXT("OpenMapTip", "Build and open the density heat map in your browser (from the latest generated/profiled data)."),
								[]() { FCameraProfilingTools::WriteHeatmap(/*bOpenBrowser=*/true); })
						]
					]
				]
			];
	}
}

void CameraProfilingPanel::Register()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GPanelTabName, FOnSpawnTab::CreateStatic(&SpawnPanelTab))
		.SetDisplayName(LOCTEXT("PanelTitle", "Camera Profiling"))
		.SetTooltipText(LOCTEXT("PanelTooltip", "Camera grid + heat map controls (grid size, bounds, placement)."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"));
}

void CameraProfilingPanel::Unregister()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPanelTabName);
	}
}

void CameraProfilingPanel::Open()
{
	FGlobalTabmanager::Get()->TryInvokeTab(GPanelTabName);
}

#undef LOCTEXT_NAMESPACE
