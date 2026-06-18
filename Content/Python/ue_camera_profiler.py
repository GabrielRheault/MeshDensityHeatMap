"""
ue_camera_profiler.py  --  RUNS INSIDE UNREAL ENGINE 5.6 (Python API).

Part of the "Camera Profiling" plugin. Normally driven by the Tools -> Yes Chef ->
Camera Profiling menu; you don't run it by hand.

Three responsibilities:
    1. EXPORT  : write scene_data.json (actor + instanced-mesh positions, bounds, navmesh
                 volumes, density grid for the heat map)
    2. SPAWN   : read camera_positions.json, raycast/NavMesh to validate Z, spawn CameraActors
    3. PROFILE : per camera -> 1 .utrace (Unreal Insights) + screenshot + memreport, in PIE
                 (game render path) or the editor viewport.

Profiling is tick-driven (register_slate_post_tick_callback): a frame must render between
cameras, so we step one camera per few ticks. The call returns immediately; profiling
continues in the background and logs "Camera profiling complete" when done.

Output paths are derived from this file's location (-> <Project>/Saved/CameraProfiling/data),
so the plugin works dropped into any UE5.6 project with no edits.
"""

import json
import math
import os
import re

import unreal

# ----------------------------------------------------------------------------
# Paths -- derived from this file so the plugin is portable.
#   .../Plugins/CameraProfiling/Content/Python -> project root is 4 levels up.
# ----------------------------------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.abspath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir))
_DATA_DIR = os.path.join(_PROJECT_ROOT, "Saved", "CameraProfiling", "data").replace("\\", "/")

# ----------------------------------------------------------------------------
# CONFIG
# ----------------------------------------------------------------------------
CONFIG = {
    "data_dir": _DATA_DIR,
    "scene_data_file": "scene_data.json",
    "camera_positions_file": "camera_positions.json",
    "screenshot_dir": _DATA_DIR + "/screenshots",

    "RUN_EXPORT": True,
    "RUN_SPAWN": True,
    "RUN_PROFILE": True,

    # Placement: "raycast" (line trace -Z) or "navmesh" (project to NavMesh).
    "placement_method": "raycast",
    "trace_extra_height": 100000.0,   # start the down-trace this far above the nominal Z
    "trace_depth": 300000.0,          # trace down this far from the start
    "height_above_ground": 250.0,     # place the camera this far above the ground hit
    "navmesh_project_extent": [200.0, 200.0, 100000.0],
    # Only spawn where the raycast hit is actually ON the NavMesh. Rejects cameras that
    # landed on roofs/geometry inside the NavMeshBoundsVolume box but with no navmesh under them.
    "require_navmesh": True,
    # Max vertical gap (UU) allowed between the raycast hit and the NavMesh below it.
    # Set roughly to your shortest building/prop height; bigger = more lenient.
    "navmesh_tolerance_z": 300.0,
    # If a camera's point isn't on the navmesh, try this many ring offsets WITHIN its grid cell
    # before giving up (then it snaps to the nearest navmesh point). Keeps the grid count intact.
    "placement_retries": 16,
    "outliner_folder": "CameraGrid",  # World Outliner folder the spawned cameras are grouped under

    # Profiling
    # "pie"    = profile inside Play-In-Editor (game render path + game scalability => shipping-
    #            representative numbers). RECOMMENDED for real perf.
    # "editor" = profile by moving the editor viewport (fast, but editor render path is costlier).
    "profile_mode": "pie",
    "screenshot_res": [1920, 1080],
    "settle_ticks": 5,     # editor mode: ticks to render after moving the viewport, before tracing
    "capture_ticks": 12,   # ticks the trace records this view before stopping (more = more frames)
    "trace_dir": _DATA_DIR + "/traces",
    "trace_channels": "cpu,gpu,frame,bookmark",  # Unreal Insights channels per .utrace

    # --- PIE-mode tuning ---
    "pie_warmup_ticks": 30,   # ticks to let PIE spin up (streaming/Lumen/TSR) before the 1st camera
    "pie_settle_ticks": 24,   # ticks after switching view target, before tracing (temporal AA/Lumen
                              #   need ~this many frames to converge for stable GPU timings)
    # Standalone (C++) profiler: measure each camera for this many WALL-CLOCK seconds to get its
    # avg frame time / FPS (frame rate is uncapped during the run so the number is real).
    "capture_seconds": 1.0,
    # Console commands applied ONCE at profile start (e.g. force game scalability so PIE matches
    # your shipping target). Empty = use whatever the editor/PIE currently has.
    "scalability_commands": [],
    # editor mode only: hide editor sprites/gizmos/grid during capture for a cleaner (slightly
    # cheaper) frame. No effect in PIE (already the game view).
    "editor_game_view": True,

    # Export: include instanced-mesh instances when computing scene bounds. False = actors only.
    "export_instances": True,
    # Write every position into scene_data.json. Only needed for placement_method "data";
    # leave False so the JSON stays small (bounds + navmesh volumes are computed either way).
    "export_positions": False,

    # Asset-density clustering (so cameras can aim at where assets are dense).
    "cluster_cell_size": 2000.0,   # bin size (UU) for density binning (the heat map's coarse LOD)
    "min_cluster_weight": 20,      # a bin needs at least this many assets to count as a cluster
    # Top-down map overlay: resolution (px) of the orthographic render used as the heat-map background.
    "topdown_px": 2048,
    # Heat map finest LOD = cluster_cell_size / heatmap_subdiv (must be a power of 2). The viewer
    # shows base, base/2, ... down to the finest as you zoom. 8 = base, /2, /4, /8 (3 subdivisions).
    # Larger = finer detail when zoomed in, but a bigger scene_data.json.
    "heatmap_subdiv": 8,
}


# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------
def _editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def _actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def _ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def _folder_cameras(cfg):
    """All CameraActors in the outliner folder (spawned GridCam_* PLUS any you add by hand).
    This is the single source of 'which cameras to profile'."""
    folder = str(cfg.get("outliner_folder", "CameraGrid"))
    return [a for a in _actor_subsystem().get_all_level_actors()
            if isinstance(a, unreal.CameraActor) and str(a.get_folder_path()) == folder]


# Per-StaticMesh metrics for the heat map, cached by mesh path (read each mesh once).
# (verts, materials) -- materials is used as the draw-call estimate (one section per material).
# NOTE: triangle count isn't exposed to Python in 5.6 (UStaticMesh::GetNumTriangles isn't a
# UFUNCTION), so we use VERTICES as the geometry-weight metric.
_MESH_METRICS = {}
_MESH_METRIC_WARNED = False


def _comp_static_mesh(comp):
    """The StaticMesh of a component, robust across subclasses. Foliage's
    FoliageInstancedStaticMeshComponent doesn't expose get_static_mesh() in Python, so fall back
    to the 'static_mesh' UPROPERTY (present on every UStaticMeshComponent subclass)."""
    getter = getattr(comp, "get_static_mesh", None)
    if getter is not None:
        try:
            return getter()
        except Exception:
            pass
    try:
        return comp.get_editor_property("static_mesh")
    except Exception:
        return None


def _mesh_metrics(mesh):
    global _MESH_METRIC_WARNED
    if mesh is None:
        return (0, 0)
    key = mesh.get_path_name()
    cached = _MESH_METRICS.get(key)
    if cached is None:
        try:
            sm = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
            cached = (int(sm.get_number_verts(mesh, 0)), int(sm.get_number_materials(mesh)))
        except Exception as exc:
            if not _MESH_METRIC_WARNED:  # surface the first failure instead of silently 0-ing
                _MESH_METRIC_WARNED = True
                unreal.log_warning(f"[export] mesh-metric read failed (e.g. {key}): {exc} -- "
                                   f"vertices/draw-calls will be 0. Is StaticMeshEditorSubsystem available?")
            cached = (0, 0)
        _MESH_METRICS[key] = cached
    return cached


