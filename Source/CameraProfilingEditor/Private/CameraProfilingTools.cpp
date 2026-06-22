#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ImageUtils.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UnrealClient.h"

#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "AI/Navigation/NavigationTypes.h"

#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/HLOD/HLODActor.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraProfilingEditor, Log, All);

namespace
{
	/** Per-fine-cell accumulator: instance count, total triangles, draw-call estimate, light-overlap cost. */
	struct FDensityBin
	{
		int32 Instances = 0;
		double Triangles = 0.0;
		int32 Draws = 0;
		double Lights = 0.0;  // sum of weighted dynamic-light footprints covering this cell (overdraw cost)
	};

	/** Coarse clustering accumulator: centroid sums + count. */
	struct FClusterBin
	{
		double SumX = 0.0, SumY = 0.0, SumZ = 0.0;
		int32 Count = 0;
	};

	FIntPoint CellKey(double X, double Y, double Cell)
	{
		return FIntPoint(FMath::FloorToInt(X / Cell), FMath::FloorToInt(Y / Cell));
	}

	/** Triangles of a static mesh's LOD0 from its render data (true count, unlike the Python vertex fallback). */
	int32 MeshTriangles(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return 0;
		}
		const FStaticMeshRenderData* RD = Mesh->GetRenderData();
		if (RD && RD->LODResources.Num() > 0)
		{
			return static_cast<int32>(RD->LODResources[0].GetNumTriangles());
		}
		return 0;
	}

	/** Draw-call estimate = material-slot (section) count, matching the old Python metric. */
	int32 MeshDraws(const UStaticMesh* Mesh)
	{
		return Mesh ? Mesh->GetStaticMaterials().Num() : 0;
	}

	UNavigationSystemV1* NavSystem(UWorld* World)
	{
		return World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	}

	/** Project (x,y,z) onto the NavMesh within Extent; returns true + the projected Z. */
	bool ProjectNav(UWorld* World, const FVector& Point, const FVector& Extent, double& OutZ)
	{
		UNavigationSystemV1* Nav = NavSystem(World);
		if (!Nav)
		{
			return false;
		}
		FNavLocation Out;
		if (Nav->ProjectPointToNavigation(Point, Out, Extent))
		{
			OutZ = Out.Location.Z;
			return true;
		}
		return false;
	}

	/** True if (x,y,z) sits on the NavMesh within ToleranceZ. Fails OPEN when there's no nav system. */
	bool OnNavmesh(UWorld* World, double X, double Y, double Z, double ToleranceZ)
	{
		if (!NavSystem(World))
		{
			return true; // nothing to validate against
		}
		double Unused;
		return ProjectNav(World, FVector(X, Y, Z), FVector(50.0, 50.0, ToleranceZ), Unused);
	}
}

UWorld* FCameraProfilingTools::EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString FCameraProfilingTools::DataDir()
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraProfiling/data"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	return Dir;
}

FString FCameraProfilingTools::HistoryRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraProfiling/history"));
}

namespace
{
	// The files a "generation" produces; archived together and restored together.
	const TCHAR* GGenerationFiles[] = {
		TEXT("scene_data.json"), TEXT("camera_positions.json"), TEXT("camera_grid.json"),
		TEXT("map_topdown.png"), TEXT("map_topdown.json")
	};
}

void FCameraProfilingTools::SnapshotGeneration()
{
	IFileManager& FM = IFileManager::Get();
	const FString Src = DataDir();
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H-%M-%S"));
	const FString Dest = FPaths::Combine(HistoryRoot(), Stamp);
	FM.MakeDirectory(*Dest, /*Tree=*/true);

	int32 Copied = 0;
	for (const TCHAR* F : GGenerationFiles)
	{
		const FString S = FPaths::Combine(Src, F);
		if (FM.FileExists(*S) && FM.Copy(*FPaths::Combine(Dest, F), *S) == COPY_OK) { ++Copied; }
	}
	UE_LOG(LogCameraProfilingEditor, Log, TEXT("[history] archived generation '%s' (%d files) -> %s"),
		*Stamp, Copied, *Dest);
}

TArray<FString> FCameraProfilingTools::ListSnapshots()
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *FPaths::Combine(HistoryRoot(), TEXT("*")), /*Files=*/false, /*Directories=*/true);
	Dirs.Sort([](const FString& A, const FString& B) { return A > B; }); // timestamp names sort newest-first
	return Dirs;
}

bool FCameraProfilingTools::LoadSnapshot(const FString& Name, bool bOpenBrowser)
{
	IFileManager& FM = IFileManager::Get();
	// Guard against path traversal from the (file://) page: accept a plain folder name only.
	if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\")) || Name.Contains(TEXT("..")))
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[history] rejected snapshot name '%s'."), *Name);
		return false;
	}
	const FString Src = FPaths::Combine(HistoryRoot(), Name);
	if (!FM.DirectoryExists(*Src))
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[history] snapshot '%s' not found."), *Name);
		return false;
	}
	const FString Dst = DataDir();
	int32 Copied = 0;
	for (const TCHAR* F : GGenerationFiles)
	{
		const FString S = FPaths::Combine(Src, F);
		if (FM.FileExists(*S) && FM.Copy(*FPaths::Combine(Dst, F), *S) == COPY_OK) { ++Copied; }
	}
	UE_LOG(LogCameraProfilingEditor, Log, TEXT("[history] restored generation '%s' (%d files); rebuilding heat map."),
		*Name, Copied);
	return WriteHeatmap(bOpenBrowser);
}

