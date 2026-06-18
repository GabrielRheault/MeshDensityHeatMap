#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CameraProfilingSettings.generated.h"

/** How a spawned camera's ground Z is resolved. */
UENUM()
enum class ECameraPlacementMethod : uint8
{
	/** Down line-trace (Visibility, then WorldStatic/WorldDynamic object trace). */
	Raycast,
	/** Project the point onto the NavMesh (falls back to Raycast if projection fails). */
	NavMesh,
};

/** Which extent the camera grid is laid over. */
UENUM()
enum class ECameraBoundsSource : uint8
{
	/** Union of the level's NavMeshBoundsVolume AABBs (falls back to Scene if none). */
	NavMesh,
	/** Overall actor/instance bounds of the level. */
	Scene,
};

/** Where per-camera profiling runs. */
UENUM()
enum class ECameraProfileMode : uint8
{
	/** Play-In-Editor: game render path + game scalability (shipping-representative). */
	PIE,
	/** Move the editor viewport (faster, but editor render path is costlier). */
	Editor,
};

/**
 * Settings for the Camera Profiling plugin (Project Settings -> Yes Chef -> Camera Profiling).
 * Replaces the old Python CONFIG dict; all distances are in Unreal units (cm).
 */
UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Camera Profiling"))
class CAMERAPROFILINGEDITOR_API UCameraProfilingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Yes Chef"); }

	// --- Grid layout ---
	/** Default cells [X, Y] when "From Settings" is chosen in the Generate Cameras submenu. */
	UPROPERTY(EditAnywhere, config, Category = "Grid")
	FIntPoint GridResolution = FIntPoint(10, 10);

	/** Random jitter inside each cell, as a fraction of the cell half-size (0..1). */
	UPROPERTY(EditAnywhere, config, Category = "Grid", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Jitter = 0.5f;

	/** Shrink the usable bounds inward by this fraction on each side (0..0.49). */
	UPROPERTY(EditAnywhere, config, Category = "Grid", meta = (ClampMin = "0.0", ClampMax = "0.49"))
	float Padding = 0.05f;

	/** Lay the grid over NavMesh volumes or the whole-scene bounds. */
	UPROPERTY(EditAnywhere, config, Category = "Grid")
	ECameraBoundsSource BoundsSource = ECameraBoundsSource::NavMesh;

	/** Aim each camera at the nearest dense asset cluster (face density, not empty space). */
	UPROPERTY(EditAnywhere, config, Category = "Grid")
	bool bAimAtClusters = true;

	/** Fraction of cameras that get aimed at a cluster (0..1). */
	UPROPERTY(EditAnywhere, config, Category = "Grid", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AimFraction = 1.0f;

	/** Seed for the jitter/aim RNG so layouts are reproducible. */
	UPROPERTY(EditAnywhere, config, Category = "Grid")
	int32 RandomSeed = 1234;

	/** [pitch, yaw, roll] base rotation; cluster aiming overrides yaw. */
	UPROPERTY(EditAnywhere, config, Category = "Grid")
	FRotator BaseRotation = FRotator::ZeroRotator;

	// --- Placement ---
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	ECameraPlacementMethod PlacementMethod = ECameraPlacementMethod::Raycast;

	/** Start the down-trace this far above the nominal Z. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	float TraceExtraHeight = 100000.0f;

	/** Trace down this far from the start. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	float TraceDepth = 300000.0f;

	/** Place the camera this far above the ground hit. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	float HeightAboveGround = 250.0f;

	/** Only spawn where the hit is actually ON the NavMesh (rejects roofs inside the volume box). */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	bool bRequireNavmesh = true;

	/** Max vertical gap (UU) allowed between the ground hit and the NavMesh below it. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	float NavmeshToleranceZ = 300.0f;

	/** Ring offsets tried within a cell before snapping to the nearest navmesh point. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	int32 PlacementRetries = 16;

	/** World Outliner folder the spawned cameras are grouped under. */
	UPROPERTY(EditAnywhere, config, Category = "Placement")
	FString OutlinerFolder = TEXT("CameraGrid");

	// --- Density / heat map ---
	/** Bin size (UU) for density clustering = the heat map's coarse LOD. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map")
	float ClusterCellSize = 2000.0f;

	/** A coarse bin needs at least this many assets to count as a cluster. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map")
	int32 MinClusterWeight = 20;

	/** Finest LOD = ClusterCellSize / HeatmapSubdiv (power of 2). 8 = base, /2, /4, /8. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map", meta = (ClampMin = "1"))
	int32 HeatmapSubdiv = 8;

	/** Include instanced-mesh instances when binning density / computing bounds. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map")
	bool bExportInstances = true;

	/** Resolution (px) of the square orthographic top-down render used as the heat-map background. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map", meta = (ClampMin = "256"))
	int32 TopdownPx = 4096;

	/** Exposure compensation (EV stops) for the top-down capture; +1 doubles brightness. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map")
	float TopdownExposureBias = 2.0f;

	/** Localhost port for the heat map's "Go to / inspect cell" bridge. */
	UPROPERTY(EditAnywhere, config, Category = "Heat Map", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 GotoPort = 30080;

	// --- Profiling ---
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	ECameraProfileMode ProfileMode = ECameraProfileMode::PIE;

	/** Ticks to let PIE spin up (streaming/Lumen/TSR) before the first camera. */
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	int32 WarmupTicks = 30;

	/** Ticks after switching view target, before tracing (temporal AA/Lumen convergence). */
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	int32 SettleTicks = 24;

	/** Measure each camera for this many wall-clock seconds to get its average frame time. */
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	float CaptureSeconds = 1.0f;

	/** Unreal Insights channels recorded per .utrace. */
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	FString TraceChannels = TEXT("cpu,gpu,frame,bookmark");

	/** Per-camera screenshot resolution. */
	UPROPERTY(EditAnywhere, config, Category = "Profiling")
	FIntPoint ScreenshotRes = FIntPoint(1920, 1080);
};
