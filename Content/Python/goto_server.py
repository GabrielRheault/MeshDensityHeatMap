"""
goto_server.py  --  tiny localhost HTTP bridge so the density heat map (a browser page) can
drive the Unreal editor viewport.

A browser can't talk to UE directly (Python remote-exec is UDP; Remote Control needs object
paths + CORS). So this serves a minimal HTTP endpoint on 127.0.0.1 that the heat map's
"Go to this cell" button calls. Requests are queued and executed ON THE GAME THREAD (a slate
post-tick callback), because UE APIs must run there.

Bound to 127.0.0.1 only (not network-exposed). Started by init_unreal at editor startup.

    GET /goto?x=&y=                              -> frame that spot (oblique 3/4 view)
    GET /gotocam?x=&y=&z=&pitch=&yaw=&roll=      -> set the viewport to a camera's exact transform
    GET /inspectcell?x=&y=&size=                 -> select the cell's meshes in-editor + return a
                                                    per-mesh vertex breakdown (heaviest first)
    GET /ping                                    -> health check

Keep CONFIG['port'] in sync with generate_heatmap.CONFIG['goto_port'].
"""

import json
import queue
import threading

import unreal

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

CONFIG = {
    "host": "127.0.0.1",
    "port": 30080,
    "frame_back": 3500.0,   # how far back (along -X) the viewport sits from the target
    "frame_up": 3500.0,     # how far above the ground the viewport sits
    "nominal_z": 0.0,       # raycast start reference for finding the ground at (x, y)
}

_QUEUE = queue.Queue()
_SERVER = None
_THREAD = None
_TICK = None


def _goto_cell(x, y):
    """Frame world (x, y) with an oblique 3/4 view. Runs on the game thread."""
    import ue_camera_profiler as uecp  # reuse ground raycast + world helper
    world = uecp._editor_world()
    ground = uecp._resolve_ground_z(world, x, y, CONFIG["nominal_z"], uecp.CONFIG)
    if ground is None:
        ground = CONFIG["nominal_z"]
        unreal.log_warning(f"[goto] no ground at ({x:.0f},{y:.0f}); using z={ground:.0f}.")

    target = unreal.Vector(x, y, ground)
    cam_loc = unreal.Vector(x - CONFIG["frame_back"], y, ground + CONFIG["frame_up"])
    cam_rot = unreal.MathLibrary.find_look_at_rotation(cam_loc, target)  # oblique 3/4 look-down
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(cam_loc, cam_rot)
    unreal.log(f"[goto] viewport -> cell ({x:.0f}, {y:.0f}, {ground:.0f})")


def _goto_cam(x, y, z, pitch, yaw, roll):
    """Pilot the actual CameraActor nearest (x,y,z) so the viewport matches it EXACTLY (no height
    offset / Z guesswork). Falls back to setting the viewport transform if no camera is found.
    Runs on the game thread. Eject with Ctrl+Shift+P."""
    editor_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    best, best_d2 = None, None
    for a in editor_actors.get_all_level_actors():
        if not isinstance(a, unreal.CameraActor):
            continue
        p = a.get_actor_location()
        dx, dy, dz = p.x - x, p.y - y, p.z - z
        d2 = dx * dx + dy * dy + dz * dz
        if best_d2 is None or d2 < best_d2:
            best_d2, best = d2, a

    if best is not None:
        les.pilot_level_actor(best)  # locks the viewport to the real camera => exact transform
        unreal.log(f"[goto] piloting camera '{best.get_actor_label()}' (eject with Ctrl+Shift+P).")
    else:
        unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(
            unreal.Vector(x, y, z), unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll))
        unreal.log_warning("[goto] no CameraActor found near target; set viewport transform directly.")


