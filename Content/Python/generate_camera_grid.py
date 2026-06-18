"""
generate_camera_grid.py  --  pure Python (runs inside UE via the menu, or standalone).

Reads scene_data.json (exported from UE), builds a jittered 2D grid of camera positions over
the level's NavMeshBoundsVolume(s) (or scene bounds), aims each camera at the nearest dense
asset cluster, and writes camera_positions.json for the UE side to spawn.

Output paths are derived from this file's location (-> <Project>/Saved/CameraProfiling/data),
so the plugin is portable. All distances are in Unreal units (cm).
"""

import json
import math
import os
import random

# Paths derived from this file: .../Plugins/CameraProfiling/Content/Python -> project root x4 up
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

    # --- Bounds ---
    # "navmesh" = lay the grid over the level's NavMeshBoundsVolume(s) and keep only points
    #             that fall inside one of them. "scene" = use the overall actor/instance bounds.
    # Falls back to "scene" automatically if no NavMeshBoundsVolume was exported.
    "bounds_source": "navmesh",

    # --- Grid layout ---
    "use_cell_size": False,
    "grid_resolution": [10, 10],   # [cells_x, cells_y]
    "cell_size": 2000.0,           # UU per cell (only when use_cell_size = True)

    # Random jitter inside each cell, as a fraction of the cell half-size (0..1).
    "jitter": 0.5,
    # Shrink the usable bounds inward by this fraction on each side (0..0.49).
    "padding": 0.05,

    # --- Camera placement ---
    # Nominal Z used as the downward-raycast START in UE (overwritten by the ground hit).
    "camera_height": 50000.0,
    # [pitch, yaw, roll] in degrees. [0,0,0] = looking forward (+X).
    "rotation": [0.0, 0.0, 0.0],

    # Aim cameras at the nearest dense asset cluster (face density, not empty space).
    "aim_at_clusters": True,
    "aim_fraction": 1.0,           # fraction of cameras that get aimed (0..1)

    "random_seed": 1234,           # None for a different layout every run
}


def _resolve_counts(min_xy, max_xy, cfg):
    """Return (nx, ny) cell counts based on config."""
    if cfg["use_cell_size"]:
        size = max(1e-3, float(cfg["cell_size"]))
        nx = max(1, round((max_xy[0] - min_xy[0]) / size))
        ny = max(1, round((max_xy[1] - min_xy[1]) / size))
        return nx, ny
    nx, ny = cfg["grid_resolution"]
    return max(1, int(nx)), max(1, int(ny))


