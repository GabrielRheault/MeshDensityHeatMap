# Camera Profiling

A **camera-grid performance profiler** for Unreal Engine 5.6, plus a 2D **asset-density heat map** to
see *where* a level is heavy — geometry **and lights** — before you even profile.

It ships as **two C++ modules** in one plugin:

| Module | Runs in | What it does |
|--------|---------|--------------|
| **`CameraProfilingEditor`** | The Unreal Editor (+ PIE) | Lay a camera grid over the level, render a top-down map, build the density heat map, inspect any cell, and run PIE profiling. |
| **`CameraProfilingRuntime`** | A packaged/standalone/console game | Fly the player view through every camera and record one continuous Unreal Insights trace (bookmark + screenshot per camera). |

The heat map is a self-contained HTML file you open in any browser — no server, no install.

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

- **Unreal Engine 5.6** (C++ project — you must be able to compile it).
- A level to profile. **NavMesh is optional**: it's only used when **Bounds Source = `NavMesh`** (see
  [Settings](#settings)). In `Scene` / `WorldPartition` mode cameras are placed by raycast alone, so no
  NavMeshBoundsVolume or built navmesh is needed.

## Installation

1. Copy the `CameraProfiling` folder into your project's `Plugins/` directory:
   `YourProject/Plugins/CameraProfiling/`.
2. Regenerate project files (right-click the `.uproject` → *Generate Visual Studio project files*).
3. Build **Development Editor** (see [Building from source](#building-from-source) — a command-line build
   is more reliable than the IDE on large projects).
4. Open the project. The plugin is enabled by default; open it from **Tools → Camera Profiling**.

---

## Tool 1 — Editor: grid + heat map

**Tools → Camera Profiling** opens a dockable **panel** that holds every setting (grid size, Bounds
Source, placement, heat-map, profiling) and three action buttons:

```
Camera Profiling panel
├── … all settings (also in Project Settings → Yes Chef → Camera Profiling) …
├── 1. Generate Data (cameras + JSON)
├── 2. Run Camera Profiling
└── 3. Open Heat Map
```

Every **Generate Data** run is archived to `data/history/<timestamp>/`, so you build up a log of
generations. You switch between them **from the heat map itself**, via the **Generation** dropdown (see
[The heat map](#the-heat-map)) — handy for comparing a level before/after an optimization pass. (History
accumulates on disk; delete `data/history/` to clear it.)

### Steps

1. **Open the level** you want to profile. (On World Partition maps, **Load** the regions you care about
   first — see [World Partition](#world-partition).)
2. **`1. Generate Data (cameras + JSON)`** — set the grid size in the panel (e.g. `10 × 10`), then click
   it. This:
   - scans the level into `scene_data.json` (bounds, NavMesh volumes, per-cell **instances / triangles /
     draw-call / light** density, asset clusters),
   - renders an orthographic **top-down image** (`map_topdown.png`) for the heat-map background,
   - lays a **jittered grid** over the chosen **Bounds Source**, aims each camera toward the local
     **density concentration** (every asset cluster pulls the facing by weight × distance-falloff, so
     cameras face the hot zones), resolves each camera's ground by raycast (or navmesh projection in
     `NavMesh` mode),
   - spawns `GridCam_###` actors into a **`CameraGrid`** World Outliner folder.
3. **`2. Run Camera Profiling`** *(optional)* — fly the view through every camera, capturing a trace +
   screenshot + frame timing each (see [Tool 2](#tool-2--profiler)).
4. **`3. Open Heat Map`** — builds and opens `density_heatmap.html` in your browser (from the latest
   generated/profiled data). Use any time to (re)open the map.

> Camera placement follows **Bounds Source**: `NavMesh` projects each camera onto the navmesh and rejects
> off-navmesh points; `Scene` / `WorldPartition` place purely by down-trace. If every camera is "skipped:
> down-trace hit NO collision," the geometry under the grid has collision disabled — switch areas or check
> the level's collision.

---

## Tool 2 — Profiler

The profiler flies the view through every `GridCam_###` (the cameras in the **`CameraGrid`** folder) and,
**per camera**, captures a screenshot + an Unreal Insights timing region, then writes `camera_traces.json`
and rebuilds the heat map so each camera marker links to its data.

There are two ways to run it.

### A. PIE mode (from the editor)

Profiles through the **game render path** (game scalability), so the numbers are
shipping-representative. This is the default.

1. Run **`1. Generate Data (cameras + JSON)`** first (you need spawned `GridCam_###`).
2. **`2. Run Camera Profiling`** — starts Play-In-Editor, points the player's view at each camera in turn
   (warm-up → settle → `.utrace` + screenshot → next), then ends PIE. One `.utrace` per camera.
3. When it finishes, run **`3. Open Heat Map`** again — the camera dots now carry their screenshot +
   trace links.
4. Click a camera dot → screenshot thumbnail + the `.utrace` path + a **Copy trace path** button. Paste
   that path into **Unreal Insights → Open Trace File**.

> Prefer the editor render path? Set **Profile Mode = Editor** in Settings (faster, but the editor render
> path is costlier than the game, so the numbers are less representative).

### B. Standalone / packaged / console mode

For true, editor-free numbers (and the only option on console), use the **runtime subsystem**. It runs in
any `Game`/`PIE` world launched with `-RunCameraProfile`, reads `camera_grid.json`, and records **one
continuous trace** with a **bookmark per camera** (plus a screenshot per camera, a measured ms/fps per
camera, and a single memreport at the end).

1. Run **`1. Generate Data (cameras + JSON)`** in the editor once to produce `camera_grid.json`
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
| `-CameraProfileWarmup=<n>` / `-CameraProfileSettle=<n>` / `-CameraProfileCaptureSecs=<s>` | Override warm-up / settle frames and the per-camera measurement window. |
| `-trace=cpu,gpu,frame,bookmark` | Stream/record normally. If omitted, the profiler self-starts a file trace into `<ProjectSaved>/CameraProfiling/profile.utrace`. |
| `-CameraProfileMemReport` | Dump a memreport **per camera** (not recommended — a memreport stalls the game thread ~2s and corrupts that camera's trace region). |
| `-CameraProfileNoUncap` | Keep the project's V-Sync / MaxFPS instead of uncapping (uncapped is the default so the measured FPS is real). |

---

## The heat map

`density_heatmap.html` is fully self-contained (all data inlined) — double-click to open.

**Controls**

- **Metric** dropdown — recolors the grid by:
  - `Instances` — placement count per cell.
  - `Triangles` — LOD0 triangles per cell.
  - `Draw calls (est.)` — material-section count (instanced meshes batch to one set per component).
  - `Lights (overlap cost)` — see below.
- **Balance colors** — log scale, so sparse / medium / dense areas stay distinguishable instead of
  everything but the hottest cell looking the same.
- **Cameras / NavMesh / Map** — toggle the camera dots, NavMesh bounds, and the top-down image overlay
  (the map is **on by default** when an image exists).
- **Heat** slider — opacity of the heat over the map image.
- **Generation** dropdown — switch the map to a past **Generate Data** run (from `data/history/`). Picking
  one asks the editor to restore that generation and the page reloads showing it. Only appears while the
  **editor is open** (it goes through the localhost bridge, since a `file://` page can't read local files).
- **scroll** = zoom · **drag** = pan · **double-click** = reset. Zooming subdivides the grid (LODs); only
  on-screen cells are drawn, so it stays fast on big maps.

**The Lights metric** stamps each **dynamic local light's** attenuation radius into the grid, so
overlapping lights accumulate — a cell lit by *N* lights reads ~*N* (overdraw = the real lighting cost).
Per-light weight: **Static skipped** (baked), **Stationary ×0.5**, **Movable ×1.0**; **shadow-casters ×2**,
and **point-light shadows ×6** (cubemap vs a single shadow map). Directional/sky lights are excluded
(global, no radius).

**Click a cell** → its value for the **active metric** (the panel shows only that metric's row), plus
rank, % of densest, center, and cell size. A **See in Unreal** button then selects — in the editor —
**only the meshes/instances inside that cell** (individual foliage instances included; it does **not**
select the whole foliage actor) and lists the heaviest meshes, each tagged with its **LOD count** (or
**Nanite**, which does continuous LOD at runtime).

**Click a camera dot** → its frame rate (after profiling), screenshot thumbnail, and `.utrace` path to
copy into Unreal Insights, plus a **Go to this camera in editor** button that snaps the level viewport to
that camera's exact transform.

> In-editor selection, the camera "Go to", and the Generation dropdown use a localhost bridge (the editor
> listens on `127.0.0.1:<GotoPort>`, default `30080`; endpoints `/inspectcell`, `/gotocam`, `/snapshots`,
> `/loadsnapshot`). With the editor closed the heat map still **views** fully — only those editor-driving
> actions are inert.

---

## Settings

**Project Settings → Yes Chef → Camera Profiling** (`UCameraProfilingSettings`), or the panel. Highlights:

| Setting | Default | Notes |
|---------|---------|-------|
| Grid Resolution | `10 × 10` | Cells laid over the bounds. |
| Jitter / Padding | `0.5` / `0.05` | Random offset within a cell; inward margin. |
| Bounds Source | `Scene` | `Scene` (loaded geometry bounds), `NavMesh` (union of volumes), or `WorldPartition` (full authored world extent — for WP maps with no NavMeshBoundsVolume). **Also decides placement:** only `NavMesh` mode projects cameras onto the navmesh and rejects off-navmesh points; `Scene`/`WorldPartition` place by raycast alone. |
| Aim At Clusters | `true` | Face the densest nearby assets. |
| Height Above Ground | `250` uu | Camera height over the ground hit. |
| Placement Retries | `16` | Ring offsets tried within a cell before giving up (NavMesh mode also snaps to the nearest navmesh point). |
| Cluster Cell Size | `2000` uu | Coarsest heat-map LOD / clustering bin. |
| Heatmap Subdiv | `8` | Finest LOD = `ClusterCellSize / Subdiv` (power of 2). |
| Topdown Px | `2048` | Resolution of the square top-down render. |
| Topdown Margin | `1.3` | Extent multiplier for the top-down render (1.0 = exactly the content box). |
| Goto Port | `30080` | Localhost port for the heat-map bridge. |
| Profile Mode | `PIE` | `PIE` (game render path) or `Editor` (viewport). |
| Warmup / Settle Ticks | `30` / `24` | Frames to let streaming/Lumen/TSR converge. |
| Capture Seconds | `1.0` | Per-camera measurement window. |
| Trace Channels | `cpu,gpu,frame,bookmark` | Insights channels per `.utrace`. |
| Screenshot Res | `1920 × 1080` | Per-camera screenshot resolution. |

## Output files

All under **`<Project>/Saved/CameraProfiling/data/`** (not source-controlled):

```
scene_data.json        bounds, NavMesh volumes, clusters, density grid (instances/triangles/draws/lights)
camera_positions.json  the planned grid (pre-spawn)
camera_grid.json       resolved transforms of the CameraGrid-folder cameras + profiling params
map_topdown.png/.json  top-down overlay image + the world AABB it covers
density_heatmap.html   the heat map (open this)
camera_traces.json     camera → trace/screenshot/fps manifest (written after profiling)
diagnostics.txt        engine/world/settings + export/spawn summary of the last run (send this for support)
screenshots/camera_###.png
traces/camera_###.utrace
history/<timestamp>/    archived copy of each Generate Data run (switch back to it from the heat map)
```

> **Reporting a problem?** Send **`diagnostics.txt`** (and the browser console if the heat map misbehaves)
> — it has the engine/world/settings and the export/spawn results, which is usually enough to diagnose
> without the level. It's rewritten by every **Generate Data**.

## Building from source

This is a C++ plugin (two modules: `CameraProfilingRuntime` = Runtime, `CameraProfilingEditor` = Editor).
Adding it is a **structural change** — close the editor, regenerate project files, and build.

On large projects the Visual Studio IDE can crash just loading the solution (IntelliSense + memory). A
**command-line build is more reliable** and needs no IDE — with the editor closed:

```bat
"C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" YourProjectEditor Win64 Development -Project="C:\path\to\YourProject.uproject" -WaitMutex
```

The `CameraProfilingEditor` module is Editor-only and is **excluded from packaged games**; only the
runtime profiler ships.

> Header changes (e.g. adding/removing a `UPROPERTY`) need a full rebuild with the editor closed — Live
> Coding can't reliably hot-reload them.

## Notes & limitations

- **Triangles** are read from the static mesh's LOD0 render data (true counts); the cell breakdown also
  reports each mesh's **LOD count / Nanite** status so a high triangle load can be read in context.
- **Draw calls** are an estimate (material-section count); instanced meshes batch to one section-set per
  component, so the metric weights them once, not per instance.
- The **Lights** metric reflects *dynamic* lighting cost only (Static/baked lights are excluded).
- `Trace.File` needs an **unquoted, forward-slash, absolute** path — the plugin handles this; just keep
  the output path space-free.
- The heat map runs in a browser sandbox, so it **can't launch Unreal Insights** — it gives you the trace
  path to paste in instead.

### World Partition

The plugin works on World Partition maps, with a few things to know:

- **The editor tools only see *streamed-in* cells.** Before *Generate Data*,
  **Load All** (or load the regions you want) in the World Partition window — otherwise the density,
  bounds, and inspect only cover loaded cells.
- **No NavMeshBoundsVolume?** Set **Bounds Source = `WorldPartition`** to lay the grid over the full
  authored world (`GetEditorWorldBounds()`, which covers all cells even unloaded). It can be large and
  sparse, so the grid may include empty areas — `Scene` (loaded geometry) is tighter if you've loaded just
  the area you care about.
- **HLOD proxies are skipped** in export and cell-inspect, so distant cells' simplified meshes don't
  double-count against the real geometry.
- **Profiling streams as it flies:** the view camera is registered as a **streaming source**, so cells
  load around each profiled camera. Streaming + Nanite + Virtual Textures + Lumen take time to converge —
  raise **Warmup/Settle Ticks** on WP maps so traces/screenshots aren't captured mid-stream-in.
