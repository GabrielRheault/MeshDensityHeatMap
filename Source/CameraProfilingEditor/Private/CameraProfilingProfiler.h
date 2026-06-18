#pragma once

#include "CoreMinimal.h"
#include "TickableEditorObject.h"

class ACameraActor;
class APlayerController;
class UWorld;
class ULevelEditorSubsystem;

/**
 * Editor-side per-camera profiler (C++ port of the Python CameraProfiler / PieCameraProfiler).
 *
 * Tick-driven so a frame renders between cameras. Per camera it writes ONE camera_###.utrace
 * (Unreal Insights) + a screenshot + a memreport, then a camera_traces.json manifest the heat map
 * links to. PIE mode profiles via the game render path (shipping-representative); Editor mode moves
 * the level viewport (faster, costlier render path).
 *
 * The standalone/packaged runtime path (UCameraGridProfilerSubsystem, one bookmarked trace) is
 * separate and unchanged -- it serves a different output format.
 */
class FCameraProfilingProfiler : public FTickableEditorObject
{
public:
	static FCameraProfilingProfiler& Get();

	/** Begin profiling the current GridCam_* (no-op if already running or no cameras). */
	void Start();

	// FTickableEditorObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return bArmed; }
	virtual TStatId GetStatId() const override;

private:
	void Aim(const FTransform& Target);
	void Capture(int32 CameraIndex);
	void Finish();
	void Exec(const TCHAR* Command);
	ACameraActor* NearestPieCamera(const FVector& Location) const;

	bool bArmed = false;
	bool bPie = true;        // PIE vs Editor mode (from settings)
	bool bPieUp = false;     // PIE world + player controller are ready
	int32 Index = 0;
	int32 Phase = 0;         // 0 = aim, 1 = capture, 2 = measure window
	int32 WaitFrames = 0;

	int32 WarmupFrames = 30;
	int32 SettleFrames = 24;
	int32 CaptureFrames = 12;
	FString Channels;
	int32 ShotX = 1920, ShotY = 1080;
	FString TraceDir, ShotDir;

	TArray<FTransform> Targets;     // editor GridCam_* transforms (final, ground-resolved)
	TArray<TWeakObjectPtr<ACameraActor>> PieCams;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Pc;
	ULevelEditorSubsystem* LevelEditor = nullptr;

	/** One camera->artifacts record (mirrors the Python manifest), accumulated for camera_traces.json. */
	struct FManifestEntry { int32 Index; FString Trace; FString Shot; FVector Loc; FRotator Rot; };
	TArray<FManifestEntry> Manifest;
};