def _inspect_cell(minx, miny, size):
    """Find every static-mesh placement inside the cell's world AABB, SELECT the owning actors in
    the editor, and return a per-mesh breakdown (heaviest by total vertices first). Game thread."""
    import ue_camera_profiler as uecp  # reuse the shared mesh accessors/metrics
    editor_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    maxx, maxy = minx + size, miny + size

    per_mesh = {}   # mesh name -> [placements, vertices, {actor names}]
    selection = []
    seen = set()

    for actor in editor_actors.get_all_level_actors():
        if isinstance(actor, unreal.CameraActor):
            continue
        aname = actor.get_name()
        hit_actor = False
        for comp in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = uecp._comp_static_mesh(comp)
            if mesh is None:
                continue
            verts, _mats = uecp._mesh_metrics(mesh)
            name = mesh.get_name()
            if isinstance(comp, unreal.InstancedStaticMeshComponent):
                hits = 0  # only instances whose position is INSIDE this cell
                try:
                    for i in range(comp.get_instance_count()):
                        t = comp.get_instance_transform(i, world_space=True).translation
                        if minx <= t.x < maxx and miny <= t.y < maxy:
                            hits += 1
                except Exception:
                    pass
                if hits:
                    acc = per_mesh.setdefault(name, [0, 0])
                    acc[0] += hits; acc[1] += hits * verts
                    hit_actor = True
            else:
                loc = comp.get_world_location()
                if minx <= loc.x < maxx and miny <= loc.y < maxy:  # only if in this cell
                    acc = per_mesh.setdefault(name, [0, 0])
                    acc[0] += 1; acc[1] += verts
                    hit_actor = True
        if hit_actor and aname not in seen:
            seen.add(aname)
            selection.append(actor)

    editor_actors.set_selected_level_actors(selection)
    items = sorted(per_mesh.items(), key=lambda kv: kv[1][1], reverse=True)
    # count + vertices are for THIS CELL ONLY (filtered by the cell AABB above).
    breakdown = [{"mesh": k, "count": v[0], "vertices": v[1]} for k, v in items[:12]]
    unreal.log(f"[inspect] cell ({minx:.0f},{miny:.0f}) +{size:.0f}: selected {len(selection)} actor(s); "
               f"top = {breakdown[0]['mesh'] + ' ' + format(breakdown[0]['vertices'], ',') + 'v' if breakdown else '-'}")
    return breakdown


def _tick(delta_seconds):
    # Drain queued requests on the game thread. Tagged tuples: ("cell",x,y) / ("cam",...) /
    # ("inspect", minx, miny, size, holder) where holder is filled with the result + an Event.
    try:
        while True:
            item = _QUEUE.get_nowait()
            try:
                if item[0] == "cell":
                    _goto_cell(item[1], item[2])
                elif item[0] == "cam":
                    _goto_cam(*item[1:])
                elif item[0] == "inspect":
                    holder = item[4]
                    try:
                        holder["data"] = _inspect_cell(item[1], item[2], item[3])
                    finally:
                        holder["event"].set()
            except Exception as exc:
                unreal.log_warning(f"[goto] failed: {exc}")
                if item[0] == "inspect":
                    item[4]["event"].set()
    except queue.Empty:
        pass


class _Handler(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def _reply(self, code, body, content_type="text/plain"):
        self.send_response(code)
        self._cors()
        self.send_header("Content-Type", content_type)
        self.end_headers()
        self.wfile.write(body.encode() if isinstance(body, str) else body)

    def do_GET(self):
        parsed = urlparse(self.path)
        q = parse_qs(parsed.query)

        def num(key, default=None):
            try:
                return float(q[key][0])
            except (KeyError, IndexError, ValueError):
                return default

        if parsed.path == "/goto":
            x, y = num("x"), num("y")
            if x is None or y is None:
                return self._reply(400, "need numeric x and y")
            _QUEUE.put(("cell", x, y))
            return self._reply(200, "ok")

        if parsed.path == "/gotocam":
            x, y = num("x"), num("y")
            if x is None or y is None:
                return self._reply(400, "need numeric x and y")
            _QUEUE.put(("cam", x, y, num("z", 0.0), num("pitch", 0.0), num("yaw", 0.0), num("roll", 0.0)))
            return self._reply(200, "ok")

        if parsed.path == "/inspectcell":
            x, y, size = num("x"), num("y"), num("size")
            if x is None or y is None or size is None:
                return self._reply(400, "need numeric x, y, size")
            holder = {"event": threading.Event(), "data": None}
            _QUEUE.put(("inspect", x, y, size, holder))
            holder["event"].wait(8.0)  # game thread fills it within a frame or two
            return self._reply(200, json.dumps(holder["data"] or []), "application/json")

        if parsed.path == "/ping":
            return self._reply(200, "camera-profiling")

        self.send_response(404)
        self._cors()
        self.end_headers()

    def log_message(self, *args):
        pass  # don't spam the Output Log


def start():
    """Start the bridge (idempotent). Safe to call on every editor startup."""
    global _SERVER, _THREAD, _TICK
    if _SERVER is not None:
        return
    host, port = CONFIG["host"], CONFIG["port"]
    try:
        _SERVER = ThreadingHTTPServer((host, port), _Handler)
    except OSError as exc:
        unreal.log_warning(f"[goto] could not bind {host}:{port} ({exc}); 'Go to this cell' disabled.")
        _SERVER = None
        return
    _THREAD = threading.Thread(target=_SERVER.serve_forever, daemon=True)
    _THREAD.start()
    _TICK = unreal.register_slate_post_tick_callback(_tick)
    unreal.log(f"[goto] bridge listening on http://{host}:{port}")


def stop():
    global _SERVER, _THREAD, _TICK
    if _TICK is not None:
        unreal.unregister_slate_post_tick_callback(_TICK)
        _TICK = None
    if _SERVER is not None:
        _SERVER.shutdown()
        _SERVER = None
        _THREAD = None
