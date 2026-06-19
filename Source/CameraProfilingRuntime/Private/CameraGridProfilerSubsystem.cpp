#include "CameraGridProfilerSubsystem.h"

#include "Camera/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HighResScreenshot.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "WorldPartition/WorldPartition.h"

#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraProfile, Log, All);

namespace
{
	/** Screenshot at WxH to an exact path via the high-res path so the resolution setting is honored. */
	void RequestShotAt(const FString& Path, int32 W, int32 H)
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

bool UCameraGridProfilerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only exist when explicitly requested, and only in actual game/PIE worlds.
	if (!FParse::Param(FCommandLine::Get(), TEXT("RunCameraProfile")))
	{
		return false;
	}
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
	}
	return false;
}

void UCameraGridProfilerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	const TCHAR* Cmd = FCommandLine::Get();

	FString JsonPath;
	if (!FParse::Value(Cmd, TEXT("CameraProfileData="), JsonPath) || JsonPath.IsEmpty())
	{
		JsonPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraProfiling/data/camera_grid.json"));
	}

	OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraProfiling"));
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);

	// Load cameras + the shared profiling defaults baked into the JSON...
	if (!LoadCameras(JsonPath))
	{
		UE_LOG(LogCameraProfile, Warning, TEXT("[CameraProfile] no cameras loaded from '%s'; disabled."), *JsonPath);
		return;
	}

	// ...then let explicit launch args override those defaults.
	FParse::Value(Cmd, TEXT("CameraProfileWarmup="), WarmupFrames);
	FParse::Value(Cmd, TEXT("CameraProfileSettle="), SettleFrames);
	FParse::Value(Cmd, TEXT("CameraProfileCaptureSecs="), CaptureSeconds);
	if (FParse::Param(Cmd, TEXT("CameraProfileNoUncap"))) bUncapFrameRate = false;
	bMemReportPerCamera = FParse::Param(Cmd, TEXT("CameraProfileMemReport"));

	bArmed = true;
	WaitFrames = WarmupFrames;
	UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] armed: %d cameras from '%s' (warmup %d frames)."),
		Cameras.Num(), *JsonPath, WarmupFrames);
}

bool UCameraGridProfilerSubsystem::LoadCameras(const FString& JsonPath)
{
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *JsonPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("cameras"), Arr) || Arr == nullptr)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Arr)
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
		if (!Obj->TryGetArrayField(TEXT("location"), Loc) || Loc == nullptr || Loc->Num() < 3)
		{
			continue;
		}

		FProfileCamera Cam;
		Cam.Location = FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber());

		const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
		if (Obj->TryGetArrayField(TEXT("rotation"), Rot) && Rot != nullptr && Rot->Num() >= 3)
		{
			// JSON rotation is [pitch, yaw, roll]; FRotator(Pitch, Yaw, Roll).
			Cam.Rotation = FRotator((*Rot)[0]->AsNumber(), (*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber());
		}

		Cameras.Add(Cam);
	}

	// Shared profiling params (single source = the editor's CONFIG, written by Generate Cameras).
	// These are defaults; launch args still override them afterwards.
	const TSharedPtr<FJsonObject>* Profile = nullptr;
	if (Root->TryGetObjectField(TEXT("profile"), Profile) && Profile && Profile->IsValid())
	{
		int32 IntVal = 0;
		FString StrVal;
		double NumVal = 0.0;
		if ((*Profile)->TryGetNumberField(TEXT("warmup_frames"), IntVal)) WarmupFrames = IntVal;
		if ((*Profile)->TryGetNumberField(TEXT("settle_frames"), IntVal)) SettleFrames = IntVal;
		if ((*Profile)->TryGetNumberField(TEXT("capture_seconds"), NumVal)) CaptureSeconds = float(NumVal);
		if ((*Profile)->TryGetStringField(TEXT("channels"), StrVal)) Channels = StrVal;
	}

	return Cameras.Num() > 0;
}

