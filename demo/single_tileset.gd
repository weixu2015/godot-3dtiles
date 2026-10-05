extends Node3D
# Runtime view for the single-dataset scene: pick a dataset, frame it, then drive the camera
# with the mouse.
#
# The C++ side already puts the camera somewhere sensible when a dataset loads - it fits the
# root bounding box, from above, with the model's own up. This script does the three things
# only a script can:
#
#   * PER-DATASET framing. A declared bounding box is not the same thing as what you
#     downloaded. Aerometrex SanFrancisco 2cm declares a 4158 m root sphere covering the whole
#     city set, while only the downtown skyline is present on disk, so a box fit frames a lot
#     of empty space. `distance_scale` per entry pulls the eye in to what is actually there.
#   * TIMING. Automatic framing runs out of Tileset3D's process loop and re-applies its pose
#     for the first several frames, so reading the pose when `tileset_loaded` fires reads the
#     one the .tscn left behind - 2790 km away. The C++ says when it is done
#     (`framing_released`) and the takeover happens exactly then.
#   * INTERACTION, ported term for term from GlobeCameraController so the gestures feel the
#     same here as on the globe. The camera is a plain Camera3D on purpose: a
#     GlobeCameraController would orbit the fallback Y-up ECEF frame, which is the wrong frame
#     for a dataset the implicit georeference left in its own coordinates.
#
# Every number a dataset needs is in DATASETS below; nothing here is specific to one of them.

const DATASETS := [
	{
		"label": "Aerometrex SanFrancisco 2cm",
		"url": "http://localhost:9090/3D Tiles/Aerometrex-SanFrancisco-2cm/tileset.json",
		"sse": 4.0,
		# The root sphere covers cities that are not on disk. 0.5 frames the part that is.
		"distance_scale": 0.5,
		# Where to orbit around, as an offset from the dataset centre in its own local
		# metres. Non-zero when the interesting part is not where the box is centred.
		"pivot_offset": Vector3.ZERO,
	},
	{
		"label": "taiwan",
		"url": "E:/GISData/3D Tiles/taiwan/tileset.json",
		"sse": 16.0,
		"distance_scale": 1.0,
		"pivot_offset": Vector3.ZERO,
	},
	{
		"label": "Photogrammetry 1.1 (disk)",
		"url": "E:/GISData/3D Tiles/1.1/Photogrammetry/tileset.json",
		"sse": 16.0,
		"distance_scale": 1.0,
		"pivot_offset": Vector3.ZERO,
	},
	{
		"label": "weinan (local frame)",
		"url": "E:/GISData/3D Tiles/weinan/tileset.json",
		"sse": 16.0,
		"distance_scale": 1.0,
		"pivot_offset": Vector3.ZERO,
	},
]

const PAN_SPEED := 0.0016               # fraction of the view distance per pixel of left-drag
const DOLLY_PER_WHEEL := 1.12           # distance multiplier per wheel notch

# Right-drag, in radians per pixel of drag. The globe's reference derives its rate from the
# distance to the pivot (clamp(rho - 1, 1/5000, 1.77) per unit of screen fraction), which on
# a 6371 km planet is a good constant and on a 700 m dataset is roughly a hundred times too
# fast - a short flick sends the view spinning. Per-pixel is the one that behaves the same at
# both scales, and it is what a single-dataset view wants.
const SPIN_PER_PIXEL := 0.0044          # ~0.25 deg/px, so a full window width turns about 80 deg
const TILT_PER_PIXEL := 0.0035          # ~0.20 deg/px
# The tilt's clamp: the eye's elevation above the dataset's own plane. 90 degrees is a
# straight-down view onto the model, 0 is a look along the ground; nothing below the plane is
# reachable, which is the limit a viewer expects from data that sits on a surface.
const TILT_MIN := 0.02
const TILT_MAX := 1.5707963             # PI / 2

const DISTANCE_MIN_FACTOR := 0.02       # closest approach, as a fraction of the framed distance
const DISTANCE_MAX_FACTOR := 12.0
# How often the on-screen parameter panel is rebuilt. See _update_hud().
const HUD_REFRESH_SECONDS := 0.25
# How far the pivot may be panned from the dataset centre, in dataset radii. Panning without a
# limit walks the view off the data and the user has no way to tell how far they have gone.
const PAN_LIMIT_RADII := 2.0

