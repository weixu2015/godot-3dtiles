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
	var bar := HBoxContainer.new()
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	bar.offset_top = 10.0
	bar.offset_bottom = 42.0
	bar.offset_right = -12.0
	bar.alignment = BoxContainer.ALIGNMENT_END
	bar.add_theme_constant_override("separation", 8)
	bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(bar)

	_picker = OptionButton.new()
	for entry in DATASETS:
		_picker.add_item(entry["label"])
	_picker.item_selected.connect(_on_picker_selected)
	bar.add_child(_picker)

	_status = Label.new()
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(_status)

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
	# Horizontal: about the model's own up, unbounded. `+=` is the globe's sense (its angle is
	# `+1 * rate * (-dx/width) * 2PI` about the same axis). Swap to `-=` for the other
	# convention - the one where the surface under the cursor sticks to the cursor. They are
	# opposite gestures and neither is wrong; measured, the near face moves 206 px for a 200 px
	# drag at `-=`, and exactly the other way at `+=`.
	_rotate_rigid(_up, relative.x * SPIN_PER_PIXEL)

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
	if _status == null:
		return
	_status.text = "  左键平移   右键左右绕中心轴 / 上下倾斜(0-90°)   滚轮缩放    距离 %.0f m   高度 %.0f m   仰角 %.0f°   半径 %.0f m" % [
		_offset.length(), _eye_height(), 90.0 - rad_to_deg(_tilt_of_offset()),
		float(_tileset.dataset_radius)]