# ----------------------------------------------------------------------------
# 1. EXPORT
# ----------------------------------------------------------------------------
def export_scene_data(cfg):
    # Re-read mesh metrics fresh each export (avoid a stale 0,0 from a previous transient failure).
    global _MESH_METRIC_WARNED
    _MESH_METRICS.clear()
    _MESH_METRIC_WARNED = False

    # Exclude our own profiler cameras (and any CameraActor) so repeated runs don't
    # count GridCam_* as "assets" and pollute the bounds / density clusters.
    actors = [a for a in _actor_subsystem().get_all_level_actors()
              if not isinstance(a, unreal.CameraActor)
              and not a.get_actor_label().startswith("GridCam_")]

    positions = []
    store_positions = bool(cfg.get("export_positions", False))
    point_count = 0
    mn = [float("inf")] * 3
    mx = [float("-inf")] * 3

    # Density bins for clustering: (ix, iy) -> [sum_x, sum_y, sum_z, count]
    cluster_cell = max(1.0, float(cfg.get("cluster_cell_size", 2000.0)))
    bins = {}
    # Heat map bins assets at the FINEST LOD cell (= base / heatmap_subdiv); the viewer sums
    # these up for coarser LODs. heatmap_subdiv=4 -> 2 zoom subdivisions (base, base/2, base/4).
    heatmap_subdiv = max(1, int(cfg.get("heatmap_subdiv", 4)))
    fine_cell = cluster_cell / heatmap_subdiv
    fbins = {}

    def add(x, y, z, verts=0, draws=0):
        nonlocal point_count
        point_count += 1
        if store_positions:
            positions.append([round(x, 1), round(y, 1), round(z, 1)])
        for i, v in enumerate((x, y, z)):
            if v < mn[i]:
                mn[i] = v
            if v > mx[i]:
                mx[i] = v
        # fine heat-map bin: [instances, vertices, draw_calls]
        fkey = (int(math.floor(x / fine_cell)), int(math.floor(y / fine_cell)))
        fb = fbins.get(fkey)
        if fb is None:
            fbins[fkey] = [1, verts, draws]
        else:
            fb[0] += 1; fb[1] += verts; fb[2] += draws
        # coarse cluster bin (for camera aiming) -- count only
        key = (int(math.floor(x / cluster_cell)), int(math.floor(y / cluster_cell)))
        b = bins.get(key)
        if b is None:
            bins[key] = [x, y, z, 1]
        else:
            b[0] += x; b[1] += y; b[2] += z; b[3] += 1

    def add_draws(x, y, draws):
        # Add draw-call weight to a cell WITHOUT counting an instance/vertices -- used for instanced
        # components, whose many instances batch into ONE set of section draws (not one per instance).
        if draws <= 0:
            return
        fkey = (int(math.floor(x / fine_cell)), int(math.floor(y / fine_cell)))
        fb = fbins.get(fkey)
        if fb is None:
            fbins[fkey] = [0, 0, draws]
        else:
            fb[2] += draws

    # Every NavMeshBoundsVolume in the level, as its own AABB (volumes can be disjoint).
    navmesh_volumes = []
    expand_instances = bool(cfg.get("export_instances", True))

    for actor in actors:
        # Count only actual renderable mesh COMPONENTS at their own world location. This skips
        # lights/volumes/empty actors and the actor pivot (which previously each added a fake
        # 0-vertex density point), and handles blueprints with several mesh components.
        for comp in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = _comp_static_mesh(comp)
            if mesh is None:
                continue
            verts, mats = _mesh_metrics(mesh)
            if isinstance(comp, unreal.InstancedStaticMeshComponent):
                count = comp.get_instance_count()
                if expand_instances and count > 0:
                    for i in range(count):
                        t = comp.get_instance_transform(i, world_space=True).translation
                        add(t.x, t.y, t.z, verts, 0)        # geometry weight per instance
                    cloc = comp.get_world_location()
                    add_draws(cloc.x, cloc.y, mats)         # instanced draws batch -> sections once
                else:
                    cloc = comp.get_world_location()
                    add(cloc.x, cloc.y, cloc.z, verts, mats)
            else:
                cloc = comp.get_world_location()
                add(cloc.x, cloc.y, cloc.z, verts, mats)    # one placement per mesh component

        if isinstance(actor, unreal.NavMeshBoundsVolume):
            origin, extent = actor.get_actor_bounds(False)
            navmesh_volumes.append({
                "min": [origin.x - extent.x, origin.y - extent.y, origin.z - extent.z],
                "max": [origin.x + extent.x, origin.y + extent.y, origin.z + extent.z],
            })

    if point_count == 0:
        unreal.log_warning("[export] No actors found in the level.")
        mn = [0.0, 0.0, 0.0]
        mx = [0.0, 0.0, 0.0]

    # Dense bins -> cluster centers (centroid + weight).
    min_weight = int(cfg.get("min_cluster_weight", 20))
    clusters = [
        {"center": [round(b[0] / b[3], 1), round(b[1] / b[3], 1), round(b[2] / b[3], 1)], "weight": b[3]}
        for b in bins.values() if b[3] >= min_weight
    ]
    clusters.sort(key=lambda c: c["weight"], reverse=True)

    # Full density grid at the FINEST LOD cell for the 2D heat map (viewer sums up for coarser
    # LODs). Compact: cell size + [ix, iy, count]; cell covers [ix*cell,(ix+1)*cell] in X (same Y).
    density_grid = {
        "cell": fine_cell,          # finest LOD cell size
        "base_cell": cluster_cell,  # coarsest LOD; viewer subdivides base -> finest by /2 steps
        "metrics": ["instances", "vertices", "draws"],  # per-bin values after [ix, iy]
        "bins": [[ix, iy, b[0], b[1], b[2]] for (ix, iy), b in fbins.items()],
    }

    data = {
        "actor_count": len(actors),
        "position_count": point_count,
        "bounds": {"min": mn, "max": mx},
        "navmesh_volumes": navmesh_volumes,  # list of per-volume AABBs (may be empty)
        "clusters": clusters,                # dense asset centers [{center:[x,y,z], weight:n}]
        "density_grid": density_grid,        # all bins for the heat map (cell + [ix,iy,count])
        "positions": positions,  # empty unless export_positions = True
    }
    total_verts = sum(b[1] for b in fbins.values())
    total_draws = sum(b[2] for b in fbins.values())
    unreal.log(f"[export] mesh metrics: {len(_MESH_METRICS)} unique meshes, "
               f"total vertices {total_verts:,}, total draw-calls {total_draws:,}.")
    if total_verts == 0:
        unreal.log_warning("[export] total vertices is 0 -- meshes returned no vert count "
                           "(see any mesh-metric warning above). Heat map Vertices/Draw calls will be blank.")
    unreal.log(f"[export] {len(clusters)} asset cluster(s) (cell {cluster_cell:.0f}, min weight {min_weight}).")
    if navmesh_volumes:
        unreal.log(f"[export] found {len(navmesh_volumes)} NavMeshBoundsVolume(s).")
    else:
        unreal.log_warning("[export] no NavMeshBoundsVolume found; grid will fall back to scene bounds.")

    _ensure_dir(cfg["data_dir"])
    out_path = os.path.join(cfg["data_dir"], cfg["scene_data_file"])
    with open(out_path, "w") as f:
        json.dump(data, f)

    unreal.log(f"[export] {len(actors)} actors, {len(positions)} positions -> {out_path}")
    unreal.log(f"[export] bounds min={mn} max={mx}")
    return out_path


