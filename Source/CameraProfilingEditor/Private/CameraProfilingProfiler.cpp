#include "CameraProfilingProfiler.h"
#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "LevelEditorSubsystem.h"
#include "LevelEditorViewport.h"
#include "Camera/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HighResScreenshot.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraProfilingProfiler, Log, All);

namespace
{
	/** Screenshot at WxH to an exact path. Uses the high-res path (honors resolution) when a game
	 *  viewport exists (PIE/standalone); falls back to a viewport-res grab in editor-only mode. */
	void RequestShotAt(UWorld* /*World*/, const FString& Path, int32 W, int32 H)
	{
		FViewport* Viewport = (GEngine && GEngine->GameViewport) ? GEngine->GameViewport->Viewport : nullptr;
		if (Viewport && W > 0 && H > 0)
		{
			FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
			Config.SetResolution(W, H, 1.0f);
			Config.FilenameOverride = Path;
			Viewport->TakeHighResScreenShot();
		}
		else
		{
			FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		}
	}
}

FCameraProfilingProfiler& FCameraProfilingProfiler::Get()
{
	static FCameraProfilingProfiler Instance;
	return Instance;
}

TStatId FCameraProfilingProfiler::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(FCameraProfilingProfiler, STATGROUP_Tickables);
}

void FCameraProfilingProfiler::Start()
{
	if (bArmed)
	{
		UE_LOG(LogCameraProfilingProfiler, Warning, TEXT("[profile] already running; ignoring."));
		return;
	}
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld)
	{
		return;
	}
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();

	// Gather the GridCam_* (folder cameras) transforms from the editor world.
	const FName Folder(*S->OutlinerFolder);
	Targets.Reset();
	for (TActorIterator<ACameraActor> It(EditorWorld); It; ++It)
	{
		if (It->GetFolderPath() == Folder)
		{
			Targets.Add(It->GetActorTransform());
		}
	}
	if (Targets.Num() == 0)
	{
		UE_LOG(LogCameraProfilingProfiler, Warning, TEXT("[profile] no cameras in folder '%s'; run Generate Cameras first."), *S->OutlinerFolder);
		return;
	}

	bPie = (S->ProfileMode == ECameraProfileMode::PIE);
	WarmupFrames = S->WarmupTicks;
	SettleFrames = S->SettleTicks;
	CaptureFrames = FMath::Max(2, FMath::RoundToInt(S->CaptureSeconds * 60.0f));
	Channels = S->TraceChannels;
	ShotX = S->ScreenshotRes.X;
	ShotY = S->ScreenshotRes.Y;
	TraceDir = FPaths::Combine(FCameraProfilingTools::DataDir(), TEXT("traces"));
	ShotDir = FPaths::Combine(FCameraProfilingTools::DataDir(), TEXT("screenshots"));
	IFileManager::Get().MakeDirectory(*TraceDir, true);
	IFileManager::Get().MakeDirectory(*ShotDir, true);

	Index = 0;
	WaitFrames = 0;
	bPieUp = false;
	Manifest.Reset();
	PieCams.Reset();
	LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();

	if (bPie)
	{
		Phase = -1; // wait for PIE
		if (LevelEditor)
		{
			if (LevelEditor->IsInPlayInEditor())
			{
				LevelEditor->EditorRequestEndPlay();
			}
			LevelEditor->EditorRequestBeginPlay();
		}
		World = nullptr;
		UE_LOG(LogCameraProfilingProfiler, Log, TEXT("[profile] PIE-mode: starting Play-In-Editor for %d cameras..."), Targets.Num());
	}
	else
	{
		Phase = 0;
		World = EditorWorld;
		bPieUp = true;
		UE_LOG(LogCameraProfilingProfiler, Log, TEXT("[profile] editor-mode profiling %d cameras (1 .utrace each)."), Targets.Num());
	}

	bArmed = true;
}

void FCameraProfilingProfiler::Tick(float DeltaTime)
{
	if (!bArmed)
	{
		return;
	}
	if (WaitFrames > 0)
	{
		--WaitFrames;
		return;
	}

	// PIE bring-up: wait for the game world + player controller, then warm up.
	if (bPie && !bPieUp)
	{
		if (!LevelEditor || !LevelEditor->IsInPlayInEditor())
		{
			return;
		}
		UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
		if (!PlayWorld)
		{
			return;
		}
		APlayerController* PC = UGameplayStatics::GetPlayerController(PlayWorld, 0);
		if (!PC)
		{
			return;
		}
		World = PlayWorld;
		Pc = PC;
		PieCams.Reset();
		for (TActorIterator<ACameraActor> It(PlayWorld); It; ++It)
		{
			PieCams.Add(*It);
		}
		bPieUp = true;
		Phase = 0;
		WaitFrames = WarmupFrames;
		UE_LOG(LogCameraProfilingProfiler, Log, TEXT("[profile] PIE up: %d camera(s); warming up..."), PieCams.Num());
		return;
	}

	if (Index >= Targets.Num())
	{
		Finish();
		return;
	}

	const FTransform& T = Targets[Index];

	if (Phase == 0)
	{
		Aim(T);
		Phase = 1;
		WaitFrames = SettleFrames;
		return;
	}
	if (Phase == 1)
	{
		Capture(Index);
		Phase = 2;
		WaitFrames = CaptureFrames;
		return;
	}

	// Phase 2: stop the trace, record the manifest entry, advance.
	Exec(TEXT("Trace.Stop"));
	Manifest.Add({ Index,
		FString::Printf(TEXT("%s/camera_%03d.utrace"), *TraceDir, Index),
		FString::Printf(TEXT("%s/camera_%03d.png"), *ShotDir, Index),
		T.GetLocation(), T.Rotator() });
	UE_LOG(LogCameraProfilingProfiler, Log, TEXT("[profile] camera %d: utrace + screenshot + memreport%s"),
		Index, bPie ? TEXT(" (PIE)") : TEXT(""));
	Phase = 0;
	++Index;
	WaitFrames = 1;
}

