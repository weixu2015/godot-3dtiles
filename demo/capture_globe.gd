extends Node3D

# P0 verification harness.
#
# Two modes, both driven from the same scene:
#   1. Geometry audit (always): reads back the Globe3D surface mesh and checks the
#      ellipsoid is centred on the origin, is Y-up (poles on +Y/-Y), has the WGS84 radii,
#      and is not east-west mirrored. This runs headless because it needs no GPU.
#   2. Screenshot (only when a real rendering device is present): saves a PNG for a human
#      to eyeball orientation and texture.
#
# Run:  godot --headless --path demo capture_globe.tscn      (geometry audit)
#       godot --path demo capture_globe.tscn                 (audit + screenshot)

const OUT_PATH := "res://globe_p0_capture.png"

var _failures := 0

func _check(condition: bool, message: String) -> void:
	if condition:
		print("  PASS  ", message)
	else:
		_failures += 1
		print("  FAIL  ", message)

func _ready() -> void:
	var globe := get_node_or_null("Globe3D")
	if globe == null:
		push_error("capture_globe: no Globe3D node in the scene")
		get_tree().quit(1)
		return

	# Let the node finish building its mesh.
	await get_tree().process_frame
	await get_tree().process_frame

	print("=== Globe3D P0 geometry audit ===")
	# The surface MeshInstance3D is an internal child named "Surface".
	var surface: MeshInstance3D = globe.get_node_or_null("Surface")
	_check(surface != null, "surface MeshInstance3D exists")
	if surface != null:
		var mesh := surface.mesh
		_check(mesh != null, "surface mesh is not null")
		if mesh != null:
			var arrays := mesh.surface_get_arrays(0)
			var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			_check(vertices.size() > 0, "surface has vertices (%d)" % vertices.size())

			var min_v := Vector3(INF, INF, INF)
			var max_v := Vector3(-INF, -INF, -INF)
			for v in vertices:
				min_v = min_v.min(v)
				max_v = max_v.max(v)

			var center := (min_v + max_v) * 0.5
			_check(center.length() < 1.0, "mesh centred on local origin (|c|=%.3f)" % center.length())
			_check(absf(min_v.y + 6356752.3) < 50.0,
				"south pole at -C (y_min=%.1f)" % min_v.y)
			_check(absf(max_v.y - 6356752.3) < 50.0,
				"north pole at +C (y_max=%.1f)" % max_v.y)
			_check(absf(max_v.x - 6378137.0) < 50.0,
				"equator reaches +A on X (x_max=%.1f)" % max_v.x)

	# Geography API checks: a point at lon 0, lat 0 must sit at +X in the fallback frame
	# (the frame origin is the same spot, so the local result is the origin itself), and
	# lon +90 must be on -Z (the no-mirror requirement).
	var lon0: Vector3 = globe.geodetic_to_local(0.0, 0.0, 0.0)
	var lon90: Vector3 = globe.geodetic_to_local(90.0, 0.0, 0.0)
	var north: Vector3 = globe.geodetic_to_local(0.0, 90.0, 0.0)
	_check(absf(lon0.length()) < 1.0, "lon0/lat0 maps to the frame origin (|v|=%.3f)" % lon0.length())
	_check(north.y > 6356752.0, "lon0/lat90 is at +Y (y=%.1f)" % north.y)
	# Relative to lon0, lon+90 should be strongly negative on Z.
	var east_delta: Vector3 = lon90 - lon0
	_check(east_delta.z < -6000000.0, "lon+90 goes to -Z, no mirror (z=%.1f)" % east_delta.z)

	print("=== %s ===" % ("ALL PASS" if _failures == 0 else "%d FAILURES" % _failures))

	# Optional screenshot when the renderer is real (not the headless dummy).
	if RenderingServer.get_rendering_device() != null or DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var out := ProjectSettings.globalize_path(OUT_PATH)
		if image.save_png(out) == OK:
			print("capture_globe: wrote ", out)

	get_tree().quit(_failures)

func _notification(what: int) -> void:
	pass
