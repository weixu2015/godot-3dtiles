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
#
# The HUD picker swaps the loaded 3D Tiles dataset and flies to it. Swapping a dataset is not
# just a URL change: the georeference anchor has to follow the dataset (see
# _sync_anchor_to_dataset), otherwise the content sits on the far side of the planet from the
# anchor and lands in a float32 range whose quantisation is a metre - which is what a jitter
# looks like when it is measured rather than eyeballed.

@export var start_longitude_degrees: float = 105.0
@export var start_latitude_degrees: float = 25.0
@export var start_distance: float = 16000000.0

# Seconds a Home flight takes.
@export var home_flight_seconds: float = 1.4

const EARTH_SEMI_MAJOR_AXIS := 6378137.0

# Datasets the picker offers. Each entry is a label plus a tileset.json URL; nothing else has
# to be kept in sync, because the position and the required anchor are both read back out of
# the tileset itself once it loads.
const DATASETS := [
	{
		"label": "Photogrammetry 1.1 (disk)",
		"url": "E:/GISData/3D Tiles/1.1/Photogrammetry/tileset.json",
	},
	{
		"label": "weinan (localhost:9090)",
		"url": "http://localhost:9090/3D Tiles/weinan/tileset.json",
	},
]

var _camera: Camera3D = null
var _tileset: Node = null
var _status: Label = null
var _picker: OptionButton = null
var _was_flying := false
# Set between "the picker asked for a dataset" and "that dataset finished loading": the load
# handler has to know whether to fly, or whether it is just a first load reporting in.
var _pending_flight := false
# Seconds a switch is allowed to take before it is declared dead. Without a deadline a URL
# that never answers leaves the picker wedged - no flight, no error, and the *next* load
# would fire the stale one.
const SWITCH_TIMEOUT_SECONDS := 20.0
# How many frames home() keeps re-asking for a readable dataset before giving up on it.
const HOME_RETRY_FRAMES := 240

# Incremented on every pick; a load handler that finishes after the user picked something else
# compares its token and stays out of the way.
var _switch_token := 0
var _switch_deadline_ms := 0
var _home_retry := 0

func _ready() -> void:
	_camera = get_node_or_null("GlobeCameraController")
	if _camera == null:
		return
	_tileset = get_node_or_null("Georeference3D/Tileset3D")
	_status = get_node_or_null("HUD/Panel/Box/Status")

	_picker = get_node_or_null("HUD/Panel/Box/DatasetPicker")
	if _picker != null:
		for entry in DATASETS:
			_picker.add_item(entry["label"])
		_picker.item_selected.connect(_on_dataset_selected)
		if _tileset != null:
			_select_picker_for_url(_tileset.url)
	if _tileset != null:
		_tileset.tileset_loaded.connect(_on_tileset_loaded)
		_tileset.load_failed.connect(_on_load_failed)

	# A recognisable opening view (default: over China), framed so the whole globe and its
	# atmosphere fit comfortably.
	_camera.orbit_to(start_longitude_degrees, start_latitude_degrees, start_distance)
	_aim_sun()
	_update_status()

	# Runtime debug HUD, bottom-right, F3 to toggle. The editor's viewport_hud addon cannot
	# exist in a running game, and the numbers worth watching during a run (origin-shift
	# cost, in-flight tiles, draw calls) are exactly the ones nothing else shows.
	if not Engine.is_editor_hint():
		preload("res://globe_hud.gd").create(self, self)

# ---- dataset picker ----------------------------------------------------------------------

func _select_picker_for_url(url: String) -> void:
	for i in DATASETS.size():
		if DATASETS[i]["url"] == url:
			_picker.select(i)
			return

# The picker doubles as the fly-in button: picking the dataset that is already loaded flies to
# it, picking another one swaps the tileset and then flies once it has loaded.
func _on_dataset_selected(index: int) -> void:
	if _tileset == null or _status == null:
		return
	var entry: Dictionary = DATASETS[index]
	var wanted: String = entry["url"]
	if _tileset.url == wanted:
		home()
		return

	# Every switch takes a token. A load that finishes after the user picked something else
	# must not fly: without this, picking A, then B, and having A land last sends the camera
	# to A while the picker says B.
	_switch_token += 1
	_pending_flight = true
	_switch_deadline_ms = Time.get_ticks_msec() + int(SWITCH_TIMEOUT_SECONDS * 1000.0)
	_status.text = "loading %s ..." % entry["label"]
	# set_url() reloads internally whenever the URL actually changes, so an explicit
	# reload() here ran the whole load twice - two synchronous passes over the root
	# document plus every external tileset, which was most of the switch hitch (the
	# probe measured two ~0.65 s loads inside one 1.3 s stage). The branch above already
	# handles the same-URL pick, so reaching this line guarantees the internal reload fires.
	_tileset.set_url(wanted)

func _on_tileset_loaded() -> void:
	if not _pending_flight:
		_update_status()
		return
	_pending_flight = false
	_switch_deadline_ms = 0

	var token := _switch_token
	_status.text = "rebasing the origin onto the dataset ..."
	await _rebase_to_dataset()
	if token != _switch_token:
		return
	home()