void FCameraProfilingProfiler::Aim(const FTransform& Target)
{
	if (bPie)
	{
		if (ACameraActor* Cam = NearestPieCamera(Target.GetLocation()))
		{
			if (Pc.IsValid())
			{
				Pc->SetViewTargetWithBlend(Cam, 0.0f);
			}
		}
		else if (Pc.IsValid())
		{
			Pc->SetControlRotation(Target.Rotator()); // fallback: no PIE duplicate found
		}
	}
	else if (FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient)
	{
		VC->SetViewLocation(Target.GetLocation());
		VC->SetViewRotation(Target.Rotator());
		VC->Invalidate();
	}
}

void FCameraProfilingProfiler::Capture(int32 CameraIndex)
{
	// Per-camera trace. NOTE: Trace.File needs an UNQUOTED, forward-slash path (a quoted path is read
	// as relative). Force forward slashes; the engine refuses to overwrite, so delete a stale file first.
	FString TracePath = FString::Printf(TEXT("%s/camera_%03d.utrace"), *TraceDir, CameraIndex);
	TracePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (IFileManager::Get().FileExists(*TracePath))
	{
		IFileManager::Get().Delete(*TracePath);
	}
	Exec(*FString::Printf(TEXT("Trace.File %s %s"), *TracePath, *Channels));
	// NOTE: no memreport here -- it stalls the game thread (~2s) and, run inside the live trace, would
	// corrupt this camera's timing. One snapshot is taken at the end (Finish), with no trace active.

	const FString ShotPath = FString::Printf(TEXT("%s/camera_%03d.png"), *ShotDir, CameraIndex);
	RequestShotAt(World.Get(), ShotPath, ShotX, ShotY);
}

ACameraActor* FCameraProfilingProfiler::NearestPieCamera(const FVector& Location) const
{
	ACameraActor* Best = nullptr;
	double BestD2 = TNumericLimits<double>::Max();
	for (const TWeakObjectPtr<ACameraActor>& Weak : PieCams)
	{
		if (ACameraActor* Cam = Weak.Get())
		{
			const double D2 = FVector::DistSquared(Cam->GetActorLocation(), Location);
			if (D2 < BestD2) { BestD2 = D2; Best = Cam; }
		}
	}
	return Best;
}

void FCameraProfilingProfiler::Exec(const TCHAR* Command)
{
	if (GEngine)
	{
		GEngine->Exec(World.Get(), Command);
	}
}

void FCameraProfilingProfiler::Finish()
{
	Exec(TEXT("Trace.Stop")); // safety: ensure no trace left running
	// One memory snapshot now that no trace is active, so its ~2s stall pollutes nothing.
	Exec(TEXT("memreport -full"));
	if (bPie && LevelEditor && LevelEditor->IsInPlayInEditor())
	{
		LevelEditor->EditorRequestEndPlay();
	}
	bArmed = false;

	// Write camera_traces.json (camera->trace/screenshot map) for the heat map.
	FString Json;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
	W->WriteObjectStart();
	W->WriteArrayStart(TEXT("cameras"));
	for (const FManifestEntry& E : Manifest)
	{
		W->WriteObjectStart();
		W->WriteValue(TEXT("index"), E.Index);
		W->WriteValue(TEXT("trace"), E.Trace);
		W->WriteValue(TEXT("screenshot"), E.Shot);
		W->WriteArrayStart(TEXT("location")); W->WriteValue(E.Loc.X); W->WriteValue(E.Loc.Y); W->WriteValue(E.Loc.Z); W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("rotation")); W->WriteValue(E.Rot.Pitch); W->WriteValue(E.Rot.Yaw); W->WriteValue(E.Rot.Roll); W->WriteArrayEnd();
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();
	W->WriteObjectEnd();
	W->Close();
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(FCameraProfilingTools::DataDir(), TEXT("camera_traces.json")));

	// Rebuild the heat map so camera markers pick up their trace links (don't auto-open).
	FCameraProfilingTools::WriteHeatmap(/*bOpenBrowser=*/false);
	UE_LOG(LogCameraProfilingProfiler, Log, TEXT("[profile] complete: %d cameras. Re-open 'See Density Map' for trace links."), Manifest.Num());
}

// Entry point referenced by the Tools menu.
void FCameraProfilingTools::ProfileFromCameras()
{
	FCameraProfilingProfiler::Get().Start();
}