var _tileset: Node = null
var _camera: Camera3D = null
var _picker: OptionButton = null
var _status: Label = null

var _entry: Dictionary = {}
# The rig: where the camera is relative to the model's centre, and which way it faces. Two
# separate things on purpose.
#
# A pan moves the eye WITHOUT turning the camera, so the model slides across the viewport and
# the camera's aim is no longer the pivot. A right-drag then rotates the eye AND the facing
# rigidly about the pivot. A rigid rotation of the whole rig about C leaves the pivot's
# position in camera space untouched - it is `B^-1 * (C - eye)` before and
# `(rB)^-1 * (C - (C + r*offset)) = B^-1 * (-offset)` after, the same vector - so the model
# turns exactly where it already is on screen. That is the whole reason the facing is stored
# separately: aim the camera at the pivot instead and every rotation re-centres the subject,
# which is the jump.
var _pivot := Vector3.ZERO              # the model's own centre; the rotation centre
var _offset := Vector3.ZERO             # pivot -> eye; its length is the view distance
var _basis := Basis.IDENTITY            # the camera's facing, independent of _offset
var _framed := 0.0                      # the distance automatic framing chose, as the 100%
var _up := Vector3.UP                   # the model's own up, world space
var _reference := Vector3.RIGHT         # a horizontal unit vector, derived per dataset
var _orbit_dragging := false            # right button: turntable + tilt
var _pan_dragging := false              # left button: slide the view
# OptionButton.select() emits item_selected, so setting the initial entry programmatically
# would otherwise re-enter the handler and reload the dataset it just selected.
var _syncing_picker := false

func _ready() -> void:
	_tileset = get_node_or_null("Tileset3D")
	_camera = get_node_or_null("Camera3D")
	if _tileset == null or _camera == null:
		return
	_tileset.tileset_loaded.connect(_on_tileset_loaded)
	_tileset.framing_released.connect(_on_framing_released)
	_build_environment()
	_build_ui()
	_motion_enabled = OS.get_environment("ST_MOTION") != ""
	# ST_HIDE=1 hides the tileset's whole subtree: the traversal still runs, nothing is drawn. The
	# difference against a normal run is the renderer's share, which is what says whether a frame
	# is being spent on tiles at all.
	# ST_LOADS=<n> overrides the concurrent in-flight limit. The default is tuned for throughput;
	# the question a frame-rate run asks is different, because every in-flight request is a worker
	# competing with the main thread for the same cores.
	var loads := OS.get_environment("ST_LOADS")
	if loads != "":
		_tileset.maximum_simultaneous_loads = int(loads)
	# ST_UPLOADS=<n> overrides how many decoded tiles may be turned into nodes per frame. That
	# assembly runs on the main thread and costs about 2 ms per tile on photogrammetry, so this
	# is what decides how much of a frame a streaming burst is allowed to take.
	var uploads := OS.get_environment("ST_UPLOADS")
	if uploads != "":
		_tileset.maximum_uploads_per_frame = int(uploads)
	if OS.get_environment("ST_HIDE") != "":
		_tileset.show = false
	_stats_enabled = OS.get_environment("ST_STATS") != ""
	_shot_path = OS.get_environment("ST_SHOT")
	if OS.get_environment("ST_SHOT_AT") != "":
		_shot_at = float(OS.get_environment("ST_SHOT_AT"))
	_select(_entry_for_url(_tileset.url))
	if _tileset.tile_count > 0:
		_on_tileset_loaded()

# A bare scene has no light in it, and a photogrammetry tileset is a LIT material, so without
# this the dataset renders as a black silhouette against the clear colour - which reads as
# "the tiles did not load" and is nothing of the sort. Built here rather than as an Environment
# resource in the .tscn because the enum has to be expressed as its constant: the same setup
# written with the numeric literals in the scene file does not light anything.
func _build_environment() -> void:
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.18, 0.20, 0.23)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(1.0, 1.0, 1.0)
	environment.ambient_light_energy = 1.0
	world_env.environment = environment
	add_child(world_env)

