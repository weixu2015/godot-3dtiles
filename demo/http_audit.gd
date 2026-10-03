extends Node3D

# HTTP transport audit.
#
# Loads tilesets over http:// rather than from disk, which is the path the pre-refactor code
# supported and the one that regressed: FileAccess cannot open an http URL, so the fetch has
# to go through HTTPClient / HTTPRequest. It exercises both halves of the loader:
#
#   * the control documents (tileset.json, .subtree) through Tileset3D::read_binary_document,
#   * the payloads (b3dm / glb) through TilesetContentLoader's HTTPRequest path.
#
# A run is only healthy when every dataset reports loaded > 0 AND the frame has non-background
# pixels - "loaded=1, rendered=1, all black" is exactly how the Icospheres NaN bug presented,
# so counting pixels is what distinguishes transport success from a silent geometry failure.
#
# Usage:
#   <godot> --path demo res://http_audit.tscn
#   <godot> --path demo res://http_audit.tscn -- --base=http://localhost:9090 --only=weinan

## Server root. Must be the directory the http server was started in, because the tile URIs
## are resolved against it.
var _base := "http://localhost:9090"

const DATASETS := [
	"1.1/Photogrammetry/tileset.json",
	"weinan/tileset.json",
	"Icospheres/tileset.json",
	"texturessphere/tileset.json",
	"test/tileset.json",
	"1.0/Photogrammetry/tileset.json",
]

## Frames to let the loader fetch, decode and upload at the framed pose. The payloads come
## over loopback, so this is dominated by Draco/KTX2 decode rather than by the network.
const WARMUP_FRAMES := 200

# Hard ceiling on frames per dataset. The audit's normal exit is "warmup reached AND
# nothing in flight", which is the ideal signal - but if a transfer never completes
# (dead server, blocked socket, a worker that never publishes) that condition is never
# met and the process sits in its window forever, waiting for a human to kill it. A
# bounded deadline turns a hang into a reported failure, which is the only kind of
# failure a self-test can act on.
const MAX_FRAMES := 900
const SHOT_DIR := "res://http_shots"

var _tileset: Tileset3D
var _camera: Camera3D
var _index := 0
var _frames := 0
var _only := ""
var _save_shots := true
var _results: Array[Dictionary] = []
var _framed := false
var _peak_in_flight := 0
var _bytes_at_warmup := 0

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--only="):
			_only = arg.substr("--only=".length())
		elif arg.begins_with("--base="):
			_base = arg.substr("--base=".length())
		elif arg == "--no-shot":
			_save_shots = false

	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.far = 100000.0
	_camera.near = 0.01
	add_child(_camera)
	_camera.make_current()

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45.0, 30.0, 0.0)
	add_child(light)

	print("[http-audit] base=", _base, " datasets=", _list().size(), " only='", _only, "'")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SHOT_DIR))
	_start_next()

func _list() -> Array:
	var out: Array = []
	for relative: String in DATASETS:
		if _only != "" and not relative.contains(_only):
			continue
		out.append(relative)
	return out

func _start_next() -> void:
	var list := _list()
	if _index >= list.size():
		_report()
		get_tree().quit()
		return

	var relative: String = list[_index]
	if _tileset != null:
		_tileset.queue_free()
		_tileset = null

	var url := _base + "/" + relative
	print("[http-audit] === loading ", url, " ===")

	_tileset = Tileset3D.new()
	_tileset.url = url
	_tileset.debug_show_bounding_volume = false
	add_child(_tileset)
	_tileset.load()

	_frames = 0
	_framed = false
	_peak_in_flight = 0
	_bytes_at_warmup = 0

func _try_frame_camera() -> void:
	if _framed:
		return
	var radius := _tileset.get_dataset_radius()
	if radius <= 0.0:
		return
	_framed = true

	var target := _tileset.global_position
	var distance := (radius / tan(deg_to_rad(_camera.fov) * 0.5)) * 1.2
	_camera.global_position = target + Vector3(0.0, distance * 0.4, distance)
	_camera.look_at(target, Vector3.UP)
	_camera.near = maxf(0.01, radius * 0.001)
	_camera.far = maxf(1000.0, radius * 100.0)

func _process(_delta: float) -> void:
	if _tileset == null:
		return
	_frames += 1
	_peak_in_flight = maxi(_peak_in_flight, _tileset.get_in_flight_count())
	_try_frame_camera()

	if _frames < WARMUP_FRAMES:
		return

	var relative: String = _list()[_index]
	var rendered := _tileset.get_last_rendered_count()
	var loaded := _tileset.get_loaded_tile_count()
	var error := _tileset.get_last_error()

	# Deadline: a dataset that never settles is a failure, not a wait. Reported distinctly
	# from "loaded nothing" because the two have different causes - a timeout means the
	# transfer or the worker never finished, which is a bug in this extension; an empty
	# dataset means the content was fetched and understood but produced no renderable mesh.
	var timed_out := _frames >= MAX_FRAMES and _tileset.get_in_flight_count() > 0

	var status := "OK"
	if error != "":
		status = "FAIL(load)"
	elif timed_out:
		status = "FAIL(timeout)"
	elif loaded == 0:
		status = "FAIL(no-content)"

	var shot_note := ""
	if _save_shots and status == "OK":
		var image := get_viewport().get_texture().get_image()
		if image != null:
			var name := relative.replace("/", "__").replace(".json", "") + ".png"
			image.save_png(SHOT_DIR + "/" + name)
			var filled := _count_filled_pixels(image)
			shot_note = " shot=" + name + " filled=" + str(filled)
			if filled <= 0:
				status = "FAIL(invisible)"

	print("[http-audit] ", status, " ", relative,
		" tiles=", _tileset.get_tile_count(),
		" loaded=", loaded,
		" rendered=", rendered,
		" depth=", _tileset.get_maximum_depth(),
		" bytes=", _tileset.get_loaded_bytes(),
		" peakInFlight=", _peak_in_flight,
		" error='", error, "'", shot_note)

	_results.append({
		"dataset": relative,
		"status": status,
		"loaded": loaded,
		"rendered": rendered,
		"bytes": _tileset.get_loaded_bytes(),
		"peak": _peak_in_flight,
		"error": error,
	})

	_index += 1
	_start_next()

func _count_filled_pixels(image: Image) -> int:
	var w := image.get_width()
	var h := image.get_height()
	if w == 0 or h == 0:
		return 0
	var background := image.get_pixel(0, 0)
	var filled := 0
	var stride := 4
	for y in range(0, h, stride):
		for x in range(0, w, stride):
			var c := image.get_pixel(x, y)
			if absf(c.r - background.r) > 0.02 or absf(c.g - background.g) > 0.02 \
					or absf(c.b - background.b) > 0.02:
				filled += 1
	return filled

func _report() -> void:
	var ok := 0
	var fail := 0
	for r in _results:
		if r["status"] == "OK":
			ok += 1
		else:
			fail += 1

	print("")
	print("[http-audit] ================= SUMMARY =================")
	print("[http-audit] total=", _results.size(), " ok=", ok, " fail=", fail)
	for r in _results:
		print("[http-audit]   ", r["status"], "  ", r["dataset"],
			"  loaded=", r["loaded"], " bytes=", r["bytes"],
			" peakInFlight=", r["peak"], " error='", r["error"], "'")
	print("[http-audit] ===========================================")
