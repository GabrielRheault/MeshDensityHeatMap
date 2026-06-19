# Camera Profiling

A NavMesh-aware **camera-grid performance profiler** for Unreal Engine 5.6, plus a 2D
**asset-density heat map** to see *where* a level is heavy before you even profile.

It ships as **two C++ tools** in one plugin:

| Tool | Module | Runs in | What it does |
|------|--------|---------|--------------|
| **Editor tool** | `CameraProfilingEditor` (Editor) | The Unreal Editor | Generate a camera grid over the NavMesh, render a top-down map, build the density heat map, and inspect what's in any cell. |
| **Profiler** | `CameraProfilingEditor` (PIE) + `CameraProfilingRuntime` (standalone) | Play-In-Editor **or** a packaged/console build | Fly the view through every camera and capture one Unreal Insights `.utrace` + screenshot + memreport per camera. |

The heat map is a self-contained HTML file you can open in any browser — no server, no install.

---

## Table of contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Tool 1 — Editor: grid + heat map](#tool-1--editor-grid--heat-map)
- [Tool 2 — Profiler](#tool-2--profiler)
  - [PIE mode (from the editor)](#a-pie-mode-from-the-editor)
  - [Standalone / packaged / console mode](#b-standalone--packaged--console-mode)
- [The heat map](#the-heat-map)
- [Settings](#settings)
- [Output files](#output-files)
- [Building from source](#building-from-source)
- [Notes & limitations](#notes--limitations)

---

## Requirements

- **Unreal Engine 5.6** (C++ project — you need to be able to compile the project).
- A level with a **`NavMeshBoundsVolume`** and a **built NavMesh** (Build → Build Paths) if you want
  NavMesh-aware camera placement. Without one, the plugin falls back to whole-scene bounds.

## Installation

1. Copy the `CameraProfiling` folder into your project's `Plugins/` directory:
   `YourProject/Plugins/CameraProfiling/`.
2. Regenerate project files (right-click the `.uproject` → *Generate Visual Studio project files*).
3. Build **Development Editor**.
4. Open the project. The plugin is enabled by default; you'll find the menu under
   **Tools → _\<YourProject\>_ → Camera Profiling**.

---

## Tool 1 — Editor: grid + heat map

Everything is driven from **Tools → _\<YourProject\>_ → Camera Profiling**:

```
Tools
└── <YourProject>
    └── Camera Profiling
        ├── 1. Generate Cameras ▸  (2×2 … 40×40, or "From Settings")
        ├── 2. Profile from Cameras
        ├── 3. See Density Map
        └── Refresh Heat Map Data (rescan level, keep cameras)
```

### Steps

1. **Open the level** you want to profile and make sure the **NavMesh is built**.
2. **`1. Generate Cameras`** → pick a grid size (e.g. `10 × 10`). This:
   - scans the level into `scene_data.json` (bounds, NavMesh volumes, per-cell triangle/draw density),
   - renders an orthographic **top-down image** of the level (`map_topdown.png`) for the heat-map background,
   - lays a **jittered grid** over the NavMesh (points off the NavMesh are dropped/nudged back on),
   - aims each camera at the nearest dense asset cluster,
   - spawns `GridCam_###` actors into a **`CameraGrid`** World Outliner folder.
3. **`3. See Density Map`** → builds and opens `density_heatmap.html` in your browser.
   Use this any time to (re)open the heat map.
4. **`Refresh Heat Map Data`** → re-scan the level into the heat map **without** regenerating or moving
   cameras. Use it after you've edited the level but want to keep your camera layout.

> **Prefer a panel?** *Tools → … → Camera Profiling → Camera Profiling Panel…* opens a dockable tab with
> all the settings (grid size, **Bounds Source**, placement, navmesh options, …) and the four action
> buttons in one place — set the grid up there and hit **Generate** without touching the menu presets.
> The same values also live in **Project Settings → Yes Chef → Camera Profiling** (see [Settings](#settings)).

---

## Tool 2 — Profiler

The profiler flies the view through every `GridCam_###` and, **per camera**, captures:

- one **Unreal Insights `.utrace`** (`traces/camera_###.utrace`),
- one **screenshot** (`screenshots/camera_###.png`),
- one **memreport**.

It then writes `camera_traces.json` and rebuilds the heat map so each camera marker links to its trace.

There are two ways to run it.

### A. PIE mode (from the editor)

Profiles through the **game render path** (game scalability), so the numbers are
shipping-representative. This is the default.

1. Run **`1. Generate Cameras`** first (you need spawned `GridCam_###`).
2. **`2. Profile from Cameras`**. The plugin starts Play-In-Editor, points the player's view at each
   camera in turn (warm-up → settle → trace + screenshot + memreport → next), then ends PIE.
3. When it finishes, run **`3. See Density Map`** again — the camera dots now link to their traces.
4. Click a camera dot → it shows the screenshot + the `.utrace` path + a **Copy trace path** button.
   Paste that path into **Unreal Insights → Open Trace File**.

> Prefer the editor render path instead? Set **Profile Mode = Editor** in Settings (faster, but the
> editor render path is costlier than the game, so the numbers are less representative).

### B. Standalone / packaged / console mode

For true, editor-free numbers (and the only option on console), use the **runtime subsystem**. It runs
in any `Game`/`PIE` world when launched with `-RunCameraProfile`, reads `camera_grid.json`, and records
**one continuous trace** with a **bookmark per camera** (plus a screenshot per camera and a single
memreport at the end).

1. Run **`1. Generate Cameras`** in the editor once to produce `camera_grid.json`
   (in `<Project>/Saved/CameraProfiling/data/`). Ship/copy that file with the build if needed.
2. Launch the game with the profiling arg:

   ```bat
   YourGame.exe <Map> -RunCameraProfile -trace=cpu,gpu,frame,bookmark -windowed -ResX=1920 -ResY=1080 -log
   ```

   Or, to test from a Development Editor build without packaging:

   ```bat
   UnrealEditor.exe YourProject.uproject <Map> -game -RunCameraProfile -windowed -ResX=1920 -ResY=1080 -log
   ```

3. Open the resulting trace in **Unreal Insights**. Navigate cameras via the **Log** panel filtered to
   `camera` — double-click a `Bookmark`-category row to jump to that camera's frames.

#### Launch arguments

| Argument | Effect |
|----------|--------|
| `-RunCameraProfile` | **Required.** Arms the runtime profiler. |
| `-CameraProfileData=<path>` | Override the `camera_grid.json` path (default `<ProjectSaved>/CameraProfiling/data/camera_grid.json`). |
| `-trace=cpu,gpu,frame,bookmark` | Stream/record normally. If omitted, the profiler self-starts a file trace into `<ProjectSaved>/CameraProfiling/profile.utrace`. |
| `-CameraProfileMemReport` | Dump a memreport **per camera** (not recommended — a memreport stalls the game thread ~2s and corrupts that camera's trace region). |

---

## The heat map

`density_heatmap.html` is fully self-contained (all data inlined) — double-click to open.

**Controls**

- **Metric** dropdown: `Instances` · `Triangles` · `Draw calls (est.)`. The grid recolors per metric.
- **Balance colors** — log scale so sparse/medium/dense areas are distinguishable.
- **Cameras / NavMesh / Map** — toggle the camera dots, NavMesh bounds, and the top-down overlay.
- **Heat** slider — opacity of the heat over the map image.
- **FlipH / FlipV / Rot** — align the top-down image to the grid if it's mirrored/rotated.
- **scroll** = zoom · **drag** = pan · **double-click** = reset. Zooming subdivides the grid (LODs).

**Click a cell** → its instances / triangles / draw calls, rank, and the heaviest meshes in that cell.
If the editor is open, clicking a cell also **automatically selects — in the editor — only the
meshes/instances inside that cell** (individual foliage instances included; it does **not** select the
whole foliage actor), so they're already highlighted when you switch back to Unreal.

**Click a camera dot** → its frame rate (after profiling), screenshot, and `.utrace` path to copy into
Unreal Insights, plus a **Go to this camera in editor** button.

> The in-editor selection and the camera "Go to" button use a localhost bridge (the editor listens on
> `127.0.0.1:<GotoPort>`, default `30080`). With the editor closed the heat map still works fully — only
> those editor-driving actions are inert.

---

## Settings

**Project Settings → Yes Chef → Camera Profiling** (`UCameraProfilingSettings`). Highlights:

| Setting | Default | Notes |
|---------|---------|-------|
| Grid Resolution | `10 × 10` | Used by the "From Settings" preset. |
| Jitter / Padding | `0.5` / `0.05` | Random offset within a cell; inward margin. |
| Bounds Source | `NavMesh` | `NavMesh` (union of volumes), `Scene` (loaded geometry bounds), or `WorldPartition` (full authored world extent — for WP maps with no NavMeshBoundsVolume). |
| Aim At Clusters | `true` | Face the densest nearby assets. |
| Placement Method | `Raycast` | `Raycast` (down-trace) or `NavMesh` (project to navmesh). |
| Require Navmesh | `true` | Reject cameras that land on roofs/geometry off the NavMesh. |
| Height Above Ground | `250` uu | Camera height over the ground hit. |
| Cluster Cell Size | `2000` uu | Coarsest heat-map LOD / clustering bin. |
| Heatmap Subdiv | `8` | Finest LOD = `ClusterCellSize / Subdiv` (power of 2). |
| Topdown Px | `4096` | Resolution of the square top-down render. |
| Topdown Exposure Bias | `2.0` EV | Brighten dark/night scenes for the overlay. |
| Goto Port | `30080` | Localhost port for the heat-map bridge. |
| Profile Mode | `PIE` | `PIE` (game render path) or `Editor` (viewport). |
| Warmup / Settle Ticks | `30` / `24` | Frames to let streaming/Lumen/TSR converge. |
| Capture Seconds | `1.0` | Per-camera measurement window. |
| Trace Channels | `cpu,gpu,frame,bookmark` | Insights channels per `.utrace`. |
| Screenshot Res | `1920 × 1080` | |

## Output files

All under **`<Project>/Saved/CameraProfiling/data/`** (not source-controlled):

```
scene_data.json        scene bounds, NavMesh volumes, density grid (instances/triangles/draws)
camera_positions.json  the planned grid (pre-spawn)
camera_grid.json       resolved camera transforms + profiling params (read by the runtime profiler)
map_topdown.png/.json  top-down overlay image + the world AABB it covers
density_heatmap.html   the heat map (open this)
camera_traces.json     camera → trace/screenshot manifest (written after profiling)
screenshots/camera_###.png
traces/camera_###.utrace
```

## Building from source

This is a C++ plugin (two modules: `CameraProfilingRuntime` = Runtime, `CameraProfilingEditor` =
Editor). Adding it is a **structural change**:

1. Place it in `YourProject/Plugins/CameraProfiling/`.
2. Close the editor, **regenerate project files**, and build **Development Editor**.
3. The `CameraProfilingEditor` module is Editor-only and is **excluded from packaged games**; only the
   runtime profiler ships.

## Notes & limitations

- **Triangles** are read from the static mesh's LOD0 render data (true counts).
- `Trace.File` needs an **unquoted, forward-slash, absolute** path — the plugin handles this; just keep
  the output path space-free.
- The heat map runs in a browser sandbox, so it **can't launch Unreal Insights** — it gives you the
  trace path to paste in instead.
- NavMesh placement requires a **built** NavMesh; otherwise placement falls back to raycast / scene bounds.

### World Partition

The plugin works on World Partition maps, with a few things to know:

- **The editor tools only see *streamed-in* cells.** Before *Generate Cameras* / *Refresh Heat Map Data*,
  **Load All** (or load the regions you want) in the World Partition window — otherwise the density,
  bounds, and inspect only cover loaded cells.
- **No NavMeshBoundsVolume?** Set **Bounds Source = `WorldPartition`** to lay the grid over the full
  authored world (`GetEditorWorldBounds()`, which covers all cells even unloaded). Note this can be large
  and sparse, so the grid may include empty areas — `Scene` (loaded geometry) is tighter if you've loaded
  just the area you care about.
- **HLOD proxies are skipped** in export and cell-inspect, so distant cells' simplified meshes don't
  double-count against the real geometry.
- **Profiling streams as it flies:** the view camera is registered as a **streaming source**, so cells
  load around each profiled camera. Streaming + Nanite + Virtual Textures + Lumen take time to converge —
  raise **Warmup/Settle Ticks** on WP maps so traces/screenshots aren't captured mid-stream-in.