TOptional<double> FCameraProfilingTools::ResolveGroundZ(UWorld* World, double X, double Y, double NominalZ)
{
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	if (!World)
	{
		return TOptional<double>();
	}

	// NavMesh bounds mode: project onto the navmesh first, fall back to raycast. Other bounds sources
	// never consult the navmesh.
	if (S->BoundsSource == ECameraBoundsSource::NavMesh)
	{
		double Z;
		if (ProjectNav(World, FVector(X, Y, NominalZ), FVector(200.0, 200.0, 100000.0), Z))
		{
			return Z;
		}
	}

	const FVector Start(X, Y, NominalZ + S->TraceExtraHeight);
	const FVector End(X, Y, NominalZ + S->TraceExtraHeight - S->TraceDepth);

	// 1) Visibility channel (complex trace).
	FHitResult Hit;
	FCollisionQueryParams Params(FName(TEXT("CamProfileGround")), /*bTraceComplex=*/true);
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		return Hit.ImpactPoint.Z;
	}

	// 2) Object trace against WorldStatic + WorldDynamic.
	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams ObjQuery(FName(TEXT("CamProfileGroundObj")), /*bTraceComplex=*/false);
	if (World->LineTraceSingleByObjectType(Hit, Start, End, ObjParams, ObjQuery))
	{
		return Hit.ImpactPoint.Z;
	}

	return TOptional<double>();
}

