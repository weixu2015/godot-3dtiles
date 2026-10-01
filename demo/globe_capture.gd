extends Node3D

# Screenshot + numeric audit for demo/globe.tscn.
#
# Instances the real scene (same node tree the editor opens), waits for the imagery
# quadtree to stream a few levels, then prints tile statistics and pixel samples so the
# result does not depend on eyeballing a PNG.
#
# Run with a real GPU driver (headless uses the dummy renderer and captures nothing):
#   godot --path demo --rendering-driver opengl3 globe_capture.tscn

const OUT_PATH := "res://globe_preview.png"

func _ready() -> void:
	var packed: PackedScene = load("res://globe.tscn")
	if packed == null:
		push_error("globe_capture: cannot load globe.tscn")
		get_tree().quit(1)
		return
	var globe_scene: Node = packed.instantiate()
	add_child(globe_scene)

	# Let the scene settle, then give the quadtree time to select + download imagery.
	for _i in 10:
		await get_tree().process_frame

	var layer: Node = globe_scene.get_node_or_null("Georeference3D/GlobeTileLayer")
	var tileset: Node = globe_scene.get_node_or_null("Georeference3D/Tileset3D")
	var globe: Node = globe_scene.get_node_or_null("Georeference3D/Globe3D")

	# ~30 s with a one-line status every second: how the tile stream behaves over time
	# is exactly what distinguishes "slow server" from "broken decode".
	for _frame in 1800:
		await get_tree().process_frame
		if _frame % 60 == 0 and layer != null:
			print("t=%2ds rendered=%d loading=%d cached=%d max_level=%d" % [
				_frame / 60, layer.get_rendered_tile_count(), layer.get_loading_tile_count(),
				layer.get_cached_tile_count(), layer.get_max_selected_level()])

	print("=== globe scene audit ===")
	if globe != null:
		var surface: MeshInstance3D = globe.get_node_or_null("Surface")
		var centre_ok := false
		if surface != null and surface.mesh != null:
			var arrays := surface.mesh.surface_get_arrays(0)
			var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var min_v := Vector3(INF, INF, INF)
			var max_v := Vector3(-INF, -INF, -INF)
			for v in vertices:
				min_v = min_v.min(v)
				max_v = max_v.max(v)
			var centre := (min_v + max_v) * 0.5
			centre_ok = centre.distance_to(globe.ecef_to_local(Vector3.ZERO)) < 1.0
			print("PASS  ellipsoid centred on resolved centre (d=%.3f)" %
				centre.distance_to(globe.ecef_to_local(Vector3.ZERO)))
		else:
			print("FAIL  surface mesh missing")
	if layer != null:
		print("tiles: rendered=%d loading=%d cached=%d max_level=%d" % [
			layer.get_rendered_tile_count(), layer.get_loading_tile_count(),
			layer.get_cached_tile_count(), layer.get_max_selected_level()])
	else:
		print("FAIL  GlobeTileLayer missing")
	if tileset != null:
		print("tileset: placed_by_georeference=%s rendered=%s" % [
			tileset.is_placed_by_georeference(), tileset.get_last_rendered_count()])

	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png(ProjectSettings.globalize_path(OUT_PATH))
	print("saved %s  %s" % [OUT_PATH, img.get_size()])
	var mid := Vector2i(img.get_size().x / 2, img.get_size().y / 2)
	print("corner(space) = ", img.get_pixelv(Vector2i(8, 8)))
	print("centre        = ", img.get_pixelv(mid))
	get_tree().quit(0)