func _build_ui() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)

	# Dataset picker, top-left, and NOT a row in a bar shared with the HUD.
	#
	# A Control clamps its own size to its children's minimum, so putting a five-line label and
	# an OptionButton in one HBoxContainer stretches the button to the height of the text block:
	# a 30 px control in a 100 px row, with the empty middle showing. Two independent controls
	# anchored to the two corners cannot do that to each other.
	_picker = OptionButton.new()
	_picker.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_picker.offset_left = 12.0
	_picker.offset_top = 10.0
	_picker.custom_minimum_size = Vector2(260.0, 32.0)
	layer.add_child(_picker)
	for entry in DATASETS:
		_picker.add_item(entry["label"])
	_picker.item_selected.connect(_on_picker_selected)

	# Parameters, top-right. A PanelContainer rather than a bare Label: the numbers sit over
	# airborne imagery and need a background of their own to stay readable.
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	# Explicit: with the right edge anchored, the panel has to grow leftwards to fit its text.
	# set_anchors_preset() sets the anchors only, so the grow direction is stated here.
	panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	panel.offset_right = -12.0
	panel.offset_top = 10.0
	# Clicks in the empty corner around the text belong to the viewport, not to the panel.
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = Color(0.06, 0.07, 0.09, 0.72)
	panel_style.set_corner_radius_all(6)
	panel_style.set_content_margin_all(8)
	panel.add_theme_stylebox_override("panel", panel_style)
	layer.add_child(panel)

	_status = Label.new()
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_status.add_theme_font_size_override("font_size", 13)
	panel.add_child(_status)

func _entry_for_url(url: String) -> Dictionary:
	for entry in DATASETS:
		if entry["url"] == url:
			return entry
	# An unknown url still has to work; it just gets the neutral framing.
	return {"label": url.get_file(), "url": url, "sse": 16.0,
			"distance_scale": 1.0, "pivot_offset": Vector3.ZERO}

func _select(entry: Dictionary) -> void:
	_entry = entry
	if _picker != null:
		for i in DATASETS.size():
			if DATASETS[i]["url"] == entry["url"]:
				_syncing_picker = true
				_picker.select(i)
				_syncing_picker = false
				break

func _on_picker_selected(index: int) -> void:
	if _syncing_picker or index < 0 or index >= DATASETS.size():
		return
	_entry = DATASETS[index]
	_tileset.maximum_screen_space_error = float(_entry["sse"])
	_tileset.url = _entry["url"]

func _on_tileset_loaded() -> void:
	_tileset.maximum_screen_space_error = float(_entry.get("sse", 16.0))

# Adopt whatever pose automatic framing left, so the first drag continues from it.
func _read_frame() -> void:
	var centre: Vector3 = _tileset.dataset_center_local
	# The model's up. Automatic framing works it out the same way: a dataset the implicit frame
	# left in ECEF stands on its geodetic up, and anything authored in a local frame has no such
	# thing to find and gets +Y.
	var hint: Vector3 = _tileset.dataset_up_axis
	if hint.length_squared() > 1e-12:
		_up = (_tileset.global_transform.basis * hint).normalized()
	elif centre.length() > 1.0e6:
		_up = _tileset.global_transform.basis * centre.normalized()
	else:
		_up = _tileset.global_transform.basis.y.normalized()

	_reference = _up.cross(Vector3.FORWARD)
	if _reference.length_squared() < 1e-6:
		_reference = _up.cross(Vector3.RIGHT)
	_reference = _reference.normalized()

	_pivot = _home()
	_offset = _camera.global_position - _pivot
	if _offset.length_squared() < 1e-6:
		return
	# The C++ framing aims the camera at the dataset centre, so its facing is exactly what the
	# first rotation should build on. Taken as-is rather than re-derived from `_offset`: doing
	# the latter is what makes the two agree, and there is nothing to gain from forcing it.
	_basis = _camera.global_transform.basis.orthonormalized()
	# Per-dataset framing. The declared bounding box is not what is on disk (see DATASETS), so
	# the scale is applied to the distance the C++ fit chose, along the same direction.
	_framed = maxf(_offset.length(), 1.0) * float(_entry.get("distance_scale", 1.0))
	_offset = _offset.normalized() * _framed
	_apply_camera()