FString FCameraProfilingTools::ExportSceneData()
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[export] no editor world."));
		return FString();
	}
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();

	const double CoarseCell = FMath::Max(1.0f, S->ClusterCellSize);
	const int32 Subdiv = FMath::Max(1, S->HeatmapSubdiv);
	const double FineCell = CoarseCell / Subdiv;
	const bool bExpand = S->bExportInstances;

	TMap<FIntPoint, FDensityBin> FineBins;
	TMap<FIntPoint, FClusterBin> CoarseBins;
	TMap<int32, int64> ZHist;  // Z cell -> placement count, for percentile-trimming the Z extent too
	double MinB[3] = { TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max() };
	double MaxB[3] = { TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest() };
	int32 PointCount = 0;
	int32 ActorCount = 0;

	auto AddPoint = [&](double X, double Y, double Z, int32 Tris, int32 Draws)
	{
		++PointCount;
		const double V[3] = { X, Y, Z };
		for (int32 i = 0; i < 3; ++i)
		{
			MinB[i] = FMath::Min(MinB[i], V[i]);
			MaxB[i] = FMath::Max(MaxB[i], V[i]);
		}
		FDensityBin& FB = FineBins.FindOrAdd(CellKey(X, Y, FineCell));
		FB.Instances += 1;
		FB.Triangles += Tris;
		FB.Draws += Draws;

		FClusterBin& CB = CoarseBins.FindOrAdd(CellKey(X, Y, CoarseCell));
		CB.SumX += X; CB.SumY += Y; CB.SumZ += Z; CB.Count += 1;

		ZHist.FindOrAdd(FMath::FloorToInt(Z / FineCell)) += 1;
	};

	// Add draw-call weight without an instance/triangle (instanced comps batch into ONE section set).
	auto AddDraws = [&](double X, double Y, int32 Draws)
	{
		if (Draws <= 0)
		{
			return;
		}
		FineBins.FindOrAdd(CellKey(X, Y, FineCell)).Draws += Draws;
	};

	TArray<TPair<FVector, FVector>> NavVolumes; // (min, max)

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		// Skip our cameras and World Partition HLOD proxies (the latter would double-count distant
		// cells' simplified meshes on top of the real geometry).
		if (!Actor || Actor->IsA<ACameraActor>() || Actor->IsA<AWorldPartitionHLOD>()
			|| Actor->GetActorLabel().StartsWith(TEXT("GridCam_")))
		{
			continue;
		}
		++ActorCount;

		TInlineComponentArray<UStaticMeshComponent*> Comps;
		Actor->GetComponents(Comps);
		for (UStaticMeshComponent* Comp : Comps)
		{
			UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			const int32 Tris = MeshTriangles(Mesh);
			const int32 Draws = MeshDraws(Mesh);

			if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Comp))
			{
				const int32 Count = ISM->GetInstanceCount();
				if (bExpand && Count > 0)
				{
					for (int32 i = 0; i < Count; ++i)
					{
						FTransform T;
						if (ISM->GetInstanceTransform(i, T, /*bWorldSpace=*/true))
						{
							const FVector L = T.GetLocation();
							AddPoint(L.X, L.Y, L.Z, Tris, 0); // geometry weight per instance
						}
					}
					const FVector C = ISM->GetComponentLocation();
					AddDraws(C.X, C.Y, Draws); // instanced draws batch -> sections once
				}
				else
				{
					const FVector C = ISM->GetComponentLocation();
					AddPoint(C.X, C.Y, C.Z, Tris, Draws);
				}
			}
			else
			{
				const FVector C = Comp->GetComponentLocation();
				AddPoint(C.X, C.Y, C.Z, Tris, Draws); // one placement per mesh component
			}
		}

		if (Actor->IsA<ANavMeshBoundsVolume>())
		{
			FVector Origin, Extent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
			NavVolumes.Emplace(Origin - Extent, Origin + Extent);
		}
	}

	// --- light cost pass ---
	// Stamp each DYNAMIC local light's attenuation footprint (XY circle) into the fine grid so
	// overlapping lights pile up: a cell lit by N lights costs ~N (overdraw), which is the real
	// lighting expense. Static (baked) lights are skipped; Movable counts full, Stationary half;
	// shadow-casters cost more (a point light's shadow is a cubemap ~ 6x a spot/rect's single map).
	// Stamping is clamped to the mesh-content bounds so one large-radius light can't balloon the grid
	// (and scene_data.json) with hundreds of thousands of cells far outside the visible map.
	const int32 BIxMin = FMath::FloorToInt(MinB[0] / FineCell), BIxMax = FMath::FloorToInt(MaxB[0] / FineCell);
	const int32 BIyMin = FMath::FloorToInt(MinB[1] / FineCell), BIyMax = FMath::FloorToInt(MaxB[1] / FineCell);
	int32 LightCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		TInlineComponentArray<ULightComponent*> LightComps;
		Actor->GetComponents(LightComps);
		for (ULightComponent* LC : LightComps)
		{
			ULocalLightComponent* Local = Cast<ULocalLightComponent>(LC); // point/spot/rect (not directional)
			if (!Local || !Local->bAffectsWorld)
			{
				continue;
			}
			double Weight;
			switch (Local->Mobility)
			{
			case EComponentMobility::Movable:    Weight = 1.0; break;
			case EComponentMobility::Stationary: Weight = 0.5; break;
			default:                             continue; // Static -> baked, skip
			}
			if (Local->CastShadows)
			{
				Weight *= (Local->GetLightType() == LightType_Point) ? 6.0 : 2.0;
			}
			const double R = Local->AttenuationRadius;
			if (R <= 0.0 || Weight <= 0.0)
			{
				continue;
			}
			++LightCount;
			const FVector P = Local->GetComponentLocation();
			const double R2 = R * R;
			const int32 IxMin = FMath::Max(BIxMin, FMath::FloorToInt((P.X - R) / FineCell));
			const int32 IxMax = FMath::Min(BIxMax, FMath::FloorToInt((P.X + R) / FineCell));
			const int32 IyMin = FMath::Max(BIyMin, FMath::FloorToInt((P.Y - R) / FineCell));
			const int32 IyMax = FMath::Min(BIyMax, FMath::FloorToInt((P.Y + R) / FineCell));
			for (int32 ix = IxMin; ix <= IxMax; ++ix)
			{
				for (int32 iy = IyMin; iy <= IyMax; ++iy)
				{
					const double Dx = (ix + 0.5) * FineCell - P.X, Dy = (iy + 0.5) * FineCell - P.Y;
					if (Dx * Dx + Dy * Dy <= R2)
					{
						FineBins.FindOrAdd(FIntPoint(ix, iy)).Lights += Weight;
					}
				}
			}
		}
	}

	if (PointCount == 0)
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[export] no mesh placements found in the level."));
		for (int32 i = 0; i < 3; ++i) { MinB[i] = 0.0; MaxB[i] = 0.0; }
	}

	// Dense coarse bins -> cluster centers.
	struct FCluster { FVector Center; int32 Weight; };
	TArray<FCluster> Clusters;
	for (const TPair<FIntPoint, FClusterBin>& Pair : CoarseBins)
	{
		const FClusterBin& B = Pair.Value;
		if (B.Count >= S->MinClusterWeight)
		{
			Clusters.Add({ FVector(B.SumX / B.Count, B.SumY / B.Count, B.SumZ / B.Count), B.Count });
		}
	}
	Clusters.Sort([](const FCluster& A, const FCluster& B) { return A.Weight > B.Weight; });

	// World Partition extent (from actor descriptors -> covers ALL cells, even unloaded). Lets the
	// grid use the full authored world as its extent when there's no NavMeshBoundsVolume.
	FBox WPBounds(ForceInit);
	if (UWorldPartition* WP = World->GetWorldPartition())
	{
		WPBounds = WP->GetEditorWorldBounds();
	}
	// GetEditorWorldBounds can return the uninitialized "infinite" box (±4.4e12) on some maps; reject
	// anything absurdly large (> ~1000 km) so the WorldPartition source falls back to content bounds.
	const bool bHasWP = WPBounds.IsValid != 0 && WPBounds.GetSize().GetAbsMax() < 1.0e8;

	// Percentile-trimmed content extent (X/Y): the box holding the bulk (~98%) of the instances, so a
	// few far-flung outlier placements can't balloon the bounds and leave the map / render mostly empty.
	double ContentMin[3] = { MinB[0], MinB[1], MinB[2] };
	double ContentMax[3] = { MaxB[0], MaxB[1], MaxB[2] };
	if (FineBins.Num() > 0)
	{
		TMap<int32, int64> XW, YW;
		for (const TPair<FIntPoint, FDensityBin>& P : FineBins)
		{
			XW.FindOrAdd(P.Key.X) += P.Value.Instances;
			YW.FindOrAdd(P.Key.Y) += P.Value.Instances;
		}
		auto AxisRange = [](TMap<int32, int64>& W, double Cell, double& OutMin, double& OutMax)
		{
			TArray<int32> Keys; W.GetKeys(Keys); Keys.Sort();
			int64 Total = 0; for (int32 K : Keys) { Total += W[K]; }
			if (Total <= 0) { return; }
			const int64 LoTarget = static_cast<int64>(Total * 0.01);
			const int64 HiTarget = static_cast<int64>(Total * 0.99);
			int64 Cum = 0; int32 LoK = Keys[0], HiK = Keys.Last(); bool bGotLo = false;
			for (int32 K : Keys)
			{
				Cum += W[K];
				if (!bGotLo && Cum >= LoTarget) { LoK = K; bGotLo = true; }
				if (Cum >= HiTarget) { HiK = K; break; }
			}
			OutMin = LoK * Cell;
			OutMax = (HiK + 1) * Cell;
		};
		AxisRange(XW, FineCell, ContentMin[0], ContentMax[0]);
		AxisRange(YW, FineCell, ContentMin[1], ContentMax[1]);
		AxisRange(ZHist, FineCell, ContentMin[2], ContentMax[2]);  // trims high-Z outliers too
	}

	// --- serialize scene_data.json ---
	FString Json;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
	W->WriteObjectStart();
	W->WriteValue(TEXT("actor_count"), ActorCount);
	W->WriteValue(TEXT("position_count"), PointCount);

	W->WriteObjectStart(TEXT("bounds"));
	W->WriteArrayStart(TEXT("min"));
	for (double M : MinB) { W->WriteValue(M); }
	W->WriteArrayEnd();
	W->WriteArrayStart(TEXT("max"));
	for (double M : MaxB) { W->WriteValue(M); }
	W->WriteArrayEnd();
	W->WriteObjectEnd();

	if (bHasWP)
	{
		W->WriteObjectStart(TEXT("world_partition_bounds"));
		W->WriteArrayStart(TEXT("min")); W->WriteValue(WPBounds.Min.X); W->WriteValue(WPBounds.Min.Y); W->WriteValue(WPBounds.Min.Z); W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("max")); W->WriteValue(WPBounds.Max.X); W->WriteValue(WPBounds.Max.Y); W->WriteValue(WPBounds.Max.Z); W->WriteArrayEnd();
		W->WriteObjectEnd();
	}
	else
	{
		W->WriteNull(TEXT("world_partition_bounds"));
	}

	// Tight content extent (frames the bulk of the geometry; used by Bounds Source = Scene).
	W->WriteObjectStart(TEXT("content_bounds"));
	W->WriteArrayStart(TEXT("min")); W->WriteValue(ContentMin[0]); W->WriteValue(ContentMin[1]); W->WriteValue(ContentMin[2]); W->WriteArrayEnd();
	W->WriteArrayStart(TEXT("max")); W->WriteValue(ContentMax[0]); W->WriteValue(ContentMax[1]); W->WriteValue(ContentMax[2]); W->WriteArrayEnd();
	W->WriteObjectEnd();

	W->WriteArrayStart(TEXT("navmesh_volumes"));
	for (const TPair<FVector, FVector>& V : NavVolumes)
	{
		W->WriteObjectStart();
		W->WriteArrayStart(TEXT("min"));
		W->WriteValue(V.Key.X); W->WriteValue(V.Key.Y); W->WriteValue(V.Key.Z);
		W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("max"));
		W->WriteValue(V.Value.X); W->WriteValue(V.Value.Y); W->WriteValue(V.Value.Z);
		W->WriteArrayEnd();
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();

	W->WriteArrayStart(TEXT("clusters"));
	for (const FCluster& C : Clusters)
	{
		W->WriteObjectStart();
		W->WriteArrayStart(TEXT("center"));
		W->WriteValue(C.Center.X); W->WriteValue(C.Center.Y); W->WriteValue(C.Center.Z);
		W->WriteArrayEnd();
		W->WriteValue(TEXT("weight"), C.Weight);
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();

	W->WriteObjectStart(TEXT("density_grid"));
	W->WriteValue(TEXT("cell"), FineCell);
	W->WriteValue(TEXT("base_cell"), CoarseCell);
	W->WriteArrayStart(TEXT("metrics"));
	W->WriteValue(TEXT("instances")); W->WriteValue(TEXT("triangles")); W->WriteValue(TEXT("draws")); W->WriteValue(TEXT("lights"));
	W->WriteArrayEnd();
	W->WriteArrayStart(TEXT("bins"));
	for (const TPair<FIntPoint, FDensityBin>& Pair : FineBins)
	{
		const FDensityBin& B = Pair.Value;
		W->WriteArrayStart();
		W->WriteValue(Pair.Key.X);
		W->WriteValue(Pair.Key.Y);
		W->WriteValue(B.Instances);
		W->WriteValue(B.Triangles);
		W->WriteValue(B.Draws);
		W->WriteValue(B.Lights);
		W->WriteArrayEnd();
	}
	W->WriteArrayEnd();
	W->WriteObjectEnd();

	W->WriteArrayStart(TEXT("positions")); // empty: positions aren't dumped (bounds derived above)
	W->WriteArrayEnd();
	W->WriteObjectEnd();
	W->Close();

	const FString OutPath = FPaths::Combine(DataDir(), TEXT("scene_data.json"));
	if (!FFileHelper::SaveStringToFile(Json, *OutPath))
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[export] failed to write %s"), *OutPath);
		return FString();
	}

	double TotalTris = 0.0; int64 TotalDraws = 0;
	for (const TPair<FIntPoint, FDensityBin>& Pair : FineBins) { TotalTris += Pair.Value.Triangles; TotalDraws += Pair.Value.Draws; }
	UE_LOG(LogCameraProfilingEditor, Log,
		TEXT("[export] %d actors, %d placements, %d clusters, %d navmesh volume(s), %d dynamic light(s); triangles=%.0f draws=%lld -> %s"),
		ActorCount, PointCount, Clusters.Num(), NavVolumes.Num(), LightCount, TotalTris, TotalDraws, *OutPath);
	return OutPath;
}

