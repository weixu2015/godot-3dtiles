extends Node3D

# Interactive entry point for the virtual globe.
#
# Open demo/globe.tscn in the Godot editor: the Earth is already framed with its atmosphere,
# and pressing F5 lets you orbit it with the mouse.
#   left drag  - orbit around the Earth
#   right drag - tilt
#   wheel      - zoom, with inertia
#
# The scene renders correctly on its own; this script only adds convenience shortcuts so a
# recording can move between viewpoints without touching the mouse.

@export var start_longitude_degrees: float = 105.0
@export var start_latitude_degrees: float = 25.0
@export var start_distance: float = 16000000.0

var _camera: Camera3D = null

func _ready() -> void:
	_camera = get_node_or_null("GlobeCameraController")
	if _camera == null:
		return
	# A recognisable opening view (default: over China), framed so the whole globe and its
	# atmosphere fit comfortably.
	_camera.orbit_to(start_longitude_degrees, start_latitude_degrees, start_distance)
	_aim_sun()

# A DirectionalLight3D only cares about its orientation, so the scene file's transform says
# nothing about which side is lit. Point the sun at the hemisphere the camera is actually
# looking at, otherwise the opening view can land entirely on the night side. The offset
# keeps a terminator in frame instead of a flat, fully lit disc.
func _aim_sun() -> void:
	var sun: DirectionalLight3D = get_node_or_null("Sun")
	if sun == null:
		return
	var to_cam := _camera.global_position.normalized()
	var up := Vector3(0, 1, 0)
	if absf(to_cam.dot(up)) > 0.99:
		up = Vector3(0, 0, 1)
	var side := to_cam.cross(up).normalized()
	var sun_dir := (to_cam + side * 0.6 + up * 0.4).normalized()
	sun.global_position = sun_dir * 80000000.0
	# DirectionalLight3D travels along its -Z, so looking at the globe centre makes it shine
	# from wherever the light was placed.
	sun.look_at(Vector3.ZERO, up)

func _unhandled_input(event: InputEvent) -> void:
	if _camera == null:
		return
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return

	match key.keycode:
		KEY_1:
			_camera.orbit_to(0.0, 0.0, 16000000.0)      # equatorial overview
		KEY_2:
			_camera.orbit_to(0.0, 89.0, 16000000.0)     # polar
		KEY_3:
			_camera.orbit_to(105.0, 25.0, 11000000.0)   # oblique, close
		KEY_4:
			_camera.orbit_to(105.0, 25.0, 6800000.0)    # skimming the surface
		KEY_A:
			_toggle("Globe3D", "show_atmosphere")
		KEY_G:
			_toggle("Globe3D", "show_graticule")

func _toggle(node_path: String, property: String) -> void:
	var node := get_node_or_null(node_path)
	if node != null:
		node.set(property, not node.get(property))
