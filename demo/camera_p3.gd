extends Node3D

# P3 verification harness for GlobeCameraController.
#
# Checks the things that are easy to get wrong and would not be caught by eyeballing:
#   1. resolve_ellipsoid_center() returns the same point the globe actually drew (with no
#      Georeference3D that is the node's local origin, i.e. the Earth's centre).
#   2. orbit_to() places the camera at the requested distance and looks at the pivot.
#   3. set_camera_pose / get_camera_direction / get_camera_up round-trip.
#   4. set_distance() keeps the orbit radius.
#   5. enforce (via set_distance to a sub-surface distance) never leaves the camera below
#      the ellipsoid.
#
# Run:  godot --headless --path demo camera_p3.tscn

var _failures := 0

func _check(condition: bool, message: String) -> void:
	if condition:
		print("  PASS  ", message)
	else:
		_failures += 1
		print("  FAIL  ", message)

func _ready() -> void:
	await get_tree().process_frame
	await get_tree().process_frame

	var cam: Camera3D = get_node_or_null("GlobeCameraController")
	var globe: Node3D = get_node_or_null("Globe3D")
	_check(cam != null, "GlobeCameraController node exists")
	_check(globe != null, "Globe3D node exists")
	if cam == null or globe == null:
		get_tree().quit(1)
		return

	print("=== GlobeCameraController P3 audit ===")

	# 1. Pivot resolution. The invariant is not a hard-coded distance - it is that the point
	# the camera orbits is the ellipsoid that was actually drawn. With no Georeference3D the
	# globe's local origin *is* the Earth's centre, so both land on the origin; under a
	# Georeference3D both move together. Comparing them catches the drift that once put the
	# pivot a full Earth radius away from the mesh.
	var pivot: Vector3 = cam.resolve_ellipsoid_center()
	var globe_center: Vector3 = globe.ecef_to_local(Vector3.ZERO)
	_check(pivot.distance_to(globe_center) < 1.0,
		"pivot is the drawn ellipsoid's centre (d=%.3f)" % pivot.distance_to(globe_center))

	# 2. orbit_to: distance and facing.
	cam.orbit_to(0.0, 0.0, 20000000.0)
	await get_tree().process_frame
	var pos: Vector3 = cam.global_position
	# `pivot` comes back in the camera's parent space, so go through the parent. Calling
	# cam.to_global(pivot) would treat it as camera-local and add the camera's own position.
	var to_pivot: Vector3 = (cam.get_parent().to_global(pivot) - pos).normalized()
	var forward: Vector3 = cam.get_camera_direction()
	_check(absf(cam.get_distance() - 20000000.0) < 1.0,
		"orbit_to sets distance (%.0f)" % cam.get_distance())
	_check(forward.dot(to_pivot) > 0.99,
		"orbit_to looks at the pivot (dot=%.4f)" % forward.dot(to_pivot))

	# 3. Pose round-trip (parent space, identity parent transform -> global == local).
	var want_pos := Vector3(1000.0, 2000.0, 3000.0)
	var want_dir := Vector3(0.0, 0.0, -1.0)
	var want_up := Vector3(0.0, 1.0, 0.0)
	cam.set_camera_pose(want_pos, want_dir, want_up)
	await get_tree().process_frame
	_check((cam.global_position - want_pos).length() < 1.0,
		"set_camera_pose position round-trips")
	_check(cam.get_camera_direction().dot(want_dir) > 0.999,
		"set_camera_pose direction round-trips")
	_check(cam.get_camera_up().dot(want_up) > 0.999,
		"set_camera_pose up round-trips")

	# 4. set_distance keeps the radius.
	cam.orbit_to(0.0, 0.0, 15000000.0)
	cam.set_distance(12000000.0)
	await get_tree().process_frame
	var radius: float = (cam.global_position - cam.get_parent().to_global(pivot)).length()
	_check(absf(radius - 12000000.0) < 1.0, "set_distance keeps orbit radius (%.0f)" % radius)

	# 5. Sub-surface clamp: ask for a distance well inside the ellipsoid; the controller must
	# not leave the camera there.
	cam.set_distance(100000.0)
	await get_tree().process_frame
	var height: float = cam.camera_height_above_ellipsoid()
	_check(height > 0.0, "camera stays above the ellipsoid after a sub-surface request (h=%.1f)" % height)

	print("=== %s ===" % ("ALL PASS" if _failures == 0 else "%d FAILURES" % _failures))
	get_tree().quit(_failures)