FBox FCameraProfilingTools::ResolveExtent(const TSharedPtr<FJsonObject>& Scene)
{
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	auto ReadBox = [](const TSharedPtr<FJsonObject>& Obj) -> FBox
	{
		const TArray<TSharedPtr<FJsonValue>> Mn = Obj->GetArrayField(TEXT("min"));
		const TArray<TSharedPtr<FJsonValue>> Mx = Obj->GetArrayField(TEXT("max"));
		return FBox(FVector(Mn[0]->AsNumber(), Mn[1]->AsNumber(), Mn[2]->AsNumber()),
		            FVector(Mx[0]->AsNumber(), Mx[1]->AsNumber(), Mx[2]->AsNumber()));
	};

	// World Partition extent (matches the grid when Bounds Source = WorldPartition).
	if (S->BoundsSource == ECameraBoundsSource::WorldPartition)
	{
		const TSharedPtr<FJsonObject>* WP = nullptr;
		if (Scene->TryGetObjectField(TEXT("world_partition_bounds"), WP) && WP && WP->IsValid())
		{
			return ReadBox(*WP);
		}
	}

	// NavMesh-volume union.
	if (S->BoundsSource == ECameraBoundsSource::NavMesh)
	{
		const TArray<TSharedPtr<FJsonValue>>* Vols = nullptr;
		if (Scene->TryGetArrayField(TEXT("navmesh_volumes"), Vols) && Vols && Vols->Num() > 0)
		{
			FBox Box(ForceInit);
			for (const TSharedPtr<FJsonValue>& V : *Vols)
			{
				Box += ReadBox(V->AsObject());
			}
			return Box;
		}
	}

	// Scene (default): percentile-trimmed content bounds (frames the bulk of geometry, ignoring
	// far-flung outliers), falling back to raw bounds.
	const TSharedPtr<FJsonObject>* Content = nullptr;
	if (Scene->TryGetObjectField(TEXT("content_bounds"), Content) && Content && Content->IsValid())
	{
		return ReadBox(*Content);
	}
	return ReadBox(Scene->GetObjectField(TEXT("bounds")));
}