def capture_topdown(cfg):
    """Render an orthographic TOP-DOWN image of the level -> map_topdown.png (+ map_topdown.json
    with the world AABB it covers), used as the heat map's background overlay so a hot cell sits
    over recognizable geometry. Captures a SQUARE region centred on the scene."""
    world = _editor_world()
    try:
        with open(os.path.join(cfg["data_dir"], cfg["scene_data_file"]), "r") as f:
            b = json.load(f)["bounds"]
        mn, mx = b["min"], b["max"]
    except Exception as exc:
        unreal.log_warning(f"[topdown] no scene bounds yet ({exc}); run export first. Skipping.")
        return None

    cx, cy = (mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5
    span = max(1.0, mx[0] - mn[0], mx[1] - mn[1])  # square covering the larger axis
    half = span * 0.5
    px = int(cfg.get("topdown_px", 2048))

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, px, px, unreal.TextureRenderTargetFormat.RTF_RGBA8)

    actor_subsys = _actor_subsystem()
    cap = actor_subsys.spawn_actor_from_class(
        unreal.SceneCapture2D,
        unreal.Vector(cx, cy, mx[2] + 50000.0),
        unreal.Rotator(pitch=-90.0, yaw=0.0, roll=0.0))
    ok = False
    try:
        comp = cap.get_editor_property("capture_component2d")
        comp.set_editor_property("projection_type", unreal.CameraProjectionMode.ORTHOGRAPHIC)
        comp.set_editor_property("ortho_width", span)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("texture_target", rt)
        comp.capture_scene()
        unreal.RenderingLibrary.export_render_target(world, rt, cfg["data_dir"], "map_topdown.png")
        with open(os.path.join(cfg["data_dir"], "map_topdown.json"), "w") as f:
            json.dump({"image": "map_topdown.png",
                       "min": [cx - half, cy - half], "max": [cx + half, cy + half]}, f)
        ok = True
        unreal.log(f"[topdown] wrote map_topdown.png ({px}px, span {span:.0f}uu).")
    except Exception as exc:
        unreal.log_warning(f"[topdown] capture failed: {exc}")
    finally:
        actor_subsys.destroy_actor(cap)
    return ok


# ----------------------------------------------------------------------------
# 2. SPAWN  (with Z validation)
# ----------------------------------------------------------------------------
_DIAG_DONE = False


def _hit_location_z(hit_result):
    """Read the impact Z from a HitResult. This build exposes no FHitResult attributes, so we
    parse export_text() (e.g. '...,ImpactPoint=(X=..,Y=..,Z=123.4),...'); to_tuple() as backup."""
    global _DIAG_DONE
    try:
        text = hit_result.export_text()
    except Exception:
        text = ""

    if not _DIAG_DONE:
        _DIAG_DONE = True
        unreal.log(f"[diag] hit export_text: {text[:400]}")

    if text:
        match = (re.search(r"ImpactPoint=\(X=[^,]+,Y=[^,]+,Z=([-\d.eE+]+)\)", text)
                 or re.search(r"Location=\(X=[^,]+,Y=[^,]+,Z=([-\d.eE+]+)\)", text))
        if match:
            return float(match.group(1))

    # Backup: to_tuple() (FHitResult order -> Location=3, ImpactPoint=4).
    try:
        tup = hit_result.to_tuple()
        for i in (4, 3):
            if i < len(tup) and hasattr(tup[i], "z"):
                return float(tup[i].z)
    except Exception:
        pass
    return None


def _ground_z_raycast(world, x, y, nominal_z, cfg, verbose=False):
    """Down-trace for the ground Z. Tries the Visibility channel, then a WorldStatic/WorldDynamic
    object trace. Returns Z or None. When verbose, logs what the trace actually returned."""
    start = unreal.Vector(x, y, nominal_z + cfg["trace_extra_height"])
    end = unreal.Vector(x, y, nominal_z + cfg["trace_extra_height"] - cfg["trace_depth"])

    # 1) Visibility channel.
    hit = unreal.SystemLibrary.line_trace_single(
        world_context_object=world, start=start, end=end,
        trace_channel=unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, trace_complex=True,
        actors_to_ignore=[], draw_debug_type=unreal.DrawDebugTrace.NONE, ignore_self=True)
    if verbose:
        unreal.log(f"[diag] visibility trace: is_none={hit is None} type={type(hit).__name__}")
        if hit is not None:
            unreal.log(f"[diag] hit attrs = {[a for a in dir(hit) if not a.startswith('_')]}")
    z = _hit_location_z(hit) if hit is not None else None
    if z is not None:
        return z

    # 2) Object trace against WorldStatic (QUERY1) + WorldDynamic (QUERY2).
    hit = unreal.SystemLibrary.line_trace_single_for_objects(
        world_context_object=world, start=start, end=end,
        object_types=[unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY1, unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY2],
        trace_complex=False, actors_to_ignore=[], draw_debug_type=unreal.DrawDebugTrace.NONE, ignore_self=True)
    if verbose:
        unreal.log(f"[diag] object trace: is_none={hit is None}")
    z = _hit_location_z(hit) if hit is not None else None
    return z


# --- "data" placement: derive ground Z from the exported scene positions (no tracing) ---
_SCENE_POSITIONS = None


def _ground_z_from_data(x, y, cfg):
    """Nearest exported actor/instance position (by XY) gives a ground estimate."""
    global _SCENE_POSITIONS
    if _SCENE_POSITIONS is None:
        scene_path = os.path.join(cfg["data_dir"], cfg["scene_data_file"])
        with open(scene_path, "r") as f:
            _SCENE_POSITIONS = json.load(f).get("positions", [])
        if not _SCENE_POSITIONS:
            unreal.log_warning(
                "[spawn] placement_method='data' but scene_data.json has no positions. "
                "Set CONFIG['export_positions']=True and re-run Export, or use "
                "placement_method 'raycast'/'navmesh'. All cameras will be skipped otherwise.")
    best_d2 = None
    best_z = None
    for px, py, pz in _SCENE_POSITIONS:
        d2 = (px - x) * (px - x) + (py - y) * (py - y)
        if best_d2 is None or d2 < best_d2:
            best_d2 = d2
            best_z = pz
    return best_z


