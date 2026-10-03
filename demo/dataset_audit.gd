extends Node3D

# Headless audit across every dataset under E:/GISData/3D Tiles.
#
# Loads each tileset in turn with the same fixed camera, lets the scheduler run for a
# short warm-up, and reports tile / loaded / rendered counts plus any load error. The
# point is robustness: a dataset that produces a non-finite transform shows up as
# ERROR: Condition "!v.is_finite()" in the engine log, and the per-dataset summary line
# here makes it obvious which source is responsible.
#
# Usage:
#   <godot> --path demo res://dataset_audit.tscn
#   <godot> --path demo res://dataset_audit.tscn -- --only=Icospheres
#   <godot> --path demo res://dataset_audit.tscn -- --no-shot

const ROOT := "E:/GISData/3D Tiles"

## Every tileset to exercise. Kept as explicit paths rather than a directory walk so the
## list mirrors the web reference's picker and the run is reproducible.
const DATASETS := [
	"1.0/Photogrammetry/tileset.json",
	"1.1/Photogrammetry/tileset.json",
	"Icospheres/tileset.json",
	"Weinan/tileset.json",
	"taiwan/tileset.json",
	"texturessphere/tileset.json",
	"test/tileset.json",
	"Aerometrex-SanFrancisco-2cm/tileset.json",
	"3d-tiles-samples/1.0/TilesetWithDiscreteLOD/tileset.json",
	"3d-tiles-samples/1.0/TilesetWithRequestVolume/tileset.json",
	"3d-tiles-samples/1.0/TilesetWithTreeBillboards/tileset.json",
	"3d-tiles-samples/1.1/MultipleContents/tileset.json",
	"3d-tiles-samples/1.1/SparseImplicitOctree/tileset.json",
	"3d-tiles-samples/1.1/SparseImplicitQuadtree/tileset.json",
	"3d-tiles-samples/1.1/MetadataGranularities/tileset.json",
	"3d-tiles-samples/1.1/TilesetWithFullMetadata/tileset.json",
	"3d-tiles-samples/glTF/EXT_mesh_features/FeatureIdAttribute/tileset.json",
	"3d-tiles-samples/glTF/EXT_structural_metadata/ComplexTypes/tileset.json",
]

## Frames to let each dataset load and schedule before sampling the counters. Must be long
## enough for the scheduler to finish its requests at the framed camera pose; too short and a
## healthy dataset reports loaded=0 purely because it was still fetching.
const WARMUP_FRAMES := 150

# Bounded deadline. The warmup is a lower bound on how long to wait; this is the upper one.
# If a worker never finishes, the "done" condition is never met and an unbounded audit just
# parks in its window, which is a hang rather than a test result. See http_audit.gd.
const MAX_FRAMES := 900
const SHOT_DIR := "res://audit_shots"

var _tileset: Tileset3D
var _camera: Camera3D
var _index := 0
var _frames := 0
var _save_shots := true
var _only := ""
var _results: Array[Dictionary] = []
var _framed := false
var _radius := 0.0

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--only="):
			_only = arg.substr("--only=".length())
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

	print("[audit] datasets=", _dataset_list().size(), " only='", _only, "' shots=", _save_shots)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SHOT_DIR))
	_start_next()

func _dataset_list() -> Array:
	var out: Array = []
	for relative: String in DATASETS:
		var full: String = ROOT + "/" + relative
		if not FileAccess.file_exists(full):
			print("[audit] SKIP (missing): ", full)
			continue
		if _only != "" and not relative.contains(_only):
			continue
		out.append(relative)
	return out

func _start_next() -> void:
	var list := _dataset_list()
	if _index >= list.size():
		_report()
		get_tree().quit()
		return

	var relative: String = list[_index]
	if _tileset != null:
		_tileset.queue_free()
		_tileset = null

	var url := ROOT + "/" + relative
	print("[audit] === loading ", relative, " ===")

	_tileset = Tileset3D.new()
	_tileset.url = url
	_tileset.debug_show_bounding_volume = false
	add_child(_tileset)
	_tileset.load()

	_frames = 0
	_framed = false
	_radius = 0.0