bool FCameraProfilingTools::CaptureTopdown()
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		return false;
	}
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();

	// Read bounds from scene_data.json (export must have run).
	const FString ScenePath = FPaths::Combine(DataDir(), TEXT("scene_data.json"));
	FString SceneStr;
	TSharedPtr<FJsonObject> Scene;
	if (!FFileHelper::LoadFileToString(SceneStr, *ScenePath) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SceneStr), Scene) || !Scene.IsValid())
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[topdown] no scene_data.json; run export first."));
		return false;
	}
	// Capture over the same extent the grid uses (so the render lines up with the cells / WP bound).
	const FBox Extent = ResolveExtent(Scene);
	const double MinX = Extent.Min.X, MinY = Extent.Min.Y;
	const double MaxX = Extent.Max.X, MaxY = Extent.Max.Y, MaxZ = Extent.Max.Z;

	const double CX = (MinX + MaxX) * 0.5, CY = (MinY + MaxY) * 0.5;
	// Margin > 1 zooms the capture out so it covers a bit beyond the (percentile-trimmed) content box.
	const double Span = FMath::Max3(1.0, MaxX - MinX, MaxY - MinY) * FMath::Max(1.0f, S->TopdownMargin);
	const double Half = Span * 0.5;
	const int32 Px = FMath::Max(256, S->TopdownPx);

	// sRGB target so the exported PNG is in display space (a plain RGBA8 target stores linear base
	// color, which the editor previews correctly but exports dark — the cause of the "black" map).
	UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(World, Px, Px, RTF_RGBA8_SRGB);
	if (!RT)
	{
		return false;
	}

	// Find-or-spawn a PERSISTENT (per-session) top-down capture actor so you can select / pilot it in
	// the editor to see where it's pointed and whether geometry is under it. Spawned RF_Transient so
	// it's visible & pilotable but never saved into the level; reused (repositioned) on each refresh.
	const FString CaptureLabel = TEXT("CameraProfiling_TopDownCapture");
	ASceneCapture2D* Cap = nullptr;
	for (TActorIterator<ASceneCapture2D> It(World); It; ++It)
	{
		if (It->GetActorLabel() == CaptureLabel) { Cap = *It; break; }
	}
	if (!Cap)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		Cap = World->SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (Cap)
		{
			Cap->SetActorLabel(CaptureLabel);
			Cap->SetFolderPath(TEXT("CameraProfiling"));
		}
	}
	if (!Cap)
	{
		return false;
	}
	// pitch -90 (straight down), yaw 90 (orient so the PNG lines up with the heat-map grid). Keep the
	// camera fairly LOW over the geometry: far up, foliage/instanced meshes distance-cull (and the
	// ortho far plane can clip), so the capture sees only the bright sky. 200m clears typical trees.
	const FVector CapLoc(CX, CY, MaxZ + 20000.0);
	Cap->SetActorLocationAndRotation(CapLoc, FRotator(-90.0, 90.0, 0.0));

	bool bOk = false;
	if (USceneCaptureComponent2D* Comp = Cap->GetCaptureComponent2D())
	{
		Comp->bCaptureEveryFrame = false;   // persistent actor: only capture when we ask
		Comp->bCaptureOnMovement = false;
		Comp->ProjectionType = ECameraProjectionMode::Orthographic;
		Comp->OrthoWidth = Span;
		// Unlit base color (GBuffer albedo): the sky/atmosphere isn't written to the base-color buffer,
		// so it CANNOT wash the shot white — empty pixels come out black and geometry shows its flat
		// color. This is the "just show me a map" render, independent of the scene's lighting.
		Comp->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
		Comp->TextureTarget = RT;
		// Force full draw distance so distant foliage/instanced meshes still render into the capture,
		// then restore the project's previous value. (Distance culling can empty a high top-down shot.)
		IConsoleVariable* ViewDistCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ViewDistanceScale"));
		const float PrevViewDist = ViewDistCVar ? ViewDistCVar->GetFloat() : 1.0f;
		if (ViewDistCVar) { ViewDistCVar->Set(50.0f); }
		Comp->CaptureScene();
		if (ViewDistCVar) { ViewDistCVar->Set(PrevViewDist); }

		// Read the RT back and write the PNG ourselves, FORCING alpha opaque. The base-color capture
		// leaves alpha=0; ExportRenderTarget keeps it, so a browser draws the (correct RGB) map as
		// fully transparent — the real cause of the "black map background".
		if (FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource())
		{
			TArray<FColor> Pixels;
			if (RTRes->ReadPixels(Pixels))
			{
				for (FColor& Pixel : Pixels) { Pixel.A = 255; }
				TArray64<uint8> PngData;
				FImageUtils::PNGCompressImageArray(Px, Px, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), PngData);
				FFileHelper::SaveArrayToFile(PngData, *FPaths::Combine(DataDir(), TEXT("map_topdown.png")));
			}
		}

		FString MapJson;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&MapJson);
		W->WriteObjectStart();
		W->WriteValue(TEXT("image"), TEXT("map_topdown.png"));
		W->WriteArrayStart(TEXT("min")); W->WriteValue(CX - Half); W->WriteValue(CY - Half); W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("max")); W->WriteValue(CX + Half); W->WriteValue(CY + Half); W->WriteArrayEnd();
		W->WriteObjectEnd();
		W->Close();
		FFileHelper::SaveStringToFile(MapJson, *FPaths::Combine(DataDir(), TEXT("map_topdown.json")));

		bOk = true;
		UE_LOG(LogCameraProfilingEditor, Log,
			TEXT("[topdown] wrote map_topdown.png: %dpx, center=(%.0f, %.0f), span=%.0fuu, camZ=%.0f. ")
			TEXT("Capture actor '%s' left in level (folder 'CameraProfiling') — select it / pilot it (Ctrl+Shift+P) to inspect."),
			Px, CX, CY, Span, CapLoc.Z, *CaptureLabel);
	}

	return bOk;
}

