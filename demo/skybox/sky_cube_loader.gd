# Loads the six sky box face images into a Cubemap and hands it to the sky shader - the
# three.js CubeTextureLoader flow, in engine terms. A Cubemap cannot be persisted as a .tres
# (ResourceSaver writes _images = [null x6]; layered pixel data is not serializable), so it is
# built at startup from the already-imported face textures instead.
#
# @tool matters here: without it the editor's 3D viewport never runs _ready, the shader's
# samplerCube stays unbound and the sky renders as a white blob behind the scene - the
# panorama this replaced was an inspector resource, so the editor used to show it for free.
@tool
extends WorldEnvironment

const FACES := [
	"res://skybox/sky_px.jpg", "res://skybox/sky_nx.jpg",
	"res://skybox/sky_py.jpg", "res://skybox/sky_ny.jpg",
	"res://skybox/sky_pz.jpg", "res://skybox/sky_nz.jpg",
]


func _ready() -> void:
	var mat := environment.sky.sky_material as ShaderMaterial
	if mat == null or mat.get_shader_parameter("sky_cubemap") != null:
		return
	var images: Array = []
	for path in FACES:
		var img: Image = (load(path) as Texture2D).get_image()
		if img.is_compressed():
			img.decompress()
		images.append(img)
	var cube := Cubemap.new()
	var err: int = cube.create_from_images(images)
	if err != OK:
		push_error("sky cubemap build failed: %d" % err)
		return
	mat.set_shader_parameter("sky_cubemap", cube)
	print("sky cubemap ready: ", cube.get_layers(), " layers, ", cube.get_width(), "x", cube.get_height())