func _apply_camera() -> void:
	_camera.global_transform = Transform3D(_basis, _pivot + _offset)

# The eye's angle from the model's up: 0 is straight overhead, PI/2 is level with the plane.
func _tilt_of_offset() -> float:
	var length := _offset.length()
	if length < 1.0e-6:
		return TILT_MIN
	return acos(clampf(_offset.dot(_up) / length, -1.0, 1.0))

# A rigid rotation of the whole rig about the pivot. The pivot's screen position is invariant
# under this (see the note on the rig above), so a rotation never moves the model.
func _rotate_rigid(axis: Vector3, angle: float) -> void:
	if axis.length_squared() < 1.0e-12 or absf(angle) < 1.0e-9:
		return
	var rotation := Basis(axis.normalized(), angle)
	_offset = rotation * _offset
	_basis = (rotation * _basis).orthonormalized()
	_apply_camera()

func _on_framing_released() -> void:
	_read_frame()


func _eye_height() -> float:
	return (_camera.global_position - _home()).dot(_up)

func _home() -> Vector3:
	return _tileset.to_global(_tileset.dataset_center_local +
			(_entry.get("pivot_offset", Vector3.ZERO) as Vector3))

func _unhandled_input(event: InputEvent) -> void:
	if _camera == null:
		return
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		match button.button_index:
			MOUSE_BUTTON_LEFT:
				_pan_dragging = button.pressed
			MOUSE_BUTTON_RIGHT:
				_orbit_dragging = button.pressed
			MOUSE_BUTTON_WHEEL_UP:
				if button.pressed:
					_dolly(1.0 / DOLLY_PER_WHEEL)
			MOUSE_BUTTON_WHEEL_DOWN:
				if button.pressed:
					_dolly(DOLLY_PER_WHEEL)
	elif event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		if _orbit_dragging:
			_apply_tilt_drag(motion.relative)
		elif _pan_dragging:
			_pan(motion.relative)

# Left-drag: slide the view. The eye moves along the CAMERA's right and up, so the picture
# always moves the way the cursor is going, and the facing is left alone - which is what lets
# a later rotation happen wherever the model now sits. The rate is a fraction of the view
# distance, so a pan feels the same at 30 m and at 30 km.
func _pan(relative: Vector2) -> void:
	var step := _offset.length() * PAN_SPEED
	_offset -= _basis.x * (relative.x * step)
	_offset += _basis.y * (relative.y * step)
	_clamp_rig()

# Keep the rig usable: the model has to stay within reach, and the eye has to stay on the
# model's side of its own plane. Both are reassembled from the direction they are already
# pointing, so a pan that runs into a limit slides along it rather than snapping.
func _clamp_rig() -> void:
	var length := _offset.length()
	if length < 1.0e-6:
		_offset = _basis.z * _framed
		_apply_camera()
		return
	var direction := _offset / length
	var tilt := acos(clampf(direction.dot(_up), -1.0, 1.0))
	if tilt < TILT_MIN or tilt > TILT_MAX:
		var wanted := clampf(tilt, TILT_MIN, TILT_MAX)
		var horizontal := direction - _up * direction.dot(_up)
		if horizontal.length_squared() < 1.0e-9:
			horizontal = _reference
		direction = _up * cos(wanted) + horizontal.normalized() * sin(wanted)
	var limit := maxf(_framed * DISTANCE_MAX_FACTOR, 1.0)
	_offset = direction * clampf(length, 1.0, limit)
	_apply_camera()