FString FCameraProfilingTools::GenerateCameraGrid(int32 GridX, int32 GridY)
{
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	const FString ScenePath = FPaths::Combine(DataDir(), TEXT("scene_data.json"));

	FString SceneStr;
	TSharedPtr<FJsonObject> Scene;
	if (!FFileHelper::LoadFileToString(SceneStr, *ScenePath) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SceneStr), Scene) || !Scene.IsValid())
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[grid] scene_data.json missing; run Generate Cameras."));
		return FString();
	}

	// Extent shared with the top-down render + heat map (resolved per Bounds Source). Volumes are still
	// read here for the on-navmesh point filter below.
	const TArray<TSharedPtr<FJsonValue>>* Volumes = nullptr;
	const bool bHaveVolumes = Scene->TryGetArrayField(TEXT("navmesh_volumes"), Volumes) && Volumes && Volumes->Num() > 0;
	const bool bUseNav = (S->BoundsSource == ECameraBoundsSource::NavMesh) && bHaveVolumes;
	const TSharedPtr<FJsonObject>* WPCheck = nullptr;
	const bool bHaveWP = Scene->TryGetObjectField(TEXT("world_partition_bounds"), WPCheck) && WPCheck && WPCheck->IsValid();

	const FBox Extent = ResolveExtent(Scene);
	double BMin[3] = { Extent.Min.X, Extent.Min.Y, Extent.Min.Z };
	double BMax[3] = { Extent.Max.X, Extent.Max.Y, Extent.Max.Z };
	const TCHAR* SourceName =
		(S->BoundsSource == ECameraBoundsSource::WorldPartition && bHaveWP) ? TEXT("worldpartition")
		: bUseNav ? TEXT("navmesh") : TEXT("scene");

	auto InsideNavmesh = [&](double Px, double Py) -> bool
	{
		if (!Volumes) { return false; }
		for (const TSharedPtr<FJsonValue>& V : *Volumes)
		{
			const TSharedPtr<FJsonObject> Obj = V->AsObject();
			const TArray<TSharedPtr<FJsonValue>> VMin = Obj->GetArrayField(TEXT("min"));
			const TArray<TSharedPtr<FJsonValue>> VMax = Obj->GetArrayField(TEXT("max"));
			if (VMin[0]->AsNumber() <= Px && Px <= VMax[0]->AsNumber() &&
				VMin[1]->AsNumber() <= Py && Py <= VMax[1]->AsNumber())
			{
				return true;
			}
		}
		return false;
	};

	const double Pad = FMath::Clamp((double)S->Padding, 0.0, 0.49);
	const double SpanX = BMax[0] - BMin[0], SpanY = BMax[1] - BMin[1];
	const double MinXY[2] = { BMin[0] + SpanX * Pad, BMin[1] + SpanY * Pad };
	const double MaxXY[2] = { BMax[0] - SpanX * Pad, BMax[1] - SpanY * Pad };

	const int32 NX = FMath::Max(1, GridX > 0 ? GridX : S->GridResolution.X);
	const int32 NY = FMath::Max(1, GridY > 0 ? GridY : S->GridResolution.Y);
	const double CellW = (MaxXY[0] - MinXY[0]) / NX;
	const double CellH = (MaxXY[1] - MinXY[1]) / NY;
	const double Jitter = FMath::Clamp((double)S->Jitter, 0.0, 1.0);
	// Nominal Z = top of the content, so the spawn-time down-trace starts ABOVE the terrain. A fixed
	// height fails on maps whose ground sits far from 0 (e.g. Electric Dreams). Overwritten by the hit.
	const double Z = BMax[2];

	// Cluster centers + weights for density-weighted aiming.
	struct FAimCluster { double X, Y, W; };
	TArray<FAimCluster> AimClusters;
	const TArray<TSharedPtr<FJsonValue>>* ClustersJson = nullptr;
	if (Scene->TryGetArrayField(TEXT("clusters"), ClustersJson) && ClustersJson)
	{
		for (const TSharedPtr<FJsonValue>& C : *ClustersJson)
		{
			const TSharedPtr<FJsonObject> Obj = C->AsObject();
			const TArray<TSharedPtr<FJsonValue>> Ctr = Obj->GetArrayField(TEXT("center"));
			double W = 1.0; Obj->TryGetNumberField(TEXT("weight"), W);
			AimClusters.Add({ Ctr[0]->AsNumber(), Ctr[1]->AsNumber(), FMath::Max(0.0, W) });
		}
	}
	const bool bAim = S->bAimAtClusters && AimClusters.Num() > 0;
	const double AimFrac = FMath::Clamp((double)S->AimFraction, 0.0, 1.0);

	FRandomStream Rng(S->RandomSeed);

	// Density-weighted aim: every cluster "pulls" the camera's facing toward it, weighted by its asset
	// count and a distance falloff (reach ~1/4 of the map). Summing the pulls aims the camera at the
	// local density concentration (the heat map's hot zone) instead of just whichever cell it sits in.
	// Returns unset when the pull cancels out (uniform/symmetric density) so the caller keeps base yaw.
	const double AimReach2 = FMath::Max(1.0, FMath::Square(0.25 * FMath::Max(SpanX, SpanY)));
	auto YawToDensity = [&](double Px, double Py) -> TOptional<double>
	{
		double SumX = 0.0, SumY = 0.0;
		for (const FAimCluster& C : AimClusters)
		{
			const double Dx = C.X - Px, Dy = C.Y - Py;
			const double D2 = Dx * Dx + Dy * Dy;
			if (D2 < 1.0) { continue; } // cluster essentially under the camera -> no usable direction
			const double Falloff = 1.0 / (1.0 + D2 / AimReach2);
			const double WF = C.W * Falloff / FMath::Sqrt(D2); // unit direction * weighted falloff
			SumX += Dx * WF; SumY += Dy * WF;
		}
		if (SumX * SumX + SumY * SumY < KINDA_SMALL_NUMBER) { return TOptional<double>(); }
		return FMath::RadiansToDegrees(FMath::Atan2(SumY, SumX));
	};

	// Build the JSON.
	FString Json;
	TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&Json);
	Wr->WriteObjectStart();
	Wr->WriteObjectStart(TEXT("config"));
	Wr->WriteArrayStart(TEXT("grid")); Wr->WriteValue(NX); Wr->WriteValue(NY); Wr->WriteArrayEnd();
	Wr->WriteArrayStart(TEXT("cell_size")); Wr->WriteValue(CellW); Wr->WriteValue(CellH); Wr->WriteArrayEnd();
	Wr->WriteValue(TEXT("jitter"), Jitter);
	Wr->WriteValue(TEXT("padding"), Pad);
	Wr->WriteValue(TEXT("nominal_z"), Z);
	Wr->WriteValue(TEXT("bounds_source"), SourceName);
	Wr->WriteObjectEnd();

	int32 CamCount = 0, Dropped = 0;
	// Cameras are written into a temporary array first so camera_count is accurate.
	TArray<TTuple<double, double, double, double, double>> Cams; // x, y, pitch, yaw, roll
	for (int32 i = 0; i < NX; ++i)
	{
		for (int32 j = 0; j < NY; ++j)
		{
			const double Cx = MinXY[0] + (i + 0.5) * CellW;
			const double Cy = MinXY[1] + (j + 0.5) * CellH;
			const double Px = Cx + Rng.FRandRange(-1.0, 1.0) * Jitter * (CellW * 0.5);
			const double Py = Cy + Rng.FRandRange(-1.0, 1.0) * Jitter * (CellH * 0.5);
			if (bUseNav && !InsideNavmesh(Px, Py)) { ++Dropped; continue; }

			double Yaw = S->BaseRotation.Yaw;
			if (bAim && Rng.FRand() < AimFrac)
			{
				const TOptional<double> D = YawToDensity(Px, Py);
				if (D.IsSet()) { Yaw = D.GetValue(); }
			}
			Cams.Emplace(Px, Py, S->BaseRotation.Pitch, Yaw, S->BaseRotation.Roll);
			++CamCount;
		}
	}

	Wr->WriteValue(TEXT("camera_count"), CamCount);
	Wr->WriteArrayStart(TEXT("cameras"));
	for (const auto& C : Cams)
	{
		Wr->WriteObjectStart();
		Wr->WriteArrayStart(TEXT("location"));
		Wr->WriteValue(C.Get<0>()); Wr->WriteValue(C.Get<1>()); Wr->WriteValue(Z);
		Wr->WriteArrayEnd();
		Wr->WriteArrayStart(TEXT("rotation"));
		Wr->WriteValue(C.Get<2>()); Wr->WriteValue(C.Get<3>()); Wr->WriteValue(C.Get<4>());
		Wr->WriteArrayEnd();
		Wr->WriteObjectEnd();
	}
	Wr->WriteArrayEnd();
	Wr->WriteObjectEnd();
	Wr->Close();

	const FString OutPath = FPaths::Combine(DataDir(), TEXT("camera_positions.json"));
	FFileHelper::SaveStringToFile(Json, *OutPath);
	UE_LOG(LogCameraProfilingEditor, Log, TEXT("[grid] %dx%d -> %d cameras (%d dropped off-navmesh) -> %s"),
		NX, NY, CamCount, Dropped, *OutPath);
	return OutPath;
}

