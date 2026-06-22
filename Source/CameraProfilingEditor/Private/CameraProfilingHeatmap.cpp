#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "Camera/CameraActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "WorldPartition/HLOD/HLODActor.h"

#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraProfilingHeatmap, Log, All);

namespace
{
	int32 MeshTris(const UStaticMesh* Mesh)
	{
		if (Mesh)
		{
			const FStaticMeshRenderData* RD = Mesh->GetRenderData();
			if (RD && RD->LODResources.Num() > 0)
			{
				return static_cast<int32>(RD->LODResources[0].GetNumTriangles());
			}
		}
		return 0;
	}

	/** Number of discrete LODs authored on the mesh (Nanite meshes usually report just the fallback). */
	int32 MeshLODCount(const UStaticMesh* Mesh)
	{
		const FStaticMeshRenderData* RD = Mesh ? Mesh->GetRenderData() : nullptr;
		return RD ? RD->LODResources.Num() : 0;
	}

	TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path)
	{
		FString Str;
		TSharedPtr<FJsonObject> Obj;
		if (FFileHelper::LoadFileToString(Str, *Path))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Str), Obj);
		}
		return Obj;
	}
}

bool FCameraProfilingTools::WriteHeatmap(bool bOpenBrowser)
{
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	const FString Dir = DataDir();

	TSharedPtr<FJsonObject> Scene = LoadJsonObject(FPaths::Combine(Dir, TEXT("scene_data.json")));
	if (!Scene.IsValid())
	{
		UE_LOG(LogCameraProfilingHeatmap, Warning, TEXT("[heatmap] scene_data.json missing; run Generate Cameras / Refresh first."));
		return false;
	}

	// Camera markers come ONLY from our generated/folder cameras (never arbitrary scene cameras):
	// prefer the profiling manifest (camera_traces.json -- has screenshots/traces/fps) when it's at
	// least as new as the spawned-grid file; otherwise use camera_grid.json (the ACTUAL GridCam_*
	// cameras in the CameraGrid folder). camera_positions.json (the pre-spawn PLANNED grid, which can
	// include cells that failed to spawn) is only a last-resort fallback when no grid file exists.
	const FString ManifestPath = FPaths::Combine(Dir, TEXT("camera_traces.json"));
	const FString GridPath = FPaths::Combine(Dir, TEXT("camera_grid.json"));
	const FString PositionsPath = FPaths::Combine(Dir, TEXT("camera_positions.json"));
	IFileManager& FM = IFileManager::Get();
	const bool bHaveManifest = FM.FileExists(*ManifestPath);
	const FString GridSource = FM.FileExists(*GridPath) ? GridPath : PositionsPath;
	const bool bHaveGrid = FM.FileExists(*GridSource);
	const bool bUseManifest = bHaveManifest && (!bHaveGrid ||
		FM.GetTimeStamp(*ManifestPath) >= FM.GetTimeStamp(*GridSource));

	TArray<TSharedPtr<FJsonValue>> Cameras;
	if (bUseManifest || bHaveGrid)
	{
		TSharedPtr<FJsonObject> CamObj = LoadJsonObject(bUseManifest ? ManifestPath : GridSource);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (CamObj.IsValid() && CamObj->TryGetArrayField(TEXT("cameras"), Arr) && Arr)
		{
			Cameras = *Arr;
		}
	}

	// Make screenshot/trace paths absolute: they're stored project-relative (../../..), which a
	// file:// page can't resolve -- that's why the screenshot thumbnail wasn't showing, and why a
	// copied trace path wouldn't open in Unreal Insights.
	for (const TSharedPtr<FJsonValue>& CamVal : Cameras)
	{
		const TSharedPtr<FJsonObject> Obj = CamVal->AsObject();
		if (!Obj.IsValid()) { continue; }
		for (const TCHAR* Field : { TEXT("screenshot"), TEXT("trace") })
		{
			FString P;
			if (Obj->TryGetStringField(Field, P) && !P.IsEmpty())
			{
				Obj->SetStringField(Field, FPaths::ConvertRelativePathToFull(P));
			}
		}
	}

	// Optional top-down overlay.
	TSharedPtr<FJsonObject> MapMeta = LoadJsonObject(FPaths::Combine(Dir, TEXT("map_topdown.json")));

	// Assemble the payload the template expects at /*__DATA__*/.
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();

	// Canvas extent = the same bounds the grid/top-down render use (so cells, cameras, and the map
	// overlay all share one coordinate space, per the configured Bounds Source).
	const FBox Extent = FCameraProfilingTools::ResolveExtent(Scene);
	auto BoundsArr = [](double X, double Y, double Z)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		A.Add(MakeShared<FJsonValueNumber>(X));
		A.Add(MakeShared<FJsonValueNumber>(Y));
		A.Add(MakeShared<FJsonValueNumber>(Z));
		return A;
	};
	TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
	BoundsObj->SetArrayField(TEXT("min"), BoundsArr(Extent.Min.X, Extent.Min.Y, Extent.Min.Z));
	BoundsObj->SetArrayField(TEXT("max"), BoundsArr(Extent.Max.X, Extent.Max.Y, Extent.Max.Z));
	Payload->SetObjectField(TEXT("bounds"), BoundsObj);
	Payload->SetField(TEXT("map"), MapMeta.IsValid()
		? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueObject>(MapMeta))
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
	Payload->SetField(TEXT("density"), Scene->HasField(TEXT("density_grid"))
		? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueObject>(Scene->GetObjectField(TEXT("density_grid"))))
		: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));

	const TArray<TSharedPtr<FJsonValue>>* Clusters = nullptr;
	Payload->SetArrayField(TEXT("clusters"), Scene->TryGetArrayField(TEXT("clusters"), Clusters) && Clusters ? *Clusters : TArray<TSharedPtr<FJsonValue>>());
	const TArray<TSharedPtr<FJsonValue>>* NavVols = nullptr;
	Payload->SetArrayField(TEXT("navmesh"), Scene->TryGetArrayField(TEXT("navmesh_volumes"), NavVols) && NavVols ? *NavVols : TArray<TSharedPtr<FJsonValue>>());
	Payload->SetArrayField(TEXT("cameras"), Cameras);
	Payload->SetBoolField(TEXT("have_traces"), bUseManifest);

	TSharedRef<FJsonObject> Opts = MakeShared<FJsonObject>();
	Opts->SetNumberField(TEXT("canvas"), 1000);
	Opts->SetBoolField(TEXT("log"), true);
	Opts->SetBoolField(TEXT("show_cameras"), true);
	Opts->SetBoolField(TEXT("show_navmesh"), true);
	Opts->SetStringField(TEXT("goto"), FString::Printf(TEXT("http://127.0.0.1:%d"), S->GotoPort));
	Payload->SetObjectField(TEXT("opts"), Opts);

	FString PayloadStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadStr);
	FJsonSerializer::Serialize(Payload, Writer);

	// Load the template (shipped under the plugin's Resources/) and inline the data.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CameraProfiling"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogCameraProfilingHeatmap, Warning, TEXT("[heatmap] CameraProfiling plugin not found."));
		return false;
	}
	const FString TemplatePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/heatmap_template.html"));
	FString Html;
	if (!FFileHelper::LoadFileToString(Html, *TemplatePath))
	{
		UE_LOG(LogCameraProfilingHeatmap, Warning, TEXT("[heatmap] template missing: %s"), *TemplatePath);
		return false;
	}
	Html.ReplaceInline(TEXT("/*__DATA__*/"), *PayloadStr, ESearchCase::CaseSensitive);

	const FString OutPath = FPaths::Combine(Dir, TEXT("density_heatmap.html"));
	if (!FFileHelper::SaveStringToFile(Html, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogCameraProfilingHeatmap, Warning, TEXT("[heatmap] failed to write %s"), *OutPath);
		return false;
	}
	UE_LOG(LogCameraProfilingHeatmap, Log, TEXT("[heatmap] wrote %s"), *OutPath);

	if (bOpenBrowser)
	{
		const FString Url = TEXT("file:///") + FPaths::ConvertRelativePathToFull(OutPath).Replace(TEXT("\\"), TEXT("/"));
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
	return true;
}

