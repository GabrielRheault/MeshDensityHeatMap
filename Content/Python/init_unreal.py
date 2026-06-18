"""
init_unreal.py  --  auto-run by UE5 on editor startup for the "Camera Profiling" plugin.

Adds:  Tools -> Yes Chef -> Camera Profiling  with three actions:
    1. Generate Cameras   -> (submenu of grid sizes) export scene data + build the camera grid
                             at the chosen resolution + spawn cameras
    2. Profile from Cameras -> write the density heat map + run the profiling sequence (utrace +
                               screenshot + memreport per camera, in PIE)
    3. See Density Map    -> (re)build and open the density heat map HTML in the browser

Grid size is chosen from the Generate Cameras submenu presets (the EditorDialogLibrary popup
proved unreliable on this build, so we use menu presets instead). "From CONFIG" uses the
grid_resolution value in generate_camera_grid.CONFIG for any custom size.

The tool scripts live next to this file in the plugin's Content/Python (UE puts it on sys.path).
Output goes to <Project>/Saved/CameraProfiling/data.

After editing THIS file, restart the editor so the menu re-registers in a fresh process
(the other scripts are importlib.reload'd on every click, so they don't need a restart).
"""

import os
import sys

import unreal

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.append(_HERE)

# Grid-size presets shown under "Generate Cameras". (label, nx, ny); None = use CONFIG.
GRID_PRESETS = [
    ("Test (2 x 2)", 2, 2),
    ("4 x 4", 4, 4),
    ("6 x 6", 6, 6),
    ("8 x 8", 8, 8),
    ("10 x 10", 10, 10),
    ("15 x 15", 15, 15),
    ("20 x 20", 20, 20),
    ("30 x 30", 30, 30),
    ("40 x 40", 40, 40),
    ("From CONFIG (grid_resolution)", None, None),
]


# --- The three menu actions ---
def generate_cameras(nx=None, ny=None):
    """Export scene data -> build the camera grid (nx x ny, or CONFIG if omitted) -> spawn."""
    import importlib
    import ue_camera_profiler as uecp
    import generate_camera_grid as grid
    importlib.reload(uecp)
    importlib.reload(grid)

    uecp.export_scene_data(uecp.CONFIG)
    uecp.capture_topdown(uecp.CONFIG)   # render the top-down map overlay

    gcfg = dict(grid.CONFIG)
    gcfg["use_cell_size"] = False
    if nx is not None and ny is not None:
        gcfg["grid_resolution"] = [int(nx), int(ny)]
    grid.generate(gcfg)

    uecp.spawn_cameras(uecp.CONFIG)


def profile_from_cameras():
    """Run the profiling sequence on the spawned cameras. When it finishes, the profiler writes
    the camera->trace manifest and rebuilds the density heat map (so camera markers get their
    trace links) -- then use 'See Density Map' to view it."""
    import importlib
    import ue_camera_profiler as uecp
    importlib.reload(uecp)
    uecp._start_profile([], uecp.CONFIG)  # [] -> profile the existing GridCam_* actors


def refresh_scene_data():
    """Re-scan the level into scene_data.json (density + per-cell vertices/draw-calls) WITHOUT
    touching the cameras (no grid regen, no respawn), then rebuild + open the heat map.
    Use this when you already have cameras and just want fresh heat-map data."""
    import importlib
    import ue_camera_profiler as uecp
    importlib.reload(uecp)
    uecp.export_scene_data(uecp.CONFIG)
    uecp.capture_topdown(uecp.CONFIG)   # render the top-down map overlay
    _build_heatmap(open_browser=True)


def see_density_map():
    """(Re)build the density heat map HTML from existing scene_data.json and open it (no rescan)."""
    _build_heatmap(open_browser=True)


def _build_heatmap(open_browser):
    import importlib
    import webbrowser
    import generate_heatmap as hm
    importlib.reload(hm)
    try:
        out = hm.generate(hm.CONFIG)
    except FileNotFoundError as exc:
        unreal.log_warning(f"[CameraProfiling] {exc}")
        return
    if open_browser:
        try:
            webbrowser.open("file:///" + os.path.abspath(out).replace("\\", "/"))
        except Exception as exc:
            unreal.log_warning(f"[CameraProfiling] heat map written but couldn't auto-open: {exc}")
    unreal.log(f"[CameraProfiling] heat map: {out}")


# --- Menu registration ---
def _add_entry(menu, name, label, python):
    entry = unreal.ToolMenuEntry(name=name, type=unreal.MultiBlockType.MENU_ENTRY)
    entry.set_label(label)
    entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", python)
    menu.add_menu_entry("Actions", entry)


def _project_name():
    """Current project's name (e.g. 'YesChef'), so the menu folder adapts to whatever project
    the plugin is dropped into."""
    try:
        stem = os.path.splitext(os.path.basename(unreal.Paths.get_project_file_path()))[0]
        if stem:
            return stem
    except Exception:
        pass
    try:
        return os.path.basename(os.path.normpath(unreal.Paths.project_dir())) or "Project"
    except Exception:
        return "Project"


def register():
    menus = unreal.ToolMenus.get()
    tools = menus.find_menu("LevelEditor.MainMenu.Tools")
    if not tools:
        unreal.log_warning("[CameraProfiling] Tools menu not found; menu not registered.")
        return

    # Top folder is labelled with the current project's name (adapts when shared to another project).
    top = tools.add_sub_menu("LevelEditor.MainMenu.Tools", "", "ProjectTools", _project_name())
    cam = top.add_sub_menu("ProjectTools", "", "CameraProfiling", "Camera Profiling")

    # 1. Generate Cameras -> submenu of grid-size presets.
    gen = cam.add_sub_menu("CameraProfiling", "", "GenerateCameras", "1. Generate Cameras")
    for i, (label, nx, ny) in enumerate(GRID_PRESETS):
        args = "" if nx is None else f"{nx}, {ny}"
        _add_entry(gen, f"Grid{i}", label,
                   f"import init_unreal as i; i.generate_cameras({args})")

    # 2 & 3 -> direct actions.
    _add_entry(cam, "ProfileCameras", "2. Profile from Cameras",
               "import init_unreal as i; i.profile_from_cameras()")
    _add_entry(cam, "SeeDensityMap", "3. See Density Map",
               "import init_unreal as i; i.see_density_map()")
    _add_entry(cam, "RefreshData", "Refresh Heat Map Data (rescan level, keep cameras)",
               "import init_unreal as i; i.refresh_scene_data()")

    menus.refresh_all_widgets()
    unreal.log("[CameraProfiling] menu registered (Tools -> Yes Chef -> Camera Profiling).")


try:
    register()
except Exception as exc:  # never block editor startup on a menu error
    unreal.log_warning(f"[CameraProfiling] menu registration failed: {exc}")

# Localhost bridge so the heat map's "Go to this cell" button can move the editor viewport.
try:
    import goto_server
    goto_server.start()
except Exception as exc:
    unreal.log_warning(f"[CameraProfiling] goto bridge not started: {exc}")
