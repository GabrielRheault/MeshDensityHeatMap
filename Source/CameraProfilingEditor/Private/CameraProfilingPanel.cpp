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
#include "Widgets/Input/SComboBox.h"
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

	// --- Generation history dropdown state ---
	TArray<TSharedPtr<FString>> GSnapshots;            // timestamp folder names (newest first)
	TSharedPtr<FString> GSelectedSnapshot;             // current dropdown selection
	TSharedPtr<SComboBox<TSharedPtr<FString>>> GSnapshotCombo;

	void RefreshSnapshots()
	{
		GSnapshots.Reset();
		for (const FString& Name : FCameraProfilingTools::ListSnapshots())
		{
			GSnapshots.Add(MakeShared<FString>(Name));
		}
		if (!GSelectedSnapshot.IsValid() || !GSnapshots.ContainsByPredicate(
			[](const TSharedPtr<FString>& S) { return S.IsValid() && GSelectedSnapshot.IsValid() && *S == *GSelectedSnapshot; }))
		{
			GSelectedSnapshot = GSnapshots.Num() > 0 ? GSnapshots[0] : nullptr;
		}
		if (GSnapshotCombo.IsValid()) { GSnapshotCombo->RefreshOptions(); }
	}

	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs&)
	{
		RefreshSnapshots(); // populate the history dropdown before building it

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
								[]() { FCameraProfilingTools::GenerateData(0, 0); })
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

						// History: switch the current data to a past generation, then reopen its heat map.
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 2.f)
						[
							SNew(STextBlock).Text(LOCTEXT("HistoryLabel", "History — switch to a past generation:"))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
							[
								SAssignNew(GSnapshotCombo, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&GSnapshots)
								.OnComboBoxOpening_Lambda([]() { RefreshSnapshots(); })
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
									{ return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())); })
								.OnSelectionChanged_Lambda([](TSharedPtr<FString> In, ESelectInfo::Type) { GSelectedSnapshot = In; })
								[
									SNew(STextBlock).Text_Lambda([]()
										{ return FText::FromString(GSelectedSnapshot.IsValid() ? *GSelectedSnapshot : FString(TEXT("(no generations yet)"))); })
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
							[
								ActionButton(LOCTEXT("LoadSnap", "Load"),
									LOCTEXT("LoadSnapTip", "Restore the selected generation's JSON + top-down image as the current data and reopen its heat map."),
									[]() { if (GSelectedSnapshot.IsValid()) { FCameraProfilingTools::LoadSnapshot(*GSelectedSnapshot); } })
							]
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