# Right-drag, about the MODEL'S OWN CENTRE. The globe's reference pivots on the ground point
# under the screen centre, which is right there because the ground is the whole planet and the
# camera is 800 m above one point of it; here the subject is a 700 m city block that a
# left-drag has already pushed off to one side, and turning about that point swings it out of
# frame. Turning about the model's centre keeps it exactly where the viewport put it.
func _apply_tilt_drag(relative: Vector2) -> void:
	# Horizontal: about the model's own up, unbounded.
	#
	# The sign makes a rightward drag turn the model right, which is what the eye expects of a
	# city block under the cursor. This has been got wrong twice: the globe's own tilt works out
	# to the opposite sign (it is a planet seen from outside, turning away from the drag), and an
	# earlier attempt at proving the direction numerically measured a near-face probe instead of
	# asking the person looking at the screen. If it ever reads backwards again, this is the
	# single sign to flip.
	_rotate_rigid(_up, -relative.x * SPIN_PER_PIXEL)

	# Vertical: tilt, clamped to the 0-90 degree range. The axis is `up x offset`, which lies in
	# the plane the eye moves in, so the eye stays in that plane and the rotation angle is the
	# change in tilt exactly - a clamp on the angle is a clamp on the tilt, with no measuring and
	# undoing. Which sign it is took a measurement: `(up x o) x o` expands to `o(up.o) - up(o.o)`,
	# so a rotation of +phi about that axis gives `o'.up = |o| cos(tilt + phi)` - a POSITIVE angle
	# RAISES the tilt. Getting that backwards runs the clamp the wrong way and the eye sails past
	# 90 degrees, which is what the self-test caught at 162 degrees.
	#
	# The drag is negative so that dragging up raises the eye towards level, which moves the
	# ground in front of the camera down the screen - the same feel as the globe's tilt.
	var axis := _up.cross(_offset)
	var tilt := _tilt_of_offset()
	var angle := clampf(-relative.y * TILT_PER_PIXEL, TILT_MIN - tilt, TILT_MAX - tilt)
	_rotate_rigid(axis, angle)

func _dolly(factor: float) -> void:
	var length := clampf(_offset.length() * factor, maxf(_framed * DISTANCE_MIN_FACTOR, 1.0),
			_framed * DISTANCE_MAX_FACTOR)
	_offset = _offset.normalized() * length
	_apply_camera()

var _motion_enabled := false
var _stats_enabled := false
var _motion_time := 0.0
var _stats_elapsed := 0.0
var _frame_ms_smoothed := 0.0
var _frame_history: Array[float] = []
var _hud_elapsed := 0.0

# ST_SHOT=<absolute png path> ST_SHOT_AT=<seconds>: save the viewport and quit. The runtime
# window cannot be screenshotted from outside in every environment (no window shows up in the
# desktop enumeration here at all), but reading the viewport texture from inside the engine
# always works - so this is how the runtime view gets checked without a human looking at it.
var _shot_path := ""
var _shot_at := 6.0
var _shot_elapsed := 0.0
var _shot_taken := false

func _process(delta: float) -> void:
	if not _shot_path.is_empty() and not _shot_taken:
		_shot_elapsed += delta
		if _shot_elapsed >= _shot_at:
			_shot_taken = true
			await RenderingServer.frame_post_draw
			var image := get_viewport().get_texture().get_image()
			image.save_png(_shot_path)
			print("[single_tileset] saved %s (%dx%d)" % [_shot_path, image.get_width(),
					image.get_height()])
			get_tree().quit()
			return
	if _stats_enabled:
		_frame_history.push_back(delta)
		if _frame_history.size() > 600:
			_frame_history.remove_at(0)
	if _motion_enabled and _camera != null:
		_drive_motion(delta)
	if _status == null:
		return
	_stats_elapsed += delta
	_update_hud(delta)

# The frame time, smoothed: the instantaneous number swings by half on any one frame, and an
# optimisation is judged on where it settles.
func _frame_ms() -> float:
	var ms := 1000.0 / maxf(Engine.get_frames_per_second(), 1.0)
	_frame_ms_smoothed = ms if _frame_ms_smoothed <= 0.0 else lerpf(_frame_ms_smoothed, ms, 0.15)
	return _frame_ms_smoothed

func _tile_lines() -> String:
	if _tileset == null:
		return "no tileset"
	return "tiles  %d declared / %d loaded / %d rendered / %d in-flight  %s\nradius %.0f m   sse %.1f" % [
		int(_tileset.tile_count), int(_tileset.loaded_tile_count),
		int(_tileset.last_rendered_count), int(_tileset.in_flight_count),
		_human_bytes(int(_tileset.loaded_bytes)), float(_tileset.dataset_radius),
		float(_tileset.maximum_screen_space_error)]