void FCameraProfilingTools::RefreshHeatmapData()
{
	if (ExportSceneData().IsEmpty())
	{
		return;
	}
	CaptureTopdown();
	WriteHeatmap(/*bOpenBrowser=*/true);
}

FString FCameraProfilingTools::InspectCell(double MinX, double MinY, double Size)
{
	UWorld* World = EditorWorld();
	const double MaxX = MinX + Size, MaxY = MinY + Size;

	struct FAgg { int32 Count = 0; double Triangles = 0.0; int32 Lods = 0; bool bNanite = false; };
	TMap<FString, FAgg> PerMesh;

	if (GEditor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true, /*bWarnAboutManyActors=*/false);
	}

	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsA<ACameraActor>() || Actor->IsA<AWorldPartitionHLOD>())
			{
				continue;
			}
			bool bHitDiscrete = false;

			TInlineComponentArray<UStaticMeshComponent*> Comps;
			Actor->GetComponents(Comps);
			for (UStaticMeshComponent* Comp : Comps)
			{
				UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
				if (!Mesh)
				{
					continue;
				}
				const int32 Tris = MeshTris(Mesh);
				const int32 Lods = MeshLODCount(Mesh);
				const bool bNanite = Mesh->IsNaniteEnabled();
				const FString Name = Mesh->GetName();

				if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Comp))
				{
					// Clear any stale selection from a previous inspect, then select ONLY in-cell instances.
					ISM->ClearInstanceSelection();
					int32 Hits = 0;
					const int32 Count = ISM->GetInstanceCount();
					for (int32 i = 0; i < Count; ++i)
					{
						FTransform T;
						if (ISM->GetInstanceTransform(i, T, /*bWorldSpace=*/true))
						{
							const FVector L = T.GetLocation();
							if (L.X >= MinX && L.X < MaxX && L.Y >= MinY && L.Y < MaxY)
							{
								ISM->SelectInstance(true, i, 1);
								++Hits;
							}
						}
					}
					if (Hits > 0)
					{
						FAgg& A = PerMesh.FindOrAdd(Name);
						A.Count += Hits;
						A.Triangles += static_cast<double>(Hits) * Tris;
						A.Lods = Lods;
						A.bNanite = bNanite;
					}
					ISM->MarkRenderStateDirty();
				}
				else
				{
					const FVector L = Comp->GetComponentLocation();
					if (L.X >= MinX && L.X < MaxX && L.Y >= MinY && L.Y < MaxY)
					{
						FAgg& A = PerMesh.FindOrAdd(Name);
						A.Count += 1;
						A.Triangles += Tris;
						A.Lods = Lods;
						A.bNanite = bNanite;
						bHitDiscrete = true;
					}
				}
			}

			// Only select the actor when it has a DISCRETE placement in the cell -- never the giant
			// shared foliage/instanced actor (that was the over-selection bug).
			if (bHitDiscrete && GEditor)
			{
				GEditor->SelectActor(Actor, /*bSelected=*/true, /*bNotify=*/false);
			}
		}
	}

	if (GEditor)
	{
		GEditor->NoteSelectionChange();
	}

	// Heaviest meshes by triangles, top 12.
	TArray<TPair<FString, FAgg>> Items;
	for (const TPair<FString, FAgg>& Pair : PerMesh)
	{
		Items.Emplace(Pair.Key, Pair.Value);
	}
	Items.Sort([](const TPair<FString, FAgg>& A, const TPair<FString, FAgg>& B) { return A.Value.Triangles > B.Value.Triangles; });

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	W->WriteArrayStart();
	for (int32 i = 0; i < Items.Num() && i < 12; ++i)
	{
		W->WriteObjectStart();
		W->WriteValue(TEXT("mesh"), Items[i].Key);
		W->WriteValue(TEXT("count"), Items[i].Value.Count);
		W->WriteValue(TEXT("triangles"), Items[i].Value.Triangles);
		W->WriteValue(TEXT("lods"), Items[i].Value.Lods);
		W->WriteValue(TEXT("nanite"), Items[i].Value.bNanite);
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();
	W->Close();

	UE_LOG(LogCameraProfilingHeatmap, Log, TEXT("[inspect] cell (%.0f,%.0f)+%.0f: %d mesh type(s) selected."),
		MinX, MinY, Size, Items.Num());
	return Json;
}

void FCameraProfilingTools::GotoCamera(double X, double Y, double Z, double Pitch, double Yaw, double Roll)
{
	// Put the level-editor viewport at the profiled camera's exact transform so you see what it saw.
	if (FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient)
	{
		VC->SetViewLocation(FVector(X, Y, Z));
		VC->SetViewRotation(FRotator(Pitch, Yaw, Roll));
		VC->Invalidate();
	}
}