void UCameraGridProfilerSubsystem::BeginRun()
{
	UWorld* World = GetWorld();

	// Start a trace ourselves unless the user already launched with -trace=...
	const TCHAR* Cmd = FCommandLine::Get();
	const bool bExternalTrace = (FCString::Strifind(Cmd, TEXT("-trace=")) != nullptr) || FParse::Param(Cmd, TEXT("trace"));
	if (!bExternalTrace && GEngine)
	{
		// NOTE: unquoted, forward-slash path (Trace.File treats a quoted path as relative, and a
		// backslash path can be mis-parsed). Assumes no spaces.
		FString TracePath = FPaths::Combine(OutDir, TEXT("profile.utrace"));
		TracePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		GEngine->Exec(World, *FString::Printf(TEXT("Trace.File %s %s"), *TracePath, *Channels));
		bSelfTrace = true;
		UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] started trace -> %s"), *TracePath);
	}

	// Uncap the frame rate so the measured FPS reflects real cost, not a V-Sync / MaxFPS cap.
	if (bUncapFrameRate && GEngine)
	{
		GEngine->Exec(World, TEXT("t.MaxFPS 0"));
		GEngine->Exec(World, TEXT("r.VSync 0"));
	}

	// Spawn the view camera and make it the player's view target.
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	ViewCam = World->SpawnActor<ACameraActor>(Cameras[0].Location, Cameras[0].Rotation, Params);

	// On a World Partition map, make the view camera a streaming source so cells stream in around each
	// profiled camera as it moves (otherwise the trace/screenshot capture unloaded geometry or HLOD).
	if (ViewCam && World->GetWorldPartition())
	{
		UWorldPartitionStreamingSourceComponent* StreamSource =
			NewObject<UWorldPartitionStreamingSourceComponent>(ViewCam, TEXT("CameraProfileStreamingSource"));
		StreamSource->RegisterComponent();
		UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] World Partition detected; view camera registered as a streaming source."));
	}

	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		if (ViewCam)
		{
			PC->SetViewTargetWithBlend(ViewCam, 0.0f);
		}
	}

	Index = 0;
	Phase = 0;
	UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] run started."));
}

void UCameraGridProfilerSubsystem::CaptureCurrent()
{
	UWorld* World = GetWorld();

	TRACE_BOOKMARK(TEXT("camera_%03d"), Index);

	// NOTE: memreport -full stalls the game thread ~2s and would pollute this camera's trace
	// region, so it's OFF per-camera by default (one snapshot is taken at the end instead).
	// Pass -CameraProfileMemReport to force a per-camera dump anyway.
	if (bMemReportPerCamera && GEngine)
	{
		GEngine->Exec(World, TEXT("memreport -full"));
	}

	FString Shot = FPaths::Combine(OutDir, FString::Printf(TEXT("camera_%03d.png"), Index));
	Shot.ReplaceInline(TEXT("\\"), TEXT("/"));
	RequestShotAt(Shot, ScreenshotResX, ScreenshotResY);

	UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] camera %d: bookmark + screenshot."), Index);
}

void UCameraGridProfilerSubsystem::Finish()
{
	if (bSelfTrace && GEngine)
	{
		GEngine->Exec(GetWorld(), TEXT("Trace.Stop"));
	}
	// One memreport AFTER the trace stops, so the ~2s stall doesn't pollute any camera region.
	if (GEngine)
	{
		GEngine->Exec(GetWorld(), TEXT("memreport -full"));
	}
	WriteManifest();
	bArmed = false;
	UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] complete (%d cameras)."), Cameras.Num());
}