def _resolve_ground_z(world, x, y, nominal_z, cfg, verbose=False):
    method = cfg["placement_method"]
    if method == "data":
        return _ground_z_from_data(x, y, cfg)
    if method == "navmesh":
        z = _ground_z_navmesh(world, x, y, nominal_z, cfg)
        return z if z is not None else _ground_z_raycast(world, x, y, nominal_z, cfg, verbose)
    return _ground_z_raycast(world, x, y, nominal_z, cfg, verbose)


def _ground_z_navmesh(world, x, y, nominal_z, cfg):
    """Project the point onto the NavMesh; return Z or None (NavMesh must be built)."""
    try:
        nav = unreal.NavigationSystemV1.get_navigation_system(world)
        if not nav:
            return None
        extent = unreal.Vector(*cfg["navmesh_project_extent"])
        projected = unreal.NavigationSystemV1.project_point_to_navigation(
            world, unreal.Vector(x, y, nominal_z), nav_data=None, filter_class=None,
            query_extent=extent)
        # Returns the projected location (Vector) on success, None on failure.
        if projected is not None:
            return projected.z
    except Exception as exc:  # NavMesh not present / API mismatch -> caller falls back
        unreal.log_warning(f"[spawn] NavMesh projection failed: {exc}")
    return None


def _on_navmesh(world, x, y, z, cfg):
    """True if (x,y,z) sits on the NavMesh within navmesh_tolerance_z.

    Projects the raycast hit straight at the NavMesh using a small query box whose
    vertical half-extent is the tolerance. If there's no navmesh within that gap
    (e.g. the raycast hit a building roof and the real navmesh is far below), the
    projection fails -> return False so the camera is skipped. Fails OPEN (returns
    True) if there's no navigation system at all, so it can't block everything.
    """
    try:
        nav = unreal.NavigationSystemV1.get_navigation_system(world)
        if not nav:
            return True  # nothing to validate against
        tol = float(cfg.get("navmesh_tolerance_z", 300.0))
        extent = unreal.Vector(50.0, 50.0, tol)
        projected = unreal.NavigationSystemV1.project_point_to_navigation(
            world, unreal.Vector(x, y, z), nav_data=None, filter_class=None, query_extent=extent)
        return projected is not None
    except Exception as exc:
        unreal.log_warning(f"[spawn] navmesh check failed ({exc}); allowing camera.")
        return True


def _nearest_navmesh_point(world, x, y, z, extent_xy):
    """Nearest point ON the NavMesh to (x,y,z), within +/- extent_xy. Returns a Vector or None."""
    try:
        nav = unreal.NavigationSystemV1.get_navigation_system(world)
        if not nav:
            return None
        extent = unreal.Vector(extent_xy, extent_xy, 100000.0)
        return unreal.NavigationSystemV1.project_point_to_navigation(
            world, unreal.Vector(x, y, z), nav_data=None, filter_class=None, query_extent=extent)
    except Exception:
        return None


def _place_camera(world, x, y, nominal_z, cfg, half_w, half_h, verbose=False):
    """Find a valid spot (on ground + on NavMesh) for a camera near (x, y).

    Tries the original point, then ring offsets WITHIN the cell (so the camera stays in its grid
    cell), then as a last resort snaps to the nearest NavMesh point. Returns (fx, fy, ground) or
    None if nothing nearby works. This is what keeps a 2x2 grid from losing a camera when its
    jittered point happens to land off the navmesh / over a hole.
    """
    require_nav = bool(cfg.get("require_navmesh", True))

    def ground_if_valid(cx, cy, vb):
        g = _resolve_ground_z(world, cx, cy, nominal_z, cfg, verbose=vb)
        if g is None:
            return None
        if require_nav and not _on_navmesh(world, cx, cy, g, cfg):
            return None
        return g

    # original point first, then ring offsets within the cell
    offsets = [(0.0, 0.0)]
    for frac in (0.3, 0.45):
        for ang in range(0, 360, 45):
            a = math.radians(ang)
            offsets.append((math.cos(a) * frac * half_w, math.sin(a) * frac * half_h))
    max_tries = 1 + int(cfg.get("placement_retries", 16))
    for i, (ox, oy) in enumerate(offsets[:max_tries]):
        g = ground_if_valid(x + ox, y + oy, verbose and i == 0)
        if g is not None:
            return (x + ox, y + oy, g)

    # last resort: snap to the nearest navmesh point (within ~one cell)
    if require_nav:
        p = _nearest_navmesh_point(world, x, y, nominal_z, max(half_w, half_h) * 2.0)
        if p is not None:
            g = _resolve_ground_z(world, p.x, p.y, nominal_z, cfg)
            return (p.x, p.y, g if g is not None else p.z)
    return None


