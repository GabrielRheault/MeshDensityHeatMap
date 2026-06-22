#pragma once

#include "CoreMinimal.h"

class UWorld;
class FJsonObject;

/**
 * Editor-side operations for the Camera Profiling plugin (C++ port of the old Python tooling).
 * All methods run on the game thread (menu callbacks / HTTP handlers).
 */
class CAMERAPROFILINGEDITOR_API FCameraProfilingTools
{
public:
	/** <ProjectSaved>/CameraProfiling/data (created on demand). */
	static FString DataDir();

	/** "Generate Data" action: re-scan the level (scene_data.json + top-down render), then build the grid
	 *  at [GridX, GridY] (<=0 uses the configured GridResolution) and spawn cameras ONLY if the CameraGrid
	 *  folder is empty -- if cameras already exist they're kept and this just refreshes the data. */
	static void GenerateData(int32 GridX, int32 GridY);

	/** Scan the level -> scene_data.json (bounds, navmesh volumes, density grid w/ true triangles,
	 *  asset clusters). Returns the written path, or empty on failure. */
	static FString ExportSceneData();

	/** Orthographic top-down render -> map_topdown.png (+ map_topdown.json AABB) for the overlay. */
	static bool CaptureTopdown();

	/** scene_data.json -> camera_positions.json (jittered grid over navmesh/scene, aimed at clusters). */
	static FString GenerateCameraGrid(int32 GridX, int32 GridY);

	/** Read camera_positions.json, validate ground/navmesh per point, spawn GridCam_* actors, and
	 *  write camera_grid.json (resolved transforms + profiling params) for the runtime subsystem. */
	static int32 SpawnCameras();

	/** Resolve the ground Z under (X, Y) using the configured placement method. Unset on miss. */
	static TOptional<double> ResolveGroundZ(UWorld* World, double X, double Y, double NominalZ);

	/** The world extent the grid, top-down render, and heat map should all share, per the configured
	 *  Bounds Source (WorldPartition → NavMesh-volume union → Scene content). Reads scene_data.json. */
	static FBox ResolveExtent(const TSharedPtr<FJsonObject>& Scene);

	// --- Phase 2: heat map ---
	/** Build density_heatmap.html from scene_data.json (+ overlay) and optionally open it. */
	static bool WriteHeatmap(bool bOpenBrowser);

	/** Select the static-mesh placements inside the cell [Min, Max] (discrete actors + individual
	 *  instances) and return a JSON per-mesh breakdown (heaviest by triangles first). */
	static FString InspectCell(double MinX, double MinY, double Size);

	/** Move the active level-editor viewport to a profiled camera's exact transform (so you see what
	 *  it saw). Called by the heat map's "Go to this camera in editor" button. */
	static void GotoCamera(double X, double Y, double Z, double Pitch, double Yaw, double Roll);

	// --- Phase 3: profiling ---
	/** Write camera_grid.json from the current cameras, arm the runtime subsystem, and start PIE
	 *  so the unified per-camera profiler runs (trace + screenshot per camera). */
	static void ProfileFromCameras();

private:
	/** The editor world, or nullptr. */
	static UWorld* EditorWorld();
};