func _human_bytes( value: int ) -> String:
	if value < 1024:
		return "%d B" % value
	if value < 1024 * 1024:
		return "%.0f KiB" % (value / 1024.0)
	return "%.1f MiB" % (value / (1024.0 * 1024.0))

func _update_hud( delta: float ) -> void:
	# Refreshed at a few hertz, NOT every frame. RenderingServer.get_rendering_info() is a query
	# into the renderer, not a counter read, and building the panel text allocates a string every
	# time - both of which belong on the same side of the frame as anything else worth avoiding.
	# globe_hud.gd throttles for the same reason; this one did not, and the process phase was
	# measured at 46 ms a frame on a scene that draws 71 calls.
	_hud_elapsed += delta
	if _hud_elapsed < HUD_REFRESH_SECONDS:
		return
	_hud_elapsed = 0.0
	var draws := RenderingServer.get_rendering_info(
			RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)
	var prims := RenderingServer.get_rendering_info(
			RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)
	var eye := _camera.global_position if _camera != null else Vector3.ZERO
	_status.text = (
			"fps %d   frame %.2f ms   draws %d   prims %s\n" +
			"cam local %s   dist %.0f m   above plane %.0f m   tilt %.0f deg\n" +
			"%s\n" +
			"near %.2f  far %.0f\n" +
			"L 平移   R 绕中心轴旋转 / tilt(0-90)   wheel 缩放") % [
		Engine.get_frames_per_second(), _frame_ms(), draws, _human_bytes(prims),
		_fmt_vec(eye), _offset.length(), _eye_height(), 90.0 - rad_to_deg(_tilt_of_offset()),
		_tile_lines(), float(_camera.near), float(_camera.far)]
	if _stats_enabled:
		_emit_stats(draws, prims)

func _fmt_vec( v: Vector3 ) -> String:
	return "(%.0f, %.0f, %.0f)" % [v.x, v.y, v.z]

# ST_STATS=1: the same numbers, once a second, on stdout. The on-screen HUD cannot be read from a
# script, and an optimisation has to be measured, not looked at.
#
# Percentiles, not an average. The first version of this printed the smoothed frame time, which
# read 113-120 fps on a run that was visibly stuttering down to 48: a 1 Hz sample of a
# smoothed series hides the tail entirely, and the tail is the whole complaint. p50 says what the
# frame time usually is, p99 and max say how bad it gets, and "over 1.5x" counts the frames a
# person would notice.
func _emit_stats( draws: int, prims: int ) -> void:
	if _stats_elapsed < 1.0:
		return
	_stats_elapsed = 0.0
	var sorted := _frame_history.duplicate()
	sorted.sort()
	var count := sorted.size()
	if count == 0:
		return
	var p := func( frac: float ) -> float:
		return sorted[clampi(int(frac * (count - 1)), 0, count - 1)] * 1000.0
	var median: float = p.call(0.5)
	var over := 0
	for value in sorted:
		if value > median * 1.5:
			over += 1
	print("[st] p50 %.2f  p90 %.2f  p99 %.2f  max %.2f ms  (%d/%d over %.1f)  script %.2f ms | loaded %d rendered %d inflight %d %.1f MiB | draws %d prims %d" % [
		median, p.call(0.9), p.call(0.99), sorted[count - 1] * 1000.0, over, count,
		median * 1.5,
		Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
		int(_tileset.loaded_tile_count), int(_tileset.last_rendered_count),
		int(_tileset.in_flight_count), float(_tileset.loaded_bytes) / (1024.0 * 1024.0),
		draws, prims])
	_frame_history.clear()

# ST_MOTION=1: drive the same two gestures the mouse would, on a sine, so frame rate UNDER MOTION
# can be captured from a script. Hand-driven input is what made every earlier performance claim
# here unrepeatable, and the gestures are the workload: a parked camera and a camera that keeps
# turning and zooming are not the same test.
func _drive_motion( delta: float ) -> void:
	_motion_time += delta
	_apply_tilt_drag(Vector2(sin(_motion_time * TAU / 12.0) * 5.0,
			sin(_motion_time * TAU / 17.0) * 1.5))
	_dolly(1.0 + 0.006 * cos(_motion_time * TAU / 9.0))