int32 FCameraProfilingTools::SpawnCameras()
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		return 0;
	}
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();

	const FString PosPath = FPaths::Combine(DataDir(), TEXT("camera_positions.json"));
	FString PosStr;
	TSharedPtr<FJsonObject> Payload;
	if (!FFileHelper::LoadFileToString(PosStr, *PosPath) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PosStr), Payload) || !Payload.IsValid())
	{
		UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[spawn] camera_positions.json missing; run Generate Cameras."));
		return 0;
	}

	// Remove stale GridCam_* from a previous run.
	TArray<AActor*> Stale;
	for (TActorIterator<ACameraActor> It(World); It; ++It)
	{
		if (It->GetActorLabel().StartsWith(TEXT("GridCam_"))) { Stale.Add(*It); }
	}
	for (AActor* A : Stale) { World->EditorDestroyActor(A, true); }

	// Cell half-size bounds how far a camera may be nudged to find navmesh.
	const TSharedPtr<FJsonObject> Cfg = Payload->GetObjectField(TEXT("config"));
	const TArray<TSharedPtr<FJsonValue>> CellSize = Cfg->GetArrayField(TEXT("cell_size"));
	const double HalfW = FMath::Max(1.0, CellSize[0]->AsNumber() * 0.5);
	const double HalfH = FMath::Max(1.0, CellSize[1]->AsNumber() * 0.5);

	// Ring offsets within the cell (original point first), mirroring the Python placement search.
	TArray<TPair<double, double>> Offsets;
	Offsets.Emplace(0.0, 0.0);
	for (double Frac : { 0.3, 0.45 })
	{
		for (int32 Ang = 0; Ang < 360; Ang += 45)
		{
			const double A = FMath::DegreesToRadians(Ang);
			Offsets.Emplace(FMath::Cos(A) * Frac * HalfW, FMath::Sin(A) * Frac * HalfH);
		}
	}
	const int32 MaxTries = FMath::Min(Offsets.Num(), 1 + FMath::Max(0, S->PlacementRetries));
	// Navmesh is only used in NavMesh bounds mode; Scene / World Partition place by raycast alone.
	const bool bRequireNav = (S->BoundsSource == ECameraBoundsSource::NavMesh);
	const double NavToleranceZ = 300.0; // max vertical gap allowed between the ground hit and the navmesh

	auto GroundIfValid = [&](double Cx, double Cy, double NominalZ, double& OutG) -> bool
	{
		TOptional<double> G = ResolveGroundZ(World, Cx, Cy, NominalZ);
		if (!G.IsSet()) { return false; }
		if (bRequireNav && !OnNavmesh(World, Cx, Cy, G.GetValue(), NavToleranceZ)) { return false; }
		OutG = G.GetValue();
		return true;
	};

	int32 Spawned = 0, Skipped = 0, Nudged = 0;
	const TArray<TSharedPtr<FJsonValue>> CamerasJson = Payload->GetArrayField(TEXT("cameras"));
	int32 Idx = 0;
	for (const TSharedPtr<FJsonValue>& CamVal : CamerasJson)
	{
		const TSharedPtr<FJsonObject> Cam = CamVal->AsObject();
		const TArray<TSharedPtr<FJsonValue>> Loc = Cam->GetArrayField(TEXT("location"));
		const TArray<TSharedPtr<FJsonValue>> Rot = Cam->GetArrayField(TEXT("rotation"));
		const double X = Loc[0]->AsNumber(), Y = Loc[1]->AsNumber(), NominalZ = Loc[2]->AsNumber();

		// Find a valid (ground + navmesh) spot.
		double FX = X, FY = Y, Ground = 0.0;
		bool bFound = false;
		for (int32 t = 0; t < MaxTries; ++t)
		{
			const double OX = X + Offsets[t].Key, OY = Y + Offsets[t].Value;
			if (GroundIfValid(OX, OY, NominalZ, Ground)) { FX = OX; FY = OY; bFound = true; break; }
		}
		if (!bFound && bRequireNav)
		{
			double Z;
			if (ProjectNav(World, FVector(X, Y, NominalZ), FVector(FMath::Max(HalfW, HalfH) * 2.0, FMath::Max(HalfW, HalfH) * 2.0, 100000.0), Z))
			{
				// Snap to the nearest navmesh point and resolve its ground.
				FNavLocation NL;
				if (UNavigationSystemV1* Nav = NavSystem(World))
				{
					if (Nav->ProjectPointToNavigation(FVector(X, Y, NominalZ), NL,
						FVector(FMath::Max(HalfW, HalfH) * 2.0, FMath::Max(HalfW, HalfH) * 2.0, 100000.0)))
					{
						FX = NL.Location.X; FY = NL.Location.Y;
						TOptional<double> G = ResolveGroundZ(World, FX, FY, NominalZ);
						Ground = G.IsSet() ? G.GetValue() : NL.Location.Z;
						bFound = true;
					}
				}
			}
		}
		if (!bFound)
		{
			++Skipped;
			// Diagnose WHY: did the down-trace hit any collision at all under this point?
			const bool bGroundHit = ResolveGroundZ(World, X, Y, NominalZ).IsSet();
			UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[spawn] camera %d near (%.0f,%.0f) skipped: %s"),
				Idx, X, Y,
				bGroundHit ? TEXT("ground hit but off-navmesh (only happens in NavMesh bounds mode)")
				           : TEXT("down-trace hit NO collision under this point (geometry likely has collision disabled)"));
			++Idx;
			continue;
		}
		if (FMath::Abs(FX - X) > 1.0 || FMath::Abs(FY - Y) > 1.0) { ++Nudged; }

		const FVector Location(FX, FY, Ground + S->HeightAboveGround);
		const FRotator Rotation(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
		if (ACameraActor* Actor = World->SpawnActor<ACameraActor>(Location, Rotation))
		{
			Actor->SetActorLabel(FString::Printf(TEXT("GridCam_%03d"), Idx));
			Actor->SetFolderPath(FName(*S->OutlinerFolder));
			++Spawned;
		}
		++Idx;
	}

	// Write camera_grid.json (resolved transforms + profiling params) for the runtime subsystem,
	// from every camera in the outliner folder (spawned GridCam_* plus any added by hand).
	const FName Folder(*S->OutlinerFolder);
	FString GridJson;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&GridJson);
	W->WriteObjectStart();
	W->WriteArrayStart(TEXT("cameras"));
	for (TActorIterator<ACameraActor> It(World); It; ++It)
	{
		if (It->GetFolderPath() != Folder) { continue; }
		const FVector L = It->GetActorLocation();
		const FRotator R = It->GetActorRotation();
		W->WriteObjectStart();
		W->WriteArrayStart(TEXT("location")); W->WriteValue(L.X); W->WriteValue(L.Y); W->WriteValue(L.Z); W->WriteArrayEnd();
		W->WriteArrayStart(TEXT("rotation")); W->WriteValue(R.Pitch); W->WriteValue(R.Yaw); W->WriteValue(R.Roll); W->WriteArrayEnd();
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();
	W->WriteObjectStart(TEXT("profile"));
	W->WriteValue(TEXT("warmup_frames"), S->WarmupTicks);
	W->WriteValue(TEXT("settle_frames"), S->SettleTicks);
	W->WriteValue(TEXT("capture_seconds"), S->CaptureSeconds);
	W->WriteValue(TEXT("channels"), S->TraceChannels);
	W->WriteObjectEnd();
	W->WriteObjectEnd();
	W->Close();
	FFileHelper::SaveStringToFile(GridJson, *FPaths::Combine(DataDir(), TEXT("camera_grid.json")));

	UE_LOG(LogCameraProfilingEditor, Log, TEXT("[spawn] spawned %d cameras (%d nudged), skipped %d."), Spawned, Nudged, Skipped);
	return Spawned;
}

void FCameraProfilingTools::GenerateData(int32 GridX, int32 GridY)
{
	// Always refresh the scanned data (density incl. lights + top-down render).
	if (ExportSceneData().IsEmpty())
	{
		return;
	}
	CaptureTopdown();

	// Only lay out + spawn a fresh camera grid when there isn't one yet. If GridCam_* cameras already
	// exist in the CameraGrid folder, keep them -- this run just refreshes the heat-map data on the
	// existing layout. (To lay out a new grid, delete the existing cameras first, then run this.)
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	const FName Folder(*S->OutlinerFolder);
	int32 Existing = 0;
	if (UWorld* World = EditorWorld())
	{
		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			if (It->GetFolderPath() == Folder) { ++Existing; }
		}
	}
	if (Existing > 0)
	{
		UE_LOG(LogCameraProfilingEditor, Log,
			TEXT("[generate] %d camera(s) already in '%s'; kept (data refreshed only). Delete them to lay out a new grid."),
			Existing, *S->OutlinerFolder);
	}
	else if (!GenerateCameraGrid(GridX, GridY).IsEmpty())
	{
		SpawnCameras();
	}

	// Archive this generation so you can switch back to it later (panel "History" dropdown).
	SnapshotGeneration();
}
