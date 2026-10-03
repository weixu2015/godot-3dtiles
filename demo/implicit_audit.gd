extends Node3D

# Headless audit for the 3D Tiles 1.1 implicit-tiling dataset. Builds a single Tileset3D
# (implicit georeference), frames it with a fixed camera, lets the scheduler materialise
# implicit subtrees for a while, then saves a screenshot and quits.

const TILESET_URL := "E:/GISData/3D Tiles/1.1/Photogrammetry/tileset.json"
const WARMUP_FRAMES := 600
const SHOT_PATH := "res://audit_1_1.png"

var tileset: Tileset3D
var frames := 0

func _ready() -> void:
	tileset = Tileset3D.new()
	tileset.url = TILESET_URL
	tileset.debug_show_bounding_volume = false
	add_child(tileset)
	tileset.load()

	var camera := Camera3D.new()
	camera.fov = 70.0
	camera.far = 40000.0
	camera.near = 1.0
	add_child(camera)
	camera.global_position = Vector3(0.0, 700.0, 1300.0)
	camera.look_at(Vector3.ZERO, Vector3.UP)
	camera.make_current()

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45.0, 30.0, 0.0)
	add_child(light)

	print("[audit] tileset created, url=", tileset.url)

func _process(_delta: float) -> void:
	frames += 1
	if frames == WARMUP_FRAMES:
		var image := get_viewport().get_texture().get_image()
		if image != null:
			image.save_png(SHOT_PATH)
			print("[audit] screenshot saved to ", SHOT_PATH)
		else:
			print("[audit] viewport image was null")
		print("[audit] tiles=", tileset.get_tile_count(),
			" loaded=", tileset.get_loaded_tile_count(),
			" rendered=", tileset.get_last_rendered_count(),
			" depth=", tileset.get_maximum_depth(),
			" error='", tileset.get_last_error(), "'")
		get_tree().quit()