func _on_load_failed(reason: String) -> void:
	# A failed switch used to leave the flight pending forever, so the *next* successful load
	# flew on its own. A URL that never arrives - see encodeUrlPath: a raw space in the path
	# makes the server answer 404 - then looked exactly like "the fly-in button stopped
	# working". Clear the state and say so.
	_pending_flight = false
	_switch_deadline_ms = 0
	if _status != null:
		_status.text = "load failed: %s" % reason

# The geodetic origin authority of the scene's Georeference3D, or null when the scene has none
# or it carries an ECEF anchor instead.
func _anchor_authority() -> Resource:
	var geo := get_node_or_null("Georeference3D")
	if geo == null:
		return null
	var authority: Resource = geo.get("origin_authority")
	if authority == null or not authority.has_method("set_longitude"):
		return null
	return authority

# Moves the ENU origin onto the dataset.
#
# This is the single lever that decides how much of the float32 budget the dataset gets. With
# the origin on the dataset, tile vertices live within a few dataset radii of zero - tens of
# kilometres at worst, where one float32 step is under a centimetre. With the origin left on
# the other side of the planet (which is what the authored anchor was), the same vertices land
# at 1.7e7 m and a single float32 step is a full metre, so the content shimmers while the
# camera is still and the imagery layer buckets across the sphere.
#
# The anchor is taken from the tileset rather than from the entry above, because the tileset is
# the authority on where it is: a converter that wrote a placeholder origin still reports its
# real position, and the anchor has to agree with that, not with what the URL was expected to
# contain.
#
# Split across frames on purpose. Re-pointing the origin invalidates the globe mesh AND the
# imagery mesh, and both rebuilds are main-thread work; running them in the same frame as the
# new dataset's first upload is what makes a switch hitch.
func _rebase_to_dataset() -> void:
	var authority := _anchor_authority()
	if authority == null or _tileset == null:
		return

	var lon: float = _tileset.get_dataset_longitude()
	var lat: float = _tileset.get_dataset_latitude()
	if is_equal_approx(float(authority.get("longitude")), lon) \
			and is_equal_approx(float(authority.get("latitude")), lat):
		return

	authority.set("longitude", lon)
	authority.set("latitude", lat)
	authority.set("height", 0.0)

	# The globe's meshes are authored in Y-up ECEF and placed by a node transform, so a moved
	# origin costs it one rebase() (three transform writes - the frame landing moved, the
	# meshes did not). The imagery still bakes the frame into its vertices at tile creation
	# time, so it is only picked up when the tree is re-created (reload_tiles()).
	var globe := get_node_or_null("Georeference3D/Globe3D")
	if globe != null:
		globe.call("rebase")
		await get_tree().process_frame

	var layer := get_node_or_null("Georeference3D/GlobeTileLayer")
	if layer != null:
		layer.call("reload_tiles")
		await get_tree().process_frame

# One float32 representable step at `magnitude`. Everything in a Godot transform is float32
# unless the engine is built with double precision, so this is the floor on how precisely a
# vertex or a camera position at that range can be placed - and a jitter is exactly this
# number showing up as a screen-space wobble.
func _float32_step(magnitude: float) -> float:
	if magnitude <= 0.0:
		return 0.0
	return pow(2.0, floor(log(magnitude) / log(2.0)) - 23.0)

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
		# The load signal can arrive while the tree is still half-built: reload() tears the
		# previous dataset down and the first frames after that run against a root whose
		# bounding volume is not readable yet. Falling back to the opening viewpoint here is
		# what made the picker "often" fail - one run in a handful flew to the overview
		# instead of the dataset (measured: |p| = 1.63e7, exactly the opening orbit). Retry for
		# a bounded number of frames instead, and only fall back once that is exhausted.
		if _home_retry > 0:
			_home_retry -= 1
			_status.text = "waiting for the dataset to become readable ..."
		else:
			_home_retry = HOME_RETRY_FRAMES
			_status.text = "Tileset3D has not loaded yet"
			_camera.orbit_to(start_longitude_degrees, start_latitude_degrees, start_distance)
		return
	_home_retry = 0

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

	# The precision readout. A model is as still as the float32 grid it is authored in is fine,
	# and the grid spacing follows the magnitude of the coordinates. Two magnitudes matter here
	# and only the first one is fixable by moving the anchor:
	#   content - the dataset's own extent, because a tile localises its vertices inside the
	#             tile and the tiles live inside the dataset;
	#   planet  - the globe surface and the camera are 6.4e6 m from the origin whatever the
	#             anchor is, so the planet layer keeps this step until the frame itself is
	#             rebased (origin shift), not until the anchor moves.
	var content_step: float = _float32_step(radius)
	var planet_step: float = _float32_step(EARTH_SEMI_MAJOR_AXIS)
	text += "\norigin to dataset " + ("n/a" if separation < 0.0 else str(separation / 1000.0) + " km")
	text += "\nfloat32 step: content " + str(content_step) + " m / planet " + str(planet_step) + " m"
	if prefix != "":
		text = prefix + "\n" + text

	# The sub-solar point is the one number that explains the terminator: if the lit side is
	# not where these coordinates say it should be, the frame conversion is wrong, not the
	# lighting.
	var globe := get_node_or_null("Georeference3D/Globe3D")
	if globe != null:
		var sub: Vector2 = globe.call("get_sub_solar_point")
		text += "\nsun  lon %.2f  lat %.2f" % [sub.x, sub.y]
	_status.text = text

