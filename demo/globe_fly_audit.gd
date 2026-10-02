extends Node3D

# Regression harness for the globe <-> 3D Tiles relative placement and the HUD Home flight.
#
# Instances the real demo/globe.tscn, waits for the tileset to load, snaps the camera to the
# dataset the same way the Home button does, and then checks the numbers instead of a picture:
#
#   1. the camera really hovers over the dataset's own longitude/latitude
#   2. the tileset's loaded content sits at the same longitude/latitude on the globe
#   3. that content is actually in front of the camera (centre of the viewport)
#
# The failure this guards against is invisible to every distance-shaped test: the camera's
# frame and the frame its own pivot came from differed by a rotation, which preserves length,
# so distances, screen space error and altitude were all correct while the flight landed on
# the far side of the planet.
#
# Run:  godot --headless --path demo globe_fly_audit.tscn

const EARTH_SEMI_MAJOR_AXIS := 6378137.0

const OUT_PATH := "res://globe_fly_audit.png"

# Degrees of longitude/latitude the camera is allowed to miss the dataset by. One degree is
# ~111 km, so 0.01 is ~1 km - three orders of magnitude tighter than the ~8000 km error this
# harness was written to catch, and loose enough to survive float32 at 6.4e6 m.
const ANGLE_TOLERANCE_DEGREES := 0.01

var _failures := 0

func _check(condition: bool, message: String) -> void:
	if condition:
		print("  PASS  ", message)
	else:
		_failures += 1
		print("  FAIL  ", message)

func _ready() -> void:
	var packed: PackedScene = load("res://globe.tscn")
	if packed == null:
		push_error("globe_fly_audit: cannot load globe.tscn")
		get_tree().quit(1)
		return

	var globe_scene: Node = packed.instantiate()
	add_child(globe_scene)

	var camera: Node = globe_scene.get_node_or_null("GlobeCameraController")
	var globe: Node = globe_scene.get_node_or_null("Georeference3D/Globe3D")
	var tileset: Node = globe_scene.get_node_or_null("Georeference3D/Tileset3D")
	# Imagery needs a tile server this harness does not run, and it only obscures the
	# geometry being measured here.
	var imagery: Node = globe_scene.get_node_or_null("Georeference3D/GlobeTileLayer")
	if imagery != null:
		imagery.queue_free()

	if camera == null or globe == null or tileset == null:
		print("globe_fly_audit: scene is missing GlobeCameraController / Globe3D / Tileset3D")
		get_tree().quit(1)
		return

	# Tileset3D parses on ready and prints its own summary, so this is a wait, not a guess.
	for _frame in 1200:
		await get_tree().process_frame
		if tileset.get_dataset_radius() > 0.0:
			break

	var radius: float = tileset.get_dataset_radius()
	if radius <= 0.0:
		print("globe_fly_audit: tileset never loaded")
		get_tree().quit(1)
		return

	var lon: float = tileset.get_dataset_longitude()
	var lat: float = tileset.get_dataset_latitude()
	var height: float = tileset.get_dataset_height()

	print("=== globe fly-to audit ===")
	print("  dataset: lon=%.6f lat=%.6f h=%.1f radius=%.1f m (anchor separation %.1f km)" % [
		lon, lat, height, radius, tileset.get_anchor_separation() / 1000.0])

	# Same framing the Home button uses, snapped instead of flown so the pose is exact.
	var framing: float = maxf(radius * 4.0, 400.0)
	var distance: float = globe.ecef_to_local(Vector3.ZERO).distance_to(
		globe.geodetic_to_local(lon, lat, height)) + framing
	camera.orbit_to(lon, lat, distance)

	# Let the traversal select + attach content from the new viewpoint.
	for _frame in 600:
		await get_tree().process_frame
		if tileset.get_loaded_tile_count() > 0:
			break

	# --- 1. where the camera ended up ---
	var camera_geodetic: Vector3 = globe.local_to_geodetic(globe.to_local(camera.global_position))
	_check(absf(camera_geodetic.x - lon) < ANGLE_TOLERANCE_DEGREES,
		"camera longitude %.6f == dataset %.6f" % [camera_geodetic.x, lon])
	_check(absf(camera_geodetic.y - lat) < ANGLE_TOLERANCE_DEGREES,
		"camera latitude %.6f == dataset %.6f" % [camera_geodetic.y, lat])

	# The framing has to leave the dataset on screen, not on the horizon: the Home button is
	# only useful if the thing it flies to is big enough to see.
	var camera_altitude: float = camera.camera_height_above_ellipsoid() - height
	_check(absf(camera_altitude - framing) < maxf(framing * 0.05, 5.0),
		"camera stands %.0f m above the dataset (asked for %.0f m)" % [camera_altitude, framing])

	# --- 2. where the tileset content ended up ---
	var content: Node3D = _first_content_node(tileset)
	if content == null:
		_failures += 1
		print("  FAIL  no tile content attached - nothing was ever in view")
	else:
		var content_geodetic: Vector3 = globe.local_to_geodetic(
			globe.to_local(content.global_position))
		# A tile can be anywhere inside the dataset's own extent, so the tolerance is the
		# radius converted to degrees, not the pixel-tight one above.
		var content_tolerance: float = maxf(radius / 111000.0, ANGLE_TOLERANCE_DEGREES) * 1.5
		_check(absf(content_geodetic.x - lon) < content_tolerance,
			"tile content longitude %.6f is on the dataset (+-%.4f)" %
			[content_geodetic.x, content_tolerance])
		_check(absf(content_geodetic.y - lat) < content_tolerance,
			"tile content latitude %.6f is on the dataset (+-%.4f)" %
			[content_geodetic.y, content_tolerance])

		# --- 3. and that it is in front of the camera, not behind another hemisphere ---
		var projected: Vector2 = _project(globe_scene, camera, content.global_position)
		_check(projected != Vector2(-1.0, -1.0),
			"tile content is in front of the camera (uv=%.3f, %.3f)" % [projected.x, projected.y])
		_check(projected != Vector2(-1.0, -1.0) and
			maxf(absf(projected.x - 0.5), absf(projected.y - 0.5)) < 0.25,
			"tile content lands near the centre of the frame")

	print("=== %s ===" % ("ALL PASS" if _failures == 0 else "%d FAILURES" % _failures))

	# Optional screenshot, only when the renderer is real: headless runs the dummy driver and
	# captures nothing. This is what turns "the numbers agree" into "and it looks right".
	if RenderingServer.get_rendering_device() != null or DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var out := ProjectSettings.globalize_path(OUT_PATH)
		if image.save_png(out) == OK:
			print("wrote ", out, " ", image.get_size())

	get_tree().quit(_failures)

# The tile content nodes are children of the Tileset3D; the debug wireframe is the only
# other one, and it has no bearing on where anything actually rendered.
func _first_content_node(tileset: Node) -> Node3D:
	for child in tileset.get_children():
		if child is Node3D:
			return child as Node3D
	return null

# Viewport normalised coordinates of a world point, or (-1, -1) when it is behind the camera.
func _project(scene: Node, camera: Camera3D, world_point: Vector3) -> Vector2:
	var viewport: Viewport = scene.get_viewport()
	if viewport == null:
		return Vector2(-1.0, -1.0)
	var local: Vector3 = camera.to_local(world_point)
	if local.z > 0.0:  # Godot cameras look down -Z
		return Vector2(-1.0, -1.0)
	var unprojected: Vector2 = camera.unproject_position(world_point)
	return Vector2(unprojected.x / viewport.get_visible_rect().size.x,
		unprojected.y / viewport.get_visible_rect().size.y)