## Place the camera so the whole dataset is inside the frustum, mirroring
## Tileset3D::frame_camera. Distances are derived from the dataset's own radius rather than
## from a fixed camera pose: without this a fixed camera renders small local datasets as a
## single pixel and flies through large ones, and both look like failures from the counters.
##
## Called every frame until the radius is known (it is only available after the tileset
## parses), then never again, so the scheduler can converge at the final pose.
func _try_frame_camera() -> void:
	if _framed:
		return
	var radius := _tileset.get_dataset_radius()
	if radius <= 0.0:
		return
	_framed = true
	_radius = radius

	var fov_rad := deg_to_rad(_camera.fov)
	var distance := (radius / tan(fov_rad * 0.5)) * 1.2
	# The tileset's own placement (its model matrix) puts the dataset at this node's
	# origin for local tilesets, and at its georeferenced position for anchored ones, so
	# global_position is the right look-at target in both cases.
	var target := _tileset.global_position
	_camera.global_position = target + Vector3(0.0, distance * 0.4, distance)
	_camera.look_at(target, Vector3.UP)
	# Sizing the clip planes to the dataset keeps a 2 km photogrammetry dataset and a
	# 300 km one both visible; a fixed near/far pair cannot do both.
	_camera.near = maxf(0.01, radius * 0.001)
	_camera.far = maxf(1000.0, radius * 100.0)

func _process(_delta: float) -> void:
	if _tileset == null:
		return
	_frames += 1

	# The radius only exists once the tileset has parsed, so framing has to be attempted
	# every frame until it succeeds. It is idempotent and latches on first success.
	_try_frame_camera()

	if _frames < WARMUP_FRAMES:
		return

	var relative: String = _dataset_list()[_index]

	var rendered := _tileset.get_last_rendered_count()
	var loaded := _tileset.get_loaded_tile_count()
	var error := _tileset.get_last_error()

	# Same deadline as the http audit: a dataset that never settles must be reported, never
	# waited on. Without this the process parks in its window until a human kills it.
	var timed_out := _frames >= MAX_FRAMES and _tileset.get_in_flight_count() > 0

	# A dataset counts as healthy when it parsed and actually scheduled something. A
	# parsed-but-empty dataset (no content at the root, e.g. a proxy-only tree) is
	# reported as WARN rather than FAIL so it does not mask real load failures.
	var status := "OK"
	if error != "":
		status = "FAIL(load)"
	elif timed_out:
		status = "FAIL(timeout)"
	elif loaded == 0:
		status = "WARN(no-content)"

	var shot_note := ""

	# Grab the frame *before* writing the result line. Content nodes are parented a frame or
	# two after load() - GLTF parsing and mesh upload are deferred - so a status printed at
	# the same moment as the screenshot would record a category that the pixel count then
	# contradicts. Measured order is: screenshot first, classify from it, print once.
	if _save_shots and status == "OK":
		var image := get_viewport().get_texture().get_image()
		if image != null:
			var name := relative.replace("/", "__").replace(".json", "") + ".png"
			image.save_png(SHOT_DIR + "/" + name)
			# A dataset can load, schedule, and still draw nothing (the Icospheres NaN bug
			# looked exactly like that: loaded=1, rendered=1, all instances at a NaN
			# transform). Count non-background pixels so "OK but invisible" is caught here
			# rather than having to eyeball every screenshot.
			var filled := _count_filled_pixels(image)
			shot_note = " shot=" + name + " filled=" + str(filled)
			if filled <= 0:
				status = "WARN(invisible)"

	print("[audit] ", status, " ", relative,
		" tiles=", _tileset.get_tile_count(),
		" loaded=", loaded,
		" rendered=", rendered,
		" depth=", _tileset.get_maximum_depth(),
		" radius=", "%.3f" % _tileset.get_dataset_radius(),
		" error='", error, "'", shot_note)

	_results.append({
		"dataset": relative,
		"status": status,
		"tiles": _tileset.get_tile_count(),
		"loaded": loaded,
		"rendered": rendered,
		"depth": _tileset.get_maximum_depth(),
		"radius": _tileset.get_dataset_radius(),
		"error": error,
	})

	_index += 1
	_start_next()

## Number of pixels that differ from the top-left corner pixel (treated as background).
## Sampling a stride keeps this cheap on a 1920x1000 grab while still catching an
## entirely empty frame.
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
			# 8-bit tolerance: JPEG-free PNG, but the sky/grid may dither slightly.
			if absf(c.r - background.r) > 0.02 or absf(c.g - background.g) > 0.02 \
					or absf(c.b - background.b) > 0.02:
				filled += 1
	return filled

func _report() -> void:
	var ok := 0
	var warn := 0
	var fail := 0
	for r in _results:
		match r["status"]:
			"OK": ok += 1
			"WARN(no-content)": warn += 1
			"WARN(invisible)": warn += 1
			_: fail += 1

	print("")
	print("[audit] ================= SUMMARY =================")
	print("[audit] total=", _results.size(), " ok=", ok, " warn=", warn, " fail=", fail)
	for r in _results:
		if r["status"] != "OK":
			print("[audit]   ", r["status"], "  ", r["dataset"], "  error='", r["error"], "'")
	print("[audit] ===========================================")