func _process(_delta: float) -> void:
	if _camera == null:
		return

	# A switch with no answer must expire rather than sit there.
	if _pending_flight and _switch_deadline_ms > 0 and Time.get_ticks_msec() > _switch_deadline_ms:
		_pending_flight = false
		_switch_deadline_ms = 0
		if _status != null:
			_status.text = "switch timed out after %.0f s: nothing loaded" % SWITCH_TIMEOUT_SECONDS

	# The status line has to settle too, otherwise "flying to ..." stays on screen for good.
	var flying: bool = _camera.call("is_flying")
	if _was_flying and not flying:
		_update_status()
	_was_flying = flying

	# A flight that could not start because the dataset was not readable yet retries here.
	if _home_retry > 0 and not flying:
		home()

# Points the scene's DirectionalLight3D at the same place the globe's atmosphere is lit
# from.
#
# This used to derive the sun from the camera ("keep the opening view out of the dark"),
# which is why the screenshot showed a bright globe and a sun sprite in unrelated places:
# the atmosphere integrates its own terminator from the globe's sub-solar direction, and any
# light that disagrees with it lights the tiles from the wrong side.
#
# Now the globe owns the sun (Globe3D::get_sun_direction / get_sun_position) and this only
# relays it, so the light, the atmosphere's scattering and the sun billboard are guaranteed
# to agree. The DirectionalLight3D is a *renderer* of that direction for the tile content;
# if the scene has no Sun node the globe still lights itself correctly.
func _aim_sun() -> void:
	var sun: DirectionalLight3D = get_node_or_null("Sun")
	if sun == null:
		return
	var globe: Node3D = get_node_or_null("Georeference3D/Globe3D")
	if globe == null:
		return

	var centre: Vector3 = globe.call("ecef_to_local", Vector3.ZERO)
	var world_centre: Vector3 = globe.to_global(centre)
	var sun_position: Vector3 = globe.to_global(globe.call("get_sun_position") as Vector3)

	# Position matters only for the light's own debug gizmo - a DirectionalLight3D shades by
	# orientation alone. It is set NEXT TO THE CAMERA rather than at the sun's real distance
	# on purpose: the sun sits 1e8 m away (far * 0.5), and a light parked that far from the
	# origin while the camera's far plane is 2e8 is what makes the light culler's frustum
	# degenerate - it is the "create_frustum_points" failure the engine spams when the camera
	# is close to the surface. The orientation is unchanged either way.
	var sun_direction: Vector3 = (sun_position - world_centre).normalized()
	var up := Vector3(0, 1, 0)
	if absf(sun_direction.dot(up)) > 0.99:
		up = Vector3(0, 0, 1)
	sun.global_position = _camera.global_position + sun_direction * 1000.0
	sun.look_at(world_centre, up)

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
			_toggle("Georeference3D/Globe3D", "show_atmosphere")
		KEY_G:
			_toggle("Georeference3D/Globe3D", "show_graticule")
		KEY_S:
			_toggle("Georeference3D/Globe3D", "show_sun")
		KEY_P:
			# Toggles the pure Rayleigh integral, with no Mie forward lobe and no sunset tint.
			# The atmosphere has several contributions and "it looks wrong" is impossible to
			# attribute without being able to peel one off.
			_toggle("Georeference3D/Globe3D", "atmosphere_debug_pure")
		KEY_T:
			# Steps the sunset tint, so the terminator can be dialled between physically
			# neutral (0.0) and the demo default (0.65).
			_cycle_sunset_tint()

func _toggle(node_path: String, property: String) -> void:
	var node := get_node_or_null(node_path)
	if node != null:
		node.set(property, not node.get(property))
		_update_status()

# Cycles the sunset tint through a few useful values, ending back at the demo default, and
# prints the sub-solar point so the terminator can be compared against a known instant.
func _cycle_sunset_tint() -> void:
	var globe := get_node_or_null("Georeference3D/Globe3D")
	if globe == null:
		return
	var steps := [0.0, 0.35, 0.65, 1.0]
	var current: float = globe.get("atmosphere_sunset_tint")
	var next: float = steps[0]
	for i in steps.size():
		if is_equal_approx(current, steps[i]):
			next = steps[(i + 1) % steps.size()]
			break
	globe.set("atmosphere_sunset_tint", next)
	_update_status("atmosphere_sunset_tint = %.2f" % next)
