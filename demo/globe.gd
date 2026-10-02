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

# Seconds a Home flight takes.
@export var home_flight_seconds: float = 1.4

const EARTH_SEMI_MAJOR_AXIS := 6378137.0

var _camera: Camera3D = null
var _tileset: Node = null
var _status: Label = null
var _was_flying := false

func _ready() -> void:
	_camera = get_node_or_null("GlobeCameraController")
	if _camera == null:
		return
	_tileset = get_node_or_null("Georeference3D/Tileset3D")
	_status = get_node_or_null("HUD/Panel/Box/Status")

	var home_button: Button = get_node_or_null("HUD/Panel/Box/HomeButton")
	if home_button != null:
		home_button.pressed.connect(home)

	# A recognisable opening view (default: over China), framed so the whole globe and its
	# atmosphere fit comfortably.
	_camera.orbit_to(start_longitude_degrees, start_latitude_degrees, start_distance)
	_aim_sun()
	if _tileset != null:
		_tileset.tileset_loaded.connect(_update_status)
	_update_status()

# Flies to wherever the 3D Tiles dataset says it is, so the dataset can be checked without
# knowing its coordinates. This is deliberately driven by the tileset's own transform rather
# than by the scene's Georeference3D: when a dataset is georeferenced somewhere else (a
# converter writing a placeholder origin, say) the two disagree, and the dataset is invisible
# from the anchor no matter how far you zoom. Home still finds it.
func home() -> void:
	if _camera == null:
		return

	if _tileset == null:
		_status.text = "no Tileset3D in the scene"
		return

	var radius: float = _tileset.get_dataset_radius()
	if radius <= 0.0:
		_status.text = "Tileset3D has not loaded yet"
		# Still give a useful answer: fall back to the globe overview.
		_camera.orbit_to(start_longitude_degrees, start_latitude_degrees, start_distance)
		return

	var lat: float = _tileset.get_dataset_latitude()
	var lon: float = _tileset.get_dataset_longitude()
	var height: float = _tileset.get_dataset_height()

	# orbit_to/fly_to take the distance from the ellipsoid *centre*, and four dataset radii is
	# a comfortable framing; the floor keeps a tiny dataset from putting the camera inside it.
	#
	# The distance is measured to the dataset itself rather than approximated as
	# "semi-major axis + height": the ellipsoid is 8 km closer to its centre at latitude 40
	# than at the equator, and using the equatorial radius parks the camera 8 km above the
	# dataset - it flies to the right place and the model is a speck.
	var distance := _distance_to_dataset(lon, lat, height) + maxf(radius * 4.0, 400.0)
	_camera.call("fly_to", lon, lat, distance, home_flight_seconds)
	_aim_sun()
	_update_status("flying to %.5f, %.5f (%.0f m radius)" % [lon, lat, radius])

# Distance from the centre of the ellipsoid to a geodetic position, measured through the
# globe, which is the only node that knows the frame the scene is using. Falls back to the
# equatorial approximation when there is no globe in the scene.
func _distance_to_dataset(lon: float, lat: float, height: float) -> float:
	var globe: Node3D = get_node_or_null("Georeference3D/Globe3D")
	if globe == null:
		return EARTH_SEMI_MAJOR_AXIS + height
	var centre: Vector3 = globe.ecef_to_local(Vector3.ZERO)
	var surface: Vector3 = globe.geodetic_to_local(lon, lat, height)
	return centre.distance_to(surface)

# Shows where the dataset actually is. A dataset that disagrees with the georeference anchor
# renders as nothing at all, which looks exactly like a rendering bug, so put the numbers on
# screen next to the button that flies there.
func _update_status(prefix: String = "") -> void:
	if _status == null:
		return
	if _tileset == null:
		_status.text = "no Tileset3D in the scene"
		return

	var radius: float = _tileset.get_dataset_radius()
	if radius <= 0.0:
		_status.text = "Tileset3D loading..."
		return

	var separation: float = _tileset.get_anchor_separation()
	var text := "dataset  lon %.5f  lat %.5f\nh %.0f m  radius %.0f m" % [
		_tileset.get_dataset_longitude(), _tileset.get_dataset_latitude(),
		_tileset.get_dataset_height(), radius]
	if separation >= 0.0:
		text += "\nanchor is %.1f km away" % [separation / 1000.0]
		if separation > maxf(radius * 3.0, 10000.0):
			text += "\nWARNING: not visible from the anchor"
	if prefix != "":
		text = prefix + "\n" + text
	_status.text = text

func _process(_delta: float) -> void:
	if _camera == null:
		return
	# The status line has to settle too, otherwise "flying to ..." stays on screen for good.
	var flying: bool = _camera.call("is_flying")
	if _was_flying and not flying:
		_update_status()
	_was_flying = flying

# A DirectionalLight3D only cares about its orientation, so the scene file's transform says
# nothing about which side is lit. Point the sun at the hemisphere the camera is actually
# looking at, otherwise the opening view can land entirely on the night side. The offset
# keeps a terminator in frame instead of a flat, fully lit disc.
func _aim_sun() -> void:
	var sun: DirectionalLight3D = get_node_or_null("Sun")
	if sun == null:
		return

	# Direction from the *ellipsoid centre* to the camera. Deliberately not
	# global_position.normalized(): that is only the same thing while the centre happens to
	# sit at the world origin, and it is off by up to 90 degrees once a flight moves the
	# camera - which puts the whole view on the night side and looks like nothing rendered.
	# resolve_ellipsoid_center() is in the camera's parent space, so it goes through the
	# parent to reach world space.
	var pivot: Vector3 = _camera.call("resolve_ellipsoid_center")
	var parent := _camera.get_parent() as Node3D
	var world_pivot: Vector3 = parent.to_global(pivot) if parent != null else pivot
	var to_cam := (_camera.global_position - world_pivot).normalized()

	var up := Vector3(0, 1, 0)
	if absf(to_cam.dot(up)) > 0.99:
		up = Vector3(0, 0, 1)
	var side := to_cam.cross(up).normalized()
	var sun_dir := (to_cam + side * 0.6 + up * 0.4).normalized()
	sun.global_position = world_pivot + sun_dir * 80000000.0
	# DirectionalLight3D travels along its -Z, so looking at the globe centre makes it shine
	# from wherever the light was placed.
	sun.look_at(world_pivot, up)

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
