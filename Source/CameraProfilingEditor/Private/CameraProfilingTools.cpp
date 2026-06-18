#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "StaticMeshResources.h"

#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "AI/Navigation/NavigationTypes.h"

#include "HAL/FileManager.h"
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
	/** Per-fine-cell accumulator: instance count, total triangles, draw-call estimate. */
	struct FDensityBin
	{
		int32 Instances = 0;
		double Triangles = 0.0;
		int32 Draws = 0;
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

TOptional<double> FCameraProfilingTools::ResolveGroundZ(UWorld* World, double X, double Y, double NominalZ)
{
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	if (!World)
	{
		return TOptional<double>();
	}

	// NavMesh method: project first, fall back to raycast.
	if (S->PlacementMethod == ECameraPlacementMethod::NavMesh)
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
		if (!Actor || Actor->IsA<ACameraActor>() || Actor->GetActorLabel().StartsWith(TEXT("GridCam_")))
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
	W->WriteValue(TEXT("instances")); W->WriteValue(TEXT("triangles")); W->WriteValue(TEXT("draws"));
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
		TEXT("[export] %d actors, %d placements, %d clusters, %d navmesh volume(s); triangles=%.0f draws=%lld -> %s"),
		ActorCount, PointCount, Clusters.Num(), NavVolumes.Num(), TotalTris, TotalDraws, *OutPath);
	return OutPath;
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
	const TSharedPtr<FJsonObject> Bounds = Scene->GetObjectField(TEXT("bounds"));
	const TArray<TSharedPtr<FJsonValue>> Mn = Bounds->GetArrayField(TEXT("min"));
	const TArray<TSharedPtr<FJsonValue>> Mx = Bounds->GetArrayField(TEXT("max"));
	const double MinX = Mn[0]->AsNumber(), MinY = Mn[1]->AsNumber();
	const double MaxX = Mx[0]->AsNumber(), MaxY = Mx[1]->AsNumber(), MaxZ = Mx[2]->AsNumber();

	const double CX = (MinX + MaxX) * 0.5, CY = (MinY + MaxY) * 0.5;
	const double Span = FMath::Max3(1.0, MaxX - MinX, MaxY - MinY);
	const double Half = Span * 0.5;
	const int32 Px = FMath::Max(256, S->TopdownPx);

	UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(World, Px, Px, RTF_RGBA8);
	if (!RT)
	{
		return false;
	}

	// pitch -90 (straight down), yaw 90 (orient so the PNG lines up with the heat-map grid).
	// Transient throwaway actor: don't let a one-shot render capture dirty/save the level.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	ASceneCapture2D* Cap = World->SpawnActor<ASceneCapture2D>(
		FVector(CX, CY, MaxZ + 50000.0), FRotator(-90.0, 90.0, 0.0), SpawnParams);
	if (!Cap)
	{
		return false;
	}

	bool bOk = false;
	if (USceneCaptureComponent2D* Comp = Cap->GetCaptureComponent2D())
	{
		Comp->ProjectionType = ECameraProjectionMode::Orthographic;
		Comp->OrthoWidth = Span;
		Comp->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		Comp->TextureTarget = RT;
		if (!FMath::IsNearlyZero(S->TopdownExposureBias))
		{
			Comp->PostProcessSettings.bOverride_AutoExposureBias = true;
			Comp->PostProcessSettings.AutoExposureBias = S->TopdownExposureBias;
		}
		Comp->CaptureScene();
		UKismetRenderingLibrary::ExportRenderTarget(World, RT, DataDir(), TEXT("map_topdown.png"));

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
		UE_LOG(LogCameraProfilingEditor, Log, TEXT("[topdown] wrote map_topdown.png (%dpx, span %.0fuu)."), Px, Span);
	}

	World->EditorDestroyActor(Cap, /*bShouldModifyLevel=*/false);
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

	// Choose extent: union of NavMesh volumes, or whole-scene bounds.
	double BMin[3], BMax[3];
	const TArray<TSharedPtr<FJsonValue>>* Volumes = nullptr;
	const bool bWantNav = (S->BoundsSource == ECameraBoundsSource::NavMesh);
	const bool bHaveVolumes = Scene->TryGetArrayField(TEXT("navmesh_volumes"), Volumes) && Volumes && Volumes->Num() > 0;
	const bool bUseNav = bWantNav && bHaveVolumes;

	if (bUseNav)
	{
		for (int32 i = 0; i < 3; ++i) { BMin[i] = TNumericLimits<double>::Max(); BMax[i] = TNumericLimits<double>::Lowest(); }
		for (const TSharedPtr<FJsonValue>& V : *Volumes)
		{
			const TSharedPtr<FJsonObject> Obj = V->AsObject();
			const TArray<TSharedPtr<FJsonValue>> VMin = Obj->GetArrayField(TEXT("min"));
			const TArray<TSharedPtr<FJsonValue>> VMax = Obj->GetArrayField(TEXT("max"));
			for (int32 i = 0; i < 3; ++i)
			{
				BMin[i] = FMath::Min(BMin[i], VMin[i]->AsNumber());
				BMax[i] = FMath::Max(BMax[i], VMax[i]->AsNumber());
			}
		}
	}
	else
	{
		const TSharedPtr<FJsonObject> Bounds = Scene->GetObjectField(TEXT("bounds"));
		const TArray<TSharedPtr<FJsonValue>> Mn = Bounds->GetArrayField(TEXT("min"));
		const TArray<TSharedPtr<FJsonValue>> Mx = Bounds->GetArrayField(TEXT("max"));
		for (int32 i = 0; i < 3; ++i) { BMin[i] = Mn[i]->AsNumber(); BMax[i] = Mx[i]->AsNumber(); }
	}

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
	const double Z = S->TraceExtraHeight; // nominal Z (overwritten by the ground hit at spawn)

	// Cluster centers for aiming.
	TArray<FVector2D> ClusterXY;
	const TArray<TSharedPtr<FJsonValue>>* ClustersJson = nullptr;
	if (Scene->TryGetArrayField(TEXT("clusters"), ClustersJson) && ClustersJson)
	{
		for (const TSharedPtr<FJsonValue>& C : *ClustersJson)
		{
			const TArray<TSharedPtr<FJsonValue>> Ctr = C->AsObject()->GetArrayField(TEXT("center"));
			ClusterXY.Emplace(Ctr[0]->AsNumber(), Ctr[1]->AsNumber());
		}
	}
	const bool bAim = S->bAimAtClusters && ClusterXY.Num() > 0;
	const double AimFrac = FMath::Clamp((double)S->AimFraction, 0.0, 1.0);

	FRandomStream Rng(S->RandomSeed);

	auto YawToNearestCluster = [&](double Px, double Py) -> double
	{
		double BestD2 = TNumericLimits<double>::Max();
		FVector2D Best(Px, Py);
		for (const FVector2D& C : ClusterXY)
		{
			const double D2 = FMath::Square(C.X - Px) + FMath::Square(C.Y - Py);
			if (D2 < BestD2) { BestD2 = D2; Best = C; }
		}
		return FMath::RadiansToDegrees(FMath::Atan2(Best.Y - Py, Best.X - Px));
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
	Wr->WriteValue(TEXT("bounds_source"), bUseNav ? TEXT("navmesh") : TEXT("scene"));
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
			if (bAim && Rng.FRand() < AimFrac) { Yaw = YawToNearestCluster(Px, Py); }
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
	const bool bRequireNav = S->bRequireNavmesh;

	auto GroundIfValid = [&](double Cx, double Cy, double NominalZ, double& OutG) -> bool
	{
		TOptional<double> G = ResolveGroundZ(World, Cx, Cy, NominalZ);
		if (!G.IsSet()) { return false; }
		if (bRequireNav && !OnNavmesh(World, Cx, Cy, G.GetValue(), S->NavmeshToleranceZ)) { return false; }
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
			UE_LOG(LogCameraProfilingEditor, Warning, TEXT("[spawn] camera %d: no valid spot near (%.0f,%.0f); skipped."), Idx, X, Y);
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

void FCameraProfilingTools::GenerateCameras(int32 GridX, int32 GridY)
{
	if (ExportSceneData().IsEmpty())
	{
		return;
	}
	CaptureTopdown();
	if (GenerateCameraGrid(GridX, GridY).IsEmpty())
	{
		return;
	}
	SpawnCameras();
}