void UCameraGridProfilerSubsystem::WriteManifest()
{
	// camera_traces.json: per-camera transform + screenshot + trace + measured ms/fps. The heat
	// map reads this to show each camera's frame rate / screenshot / trace path on click.
	// Only point at profile.utrace if WE created it; with external -trace= that file doesn't exist
	// (the trace went to the trace store), so leave the path blank rather than link a missing file.
	const FString TracePath = bSelfTrace ? FPaths::Combine(OutDir, TEXT("profile.utrace")) : FString();

	TArray<TSharedPtr<FJsonValue>> CamArray;
	for (int32 i = 0; i < Cameras.Num(); ++i)
	{
		const FProfileCamera& C = Cameras[i];
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		Obj->SetStringField(TEXT("trace"), TracePath);
		Obj->SetStringField(TEXT("screenshot"), FPaths::Combine(OutDir, FString::Printf(TEXT("camera_%03d.png"), i)));

		auto NumArr = [](double a, double b, double c)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			Out.Add(MakeShared<FJsonValueNumber>(a));
			Out.Add(MakeShared<FJsonValueNumber>(b));
			Out.Add(MakeShared<FJsonValueNumber>(c));
			return Out;
		};
		Obj->SetArrayField(TEXT("location"), NumArr(C.Location.X, C.Location.Y, C.Location.Z));
		Obj->SetArrayField(TEXT("rotation"), NumArr(C.Rotation.Pitch, C.Rotation.Yaw, C.Rotation.Roll));

		const float Ms = CameraMs.IsValidIndex(i) ? CameraMs[i] : 0.0f;
		Obj->SetNumberField(TEXT("ms"), Ms);
		Obj->SetNumberField(TEXT("fps"), Ms > 0.0f ? 1000.0f / Ms : 0.0f);

		CamArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("cameras"), CamArray);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	const FString OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraProfiling/data/camera_traces.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
	if (FFileHelper::SaveStringToFile(Json, *OutPath))
	{
		UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] wrote manifest (with fps) -> %s"), *OutPath);
	}
	else
	{
		UE_LOG(LogCameraProfile, Warning, TEXT("[CameraProfile] failed to write manifest -> %s"), *OutPath);
	}
}

void UCameraGridProfilerSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bArmed || Cameras.Num() == 0)
	{
		return;
	}
	if (WaitFrames > 0)
	{
		--WaitFrames;
		return;
	}

	if (!bStarted)
	{
		// Wait until a player controller exists before starting.
		if (UGameplayStatics::GetPlayerController(GetWorld(), 0) == nullptr)
		{
			WaitFrames = 5;
			return;
		}
		BeginRun();
		bStarted = true;
		return;
	}

	if (Index >= Cameras.Num())
	{
		Finish();
		return;
	}

	if (Phase == 0)
	{
		if (ViewCam)
		{
			ViewCam->SetActorLocationAndRotation(Cameras[Index].Location, Cameras[Index].Rotation);
		}
		Phase = 1;
		WaitFrames = SettleFrames;
		return;
	}

	if (Phase == 1)
	{
		CaptureCurrent();
		Phase = 2;
		CapTimeAcc = 0.0;
		CapFrameAcc = 0;
		return;
	}

	// Phase 2: measure this camera for ~CaptureSeconds of wall clock, then advance.
	CapTimeAcc += DeltaTime;
	++CapFrameAcc;
	if (CapTimeAcc >= CaptureSeconds)
	{
		const float AvgMs = CapFrameAcc > 0 ? float((CapTimeAcc / CapFrameAcc) * 1000.0) : 0.0f;
		CameraMs.Add(AvgMs);
		UE_LOG(LogCameraProfile, Display, TEXT("[CameraProfile] camera %d: %.2f ms (%.1f fps) over %d frames."),
			Index, AvgMs, AvgMs > 0.f ? 1000.f / AvgMs : 0.f, CapFrameAcc);
		Phase = 0;
		++Index;
		WaitFrames = 1;
	}
}

TStatId UCameraGridProfilerSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCameraGridProfilerSubsystem, STATGROUP_Tickables);
}