def spawn_cameras(cfg):
    world = _editor_world()
    pos_path = os.path.join(cfg["data_dir"], cfg["camera_positions_file"])
    if not os.path.isfile(pos_path):
        unreal.log_warning(f"[spawn] {pos_path} not found. Run 'Generate Cameras' first.")
        return []

    with open(pos_path, "r") as f:
        payload = json.load(f)

    global _SCENE_POSITIONS, _DIAG_DONE
    _SCENE_POSITIONS = None  # reload exported positions fresh each run
    _DIAG_DONE = False       # re-log the hit diagnostic once per run

    actor_subsys = _actor_subsystem()

    # Remove cameras from a previous run so they don't accumulate (and so a later
    # export doesn't count them as assets).
    stale = [a for a in actor_subsys.get_all_level_actors()
             if a.get_actor_label().startswith("GridCam_")]
    for a in stale:
        actor_subsys.destroy_actor(a)
    if stale:
        unreal.log(f"[spawn] removed {len(stale)} camera(s) from a previous run.")

    spawned = []
    skipped = 0
    nudged = 0

    # Cell size (from the grid step) bounds how far we may offset a camera to find navmesh.
    cell_cfg = payload.get("config", {}).get("cell_size", [2000.0, 2000.0])
    half_w = max(1.0, float(cell_cfg[0]) * 0.5)
    half_h = max(1.0, float(cell_cfg[1]) * 0.5)

    for idx, cam in enumerate(payload.get("cameras", [])):
        x, y, nominal_z = cam["location"]
        pitch, yaw, roll = cam["rotation"]

        placed = _place_camera(world, x, y, nominal_z, cfg, half_w, half_h, verbose=(idx == 0))
        if placed is None:
            skipped += 1
            unreal.log_warning(f"[spawn] camera {idx}: no valid navmesh spot near ({x:.0f},{y:.0f}); skipped.")
            continue

        fx, fy, ground = placed
        if abs(fx - x) > 1.0 or abs(fy - y) > 1.0:
            nudged += 1
            unreal.log(f"[spawn] camera {idx}: nudged ({x:.0f},{y:.0f}) -> ({fx:.0f},{fy:.0f}) to stay on navmesh.")

        z = ground + cfg["height_above_ground"]
        location = unreal.Vector(fx, fy, z)
        rotation = unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll)

        actor = actor_subsys.spawn_actor_from_class(unreal.CameraActor, location, rotation)
        if actor:
            actor.set_actor_label(f"GridCam_{idx:03d}")
            actor.set_folder_path(cfg["outliner_folder"])  # group under a World Outliner folder
            spawned.append(actor)

    # Build camera_grid.json from EVERY camera in the outliner folder -- the freshly spawned
    # GridCam_* plus any you added by hand -- so the standalone profiles them too.
    resolved = [{
        "location": [round(a.get_actor_location().x, 2), round(a.get_actor_location().y, 2), round(a.get_actor_location().z, 2)],
        "rotation": [round(a.get_actor_rotation().pitch, 2), round(a.get_actor_rotation().yaw, 2), round(a.get_actor_rotation().roll, 2)],
    } for a in _folder_cameras(cfg)]

    # Write the resolved transforms + shared profiling params for the in-game (standalone/console)
    # profiler to read. The "profile" block makes this CONFIG the single source for those values
    # (the C++ subsystem reads them as defaults; launch args still override).
    try:
        grid_path = os.path.join(cfg["data_dir"], "camera_grid.json")
        with open(grid_path, "w") as f:
            json.dump({
                "cameras": resolved,
                "profile": {
                    "warmup_frames": int(cfg.get("pie_warmup_ticks", 30)),
                    "settle_frames": int(cfg.get("pie_settle_ticks", 24)),
                    "capture_seconds": float(cfg.get("capture_seconds", 1.0)),
                    "channels": cfg.get("trace_channels", "cpu,gpu,frame,bookmark"),
                },
            }, f, indent=2)
        unreal.log(f"[spawn] wrote {len(resolved)} resolved cameras -> {grid_path}")
    except Exception as exc:
        unreal.log_warning(f"[spawn] could not write camera_grid.json: {exc}")

    unreal.log(f"[spawn] spawned {len(spawned)} cameras "
               f"({nudged} nudged onto navmesh), skipped {skipped}.")
    return spawned


# ----------------------------------------------------------------------------
# 3. PROFILE  (tick-driven so a frame renders between cameras)
#    Two modes (CONFIG["profile_mode"]):
#      "editor" -> move the editor viewport (fast, editor render path = costlier)
#      "pie"    -> Play-In-Editor + player view target (game render path = accurate)
# ----------------------------------------------------------------------------
def _apply_scalability(world, cfg):
    """Run one-off console commands (e.g. game sg.* scalability) once at profile start."""
    for cmd in cfg.get("scalability_commands", []) or []:
        unreal.SystemLibrary.execute_console_command(world, cmd)
        unreal.log(f"[profile] applied: {cmd}")


def _capture_camera(world, index, cfg, channels):
    """Start a per-camera Unreal Insights trace, then take the screenshot + memreport.
    The caller stops the trace after letting it record a few frames.

    NOTE: pass the trace path UNQUOTED with forward slashes -- Trace.File treats a quoted
    path as relative (it sees the leading '"') and writes into the engine Binaries dir,
    which fails. Our path has no spaces, so no quotes are needed.
    """
    trace_path = f"{cfg['trace_dir'].rstrip('/')}/camera_{index:03d}.utrace"
    try:  # Trace.File refuses to overwrite; remove a stale file from a previous run.
        if os.path.isfile(trace_path):
            os.remove(trace_path)
    except OSError:
        pass
    unreal.SystemLibrary.execute_console_command(world, f"Trace.File {trace_path} {channels}")
    unreal.SystemLibrary.execute_console_command(world, "memreport -full")
    shot = os.path.join(cfg["screenshot_dir"], f"camera_{index:03d}.png")
    res_x, res_y = cfg["screenshot_res"]
    unreal.AutomationLibrary.take_high_res_screenshot(res_x, res_y, shot)


