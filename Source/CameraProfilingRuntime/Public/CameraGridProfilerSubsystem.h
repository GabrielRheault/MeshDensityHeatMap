#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CameraGridProfilerSubsystem.generated.h"

class ACameraActor;

/** One camera to profile (final, ground-resolved transform from camera_positions.json). */
struct FProfileCamera
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

/**
 * In-game camera-grid profiler. Runs in a STANDALONE / packaged game (no editor, no Python).
 *
 * Enabled only when launched with -RunCameraProfile. Reads camera_grid.json (path from
 * -CameraProfileData=, else <ProjectSaved>/CameraProfiling/data/camera_grid.json), then
 * flies the player view through each camera and, per camera, drops an Unreal Insights bookmark
 * ("camera_###") + a screenshot + a memreport. One continuous trace for the whole pass.
 *
 * Trace: launch with -trace=cpu,gpu,frame,bookmark (streams/records normally). If no -trace is
 * given, the subsystem starts a file trace into <ProjectSaved>/CameraProfiling itself.
 */
UCLASS()
class CAMERAPROFILINGRUNTIME_API UCameraGridProfilerSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem / UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bArmed; }

private:
	bool LoadCameras(const FString& JsonPath);
	void BeginRun();
	void CaptureCurrent();
	void Finish();
	void WriteManifest();

	bool bArmed = false;        // -RunCameraProfile present and cameras loaded
	bool bStarted = false;      // BeginRun has run (PC found, trace started)
	bool bSelfTrace = false;    // we started the trace (no -trace launch arg)
	bool bMemReportPerCamera = false;  // -CameraProfileMemReport: dump memreport each camera (pollutes timing)
	int32 Index = 0;
	int32 Phase = 0;            // 0 = aim, 1 = capture, 2 = measure window
	int32 WaitFrames = 0;

	// Tunables (overridable via launch args / camera_grid.json "profile" block).
	int32 WarmupFrames = 30;    // let streaming/Lumen/TSR settle before the first camera
	int32 SettleFrames = 24;    // after switching view, before bookmarking
	float CaptureSeconds = 1.0f; // measure each camera for ~this long (wall clock) to get FPS
	bool bUncapFrameRate = true; // disable V-Sync / MaxFPS so the measured FPS is real, not a cap
	int32 ScreenshotResX = 1920;
	int32 ScreenshotResY = 1080;
	FString Channels = TEXT("cpu,gpu,frame,bookmark");

	// Per-camera measurement: accumulate frame time over the capture window.
	double CapTimeAcc = 0.0;
	int32 CapFrameAcc = 0;
	TArray<float> CameraMs;     // avg frame time (ms) per profiled camera (parallel to Cameras)

	TArray<FProfileCamera> Cameras;
	FString OutDir;

	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> ViewCam = nullptr;
};