def generate(cfg):
    data_dir = cfg["data_dir"]
    scene_path = os.path.join(data_dir, cfg["scene_data_file"])
    out_path = os.path.join(data_dir, cfg["camera_positions_file"])

    if not os.path.isfile(scene_path):
        raise FileNotFoundError(
            f"scene_data.json not found at {scene_path}\n"
            f"Run 'Generate Cameras' (which exports the scene) first."
        )

    with open(scene_path, "r") as f:
        scene = json.load(f)

    # Choose the grid extent: NavMesh volumes (union) or the whole-scene bounds.
    volumes = scene.get("navmesh_volumes") or []
    use_nav = cfg.get("bounds_source", "navmesh") == "navmesh" and len(volumes) > 0
    if use_nav:
        bmin = [min(v["min"][k] for v in volumes) for k in range(3)]
        bmax = [max(v["max"][k] for v in volumes) for k in range(3)]
    else:
        if cfg.get("bounds_source") == "navmesh":
            print("[grid] no navmesh volumes found in scene_data.json; using whole-scene bounds.")
        bmin = scene["bounds"]["min"]
        bmax = scene["bounds"]["max"]

    def inside_navmesh(px, py):
        """True if (px,py) is inside any NavMeshBoundsVolume AABB (XY)."""
        for v in volumes:
            if v["min"][0] <= px <= v["max"][0] and v["min"][1] <= py <= v["max"][1]:
                return True
        return False

    # Apply inward padding on X/Y.
    pad = max(0.0, min(0.49, float(cfg["padding"])))
    span_x = (bmax[0] - bmin[0])
    span_y = (bmax[1] - bmin[1])
    min_xy = [bmin[0] + span_x * pad, bmin[1] + span_y * pad]
    max_xy = [bmax[0] - span_x * pad, bmax[1] - span_y * pad]

    if cfg["random_seed"] is not None:
        random.seed(cfg["random_seed"])

    nx, ny = _resolve_counts(min_xy, max_xy, cfg)
    cell_w = (max_xy[0] - min_xy[0]) / nx
    cell_h = (max_xy[1] - min_xy[1]) / ny
    jitter = max(0.0, min(1.0, float(cfg["jitter"])))
    z = float(cfg["camera_height"])
    rot = [float(v) for v in cfg["rotation"]]

    clusters = scene.get("clusters") or []
    aim = bool(cfg.get("aim_at_clusters", True)) and len(clusters) > 0
    aim_fraction = max(0.0, min(1.0, float(cfg.get("aim_fraction", 1.0))))

    def yaw_to_nearest_cluster(px, py):
        """UE yaw (deg) from (px,py) toward the nearest cluster center; None if no clusters."""
        best = None
        best_d2 = None
        for c in clusters:
            cx2, cy2 = c["center"][0], c["center"][1]
            d2 = (cx2 - px) ** 2 + (cy2 - py) ** 2
            if best_d2 is None or d2 < best_d2:
                best_d2 = d2
                best = c
        if best is None:
            return None
        return math.degrees(math.atan2(best["center"][1] - py, best["center"][0] - px))

    cameras = []
    dropped = 0
    aimed = 0
    for i in range(nx):
        for j in range(ny):
            cx = min_xy[0] + (i + 0.5) * cell_w
            cy = min_xy[1] + (j + 0.5) * cell_h
            jx = (random.uniform(-1.0, 1.0)) * jitter * (cell_w * 0.5)
            jy = (random.uniform(-1.0, 1.0)) * jitter * (cell_h * 0.5)
            px, py = cx + jx, cy + jy
            if use_nav and not inside_navmesh(px, py):
                dropped += 1
                continue

            cam_rot = list(rot)
            if aim and random.random() < aim_fraction:
                yaw = yaw_to_nearest_cluster(px, py)
                if yaw is not None:
                    cam_rot = [rot[0], round(yaw, 2), rot[2]]  # keep base pitch/roll, face the cluster
                    aimed += 1

            cameras.append({
                "location": [round(px, 2), round(py, 2), z],
                "rotation": cam_rot,  # [pitch, yaw, roll]
            })

    payload = {
        "config": {
            "grid": [nx, ny],
            "cell_size": [round(cell_w, 2), round(cell_h, 2)],
            "jitter": jitter,
            "padding": pad,
            "nominal_z": z,
            "bounds_source": "navmesh" if use_nav else "scene",
        },
        "camera_count": len(cameras),
        "cameras": cameras,
    }

    os.makedirs(data_dir, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(payload, f, indent=2)

    src = f"navmesh ({len(volumes)} volume(s))" if use_nav else "scene bounds"
    print(f"[grid] extent from {src}: X[{bmin[0]:.0f},{bmax[0]:.0f}] Y[{bmin[1]:.0f},{bmax[1]:.0f}]")
    print(f"[grid] {nx} x {ny} grid -> {len(cameras)} cameras"
          + (f" ({dropped} dropped outside navmesh)" if use_nav else "")
          + f"  (cell {cell_w:.0f} x {cell_h:.0f}, jitter {jitter})")
    if aim:
        print(f"[grid] aimed {aimed}/{len(cameras)} cameras at {len(clusters)} asset cluster(s).")
    else:
        print("[grid] cluster aiming off (no clusters or disabled); using base rotation.")
    print(f"[grid] wrote {out_path}")
    return out_path


if __name__ == "__main__":
    generate(CONFIG)