def _manifest_entry(index, loc, rot, cfg):
    """One camera->artifacts record for camera_traces.json (consumed by the heat map)."""
    return {
        "index": index,
        "trace": f"{cfg['trace_dir'].rstrip('/')}/camera_{index:03d}.utrace",
        "screenshot": f"{cfg['screenshot_dir'].rstrip('/')}/camera_{index:03d}.png",
        "location": [round(loc.x, 1), round(loc.y, 1), round(loc.z, 1)],
        "rotation": [round(rot.pitch, 2), round(rot.yaw, 2), round(rot.roll, 2)],
    }


def _write_manifest_and_heatmap(manifest, cfg):
    """Write camera_traces.json (camera->trace/screenshot map) and rebuild the heat map so the
    camera markers pick up their trace links. Called when a profiling pass finishes."""
    try:
        path = os.path.join(cfg["data_dir"], "camera_traces.json")
        with open(path, "w") as f:
            json.dump({"cameras": manifest}, f, indent=2)
        unreal.log(f"[profile] wrote camera->trace manifest ({len(manifest)} cameras): {path}")
    except Exception as exc:
        unreal.log_warning(f"[profile] manifest write failed: {exc}")
    try:
        import importlib
        import generate_heatmap as hm
        importlib.reload(hm)
        hm.generate(hm.CONFIG)
        unreal.log("[profile] heat map rebuilt with camera trace links. Re-open 'See Density Map'.")
    except Exception as exc:
        unreal.log_warning(f"[profile] heat map rebuild skipped: {exc}")


class CameraProfiler:
    """EDITOR-mode profiler. Per camera, in tick-driven phases:
        0: move the editor viewport to the camera, let it render (settle_ticks)
        1: start the trace -> camera_###.utrace, take screenshot + memreport
        2: let the trace record this view (capture_ticks), stop the trace, advance
    Uses the editor render path, which is costlier than the game -- use "pie" mode for
    shipping-representative numbers.
    """

    def __init__(self, cameras, cfg):
        self.cameras = cameras
        self.cfg = cfg
        self.world = _editor_world()
        self.editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        self.les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        self.channels = cfg.get("trace_channels", "cpu,gpu,frame,bookmark")
        self.index = 0
        self.phase = 0
        self.wait = 0
        self.manifest = []
        _ensure_dir(cfg["screenshot_dir"])
        _ensure_dir(cfg["trace_dir"])
        _apply_scalability(self.world, cfg)
        self._game_view = bool(cfg.get("editor_game_view", True))
        if self._game_view:
            try:
                self.les.editor_set_game_view(True)  # hide editor sprites/gizmos/grid
            except Exception:
                self._game_view = False
        self.handle = unreal.register_slate_post_tick_callback(self._on_tick)
        unreal.log(f"[profile] editor-mode profiling {len(cameras)} cameras (1 .utrace each).")

    def _cmd(self, command):
        unreal.SystemLibrary.execute_console_command(self.world, command)

    def stop(self):
        """Tear down the tick callback (and any live trace). Safe to call twice."""
        if self.handle is not None:
            self._cmd("Trace.Stop")  # safety: ensure no trace left running
            unreal.unregister_slate_post_tick_callback(self.handle)
            self.handle = None
            if self._game_view:
                try:
                    self.les.editor_set_game_view(False)
                except Exception:
                    pass

    def _finish(self):
        self.stop()
        _write_manifest_and_heatmap(self.manifest, self.cfg)
        unreal.log("[profile] Camera profiling complete.")

    def _on_tick(self, delta_seconds):
        if self.wait > 0:
            self.wait -= 1
            return

        if self.index >= len(self.cameras):
            self._finish()
            return

        cam = self.cameras[self.index]

        if self.phase == 0:
            self.editor.set_level_viewport_camera_info(cam.get_actor_location(), cam.get_actor_rotation())
            self.phase = 1
            self.wait = self.cfg["settle_ticks"]
            return

        if self.phase == 1:
            _capture_camera(self.world, self.index, self.cfg, self.channels)
            self.phase = 2
            self.wait = self.cfg["capture_ticks"]  # let the trace record several frames
            return

        # phase 2: stop the trace, record the manifest entry, advance.
        self._cmd("Trace.Stop")
        self.manifest.append(_manifest_entry(self.index, cam.get_actor_location(),
                                             cam.get_actor_rotation(), self.cfg))
        unreal.log(f"[profile] camera {self.index}: utrace + screenshot + memreport")
        self.phase = 0
        self.index += 1
        self.wait = 1


class PieCameraProfiler:
    """PIE-mode profiler: runs Play-In-Editor and points the player view at each camera, so the
    traces use the GAME render path + game scalability (shipping-representative numbers).

    PIE duplicates the world, so the GridCam_* we spawned in the editor world have duplicates in
    the PIE world; we match each by location and use it as the player's view target.

    Tick-driven phases:
       -1: requested PIE; wait until the game world + player controller exist, then warm up
        0: set the player view target to this camera, settle (temporal AA/Lumen converge)
        1: trace + screenshot + memreport
        2: record a few frames, stop the trace, advance
        end: stop PIE
    """

    def __init__(self, cameras, cfg):
        self.cfg = cfg
        self.editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        self.les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        self.channels = cfg.get("trace_channels", "cpu,gpu,frame,bookmark")
        # Desired transforms come from the editor cameras (final, Z-resolved).
        self.targets = [(c.get_actor_location(), c.get_actor_rotation()) for c in cameras]
        self.index = 0
        self.phase = -1   # waiting for PIE to come up
        self.wait = 0
        self.world = None
        self.pc = None
        self.pie_cams = []
        self.manifest = []
        _ensure_dir(cfg["screenshot_dir"])
        _ensure_dir(cfg["trace_dir"])
        if self.les.is_in_play_in_editor():
            self.les.editor_request_end_play()
        self.les.editor_request_begin_play()
        self.handle = unreal.register_slate_post_tick_callback(self._on_tick)
        unreal.log(f"[profile] PIE-mode: starting Play-In-Editor for {len(self.targets)} cameras...")

    def _cmd(self, command):
        unreal.SystemLibrary.execute_console_command(self.world or _editor_world(), command)

    def stop(self):
        """Tear down the tick callback, any live trace, and PIE. Safe to call twice."""
        if self.handle is not None:
            self._cmd("Trace.Stop")
            unreal.unregister_slate_post_tick_callback(self.handle)
            self.handle = None
            try:
                if self.les.is_in_play_in_editor():
                    self.les.editor_request_end_play()
            except Exception:
                pass

    def _finish(self):
        self.stop()
        _write_manifest_and_heatmap(self.manifest, self.cfg)
        unreal.log("[profile] Camera profiling complete (PIE).")

    def _nearest_pie_camera(self, loc):
        """The PIE-world CameraActor nearest to loc (the duplicate of an editor GridCam)."""
        best, best_d2 = None, None
        for a in self.pie_cams:
            p = a.get_actor_location()
            dx, dy, dz = p.x - loc.x, p.y - loc.y, p.z - loc.z
            d2 = dx * dx + dy * dy + dz * dz
            if best_d2 is None or d2 < best_d2:
                best, best_d2 = a, d2
        return best

    def _on_tick(self, delta_seconds):
        if self.wait > 0:
            self.wait -= 1
            return

        if self.phase == -1:
            # Wait until PIE is fully up: game world + player controller available.
            if not self.les.is_in_play_in_editor():
                return
            world = self.editor.get_game_world()
            if world is None:
                return
            pc = unreal.GameplayStatics.get_player_controller(world, 0)
            if pc is None:
                return
            self.world = world
            self.pc = pc
            self.pie_cams = list(unreal.GameplayStatics.get_all_actors_of_class(world, unreal.CameraActor))
            _apply_scalability(world, self.cfg)
            self.phase = 0
            self.wait = self.cfg.get("pie_warmup_ticks", 30)  # let streaming/Lumen/TSR warm up
            unreal.log(f"[profile] PIE up: {len(self.pie_cams)} camera(s) in game world; warming up...")
            return

        if self.index >= len(self.targets):
            self._finish()
            return

        loc, rot = self.targets[self.index]

        if self.phase == 0:
            cam = self._nearest_pie_camera(loc)
            if cam is not None:
                self.pc.set_view_target_with_blend(cam, 0.0)
            else:
                self.pc.set_control_rotation(rot)  # fallback: no duplicate found
            self.phase = 1
            self.wait = self.cfg.get("pie_settle_ticks", 24)
            return

        if self.phase == 1:
            _capture_camera(self.world, self.index, self.cfg, self.channels)
            self.phase = 2
            self.wait = self.cfg["capture_ticks"]
            return

        # phase 2: stop the trace, record the manifest entry, advance.
        self._cmd("Trace.Stop")
        self.manifest.append(_manifest_entry(self.index, loc, rot, self.cfg))
        unreal.log(f"[profile] camera {self.index}: utrace + screenshot + memreport (PIE)")
        self.phase = 0
        self.index += 1
        self.wait = 1


# Keep a reference so the callback object isn't garbage-collected mid-run.
_ACTIVE_PROFILER = None


def _start_profile(cameras, cfg):
    """Begin the tick-driven profiling pass. If cameras is empty, profile every camera in the
    outliner folder (spawned GridCam_* plus any you added by hand)."""
    if not cameras:
        cameras = _folder_cameras(cfg)
    if not cameras:
        unreal.log_warning("[profile] no cameras to profile. Run 'Generate Cameras' first.")
        return
    global _ACTIVE_PROFILER
    # Stop any profiler still running (e.g. the menu entry was clicked twice) so two
    # tick callbacks don't fight over Trace.File / the viewport / PIE.
    if _ACTIVE_PROFILER is not None:
        _ACTIVE_PROFILER.stop()
    mode = str(cfg.get("profile_mode", "pie")).lower()
    if mode == "pie":
        _ACTIVE_PROFILER = PieCameraProfiler(cameras, cfg)
    else:
        _ACTIVE_PROFILER = CameraProfiler(cameras, cfg)


def main(cfg):
    if cfg["RUN_EXPORT"]:
        export_scene_data(cfg)

    cameras = []
    if cfg["RUN_SPAWN"]:
        cameras = spawn_cameras(cfg)

    if cfg["RUN_PROFILE"]:
        _start_profile(cameras, cfg)


if __name__ == "__main__":
    main(CONFIG)
