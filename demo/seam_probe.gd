# Seam-line discrimination probe (GDScript only, no DLL rebuild needed).
# Stages: baseline -> red boundary overlay -> half-texel UV inset -> forced no-texture.
# Screenshots: seamA_baseline / seamB_overlay / seamC_inset / seamD_notex.png
extends SceneTree

var frames := 0
var scene: Node = null
var _stage := 0
var _cooldown := 0
var _saved: Array = []      # [ShaderMaterial, scale, offset] for inset restore
var _overlay_done := false

func _initialize() -> void:
	scene = load("res://globe.tscn").instantiate()
	root.add_child(scene)

func _shot(tag: String) -> void:
	var img: Image = root.get_texture().get_image()
	img.save_png("res://seam_%s.png" % tag)
	print("[probe] shot " + tag)

func _layer() -> Node3D:
	return scene.get_node_or_null("Georeference3D/GlobeTileLayer")

func _tiles() -> Array:
	var layer: Node3D = _layer()
	if layer == null:
		return []
	var out: Array = []
	for child in layer.get_children(true):
		if child is MeshInstance3D:
			out.append(child)
	return out

func _build_overlay() -> void:
	var layer: Node3D = _layer()
	var im: ImmediateMesh = ImmediateMesh.new()
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(1, 0, 0, 1)
	var mi: MeshInstance3D = MeshInstance3D.new()
	mi.mesh = im
	mi.material_override = mat
	mi.name = "SeamOverlay"
	layer.add_child(mi)
	im.surface_begin(Mesh.PRIMITIVE_LINES)
	var n_tiles := 0
	var census: Dictionary = {}
	for t in _tiles():
		if not String(t.name).begins_with("Tile_"):
			continue
		var mesh: Mesh = t.mesh
		if mesh == null:
			continue
		var arrays: Array = mesh.surface_get_arrays(0)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			print("[probe] skip " + String(t.name) + " (no surface arrays)")
			continue
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var total: int = verts.size()
		var columns := 0
		for c in range(2, 513):
			if c * c + 4 * (c - 1) == total:
				columns = c
				break
		if columns == 0:
			continue
		n_tiles += 1
		var lv: String = String(t.name).split("_")[1]
		census[lv] = int(census.get(lv, 0)) + 1
		var top: int = columns * columns
		var ring: Array[int] = []
		for i in range(columns):            # north edge W->E
			ring.append(i)
		for r in range(1, columns):         # east edge
			ring.append(r * columns + columns - 1)
		for i in range(columns):            # south edge E->W
			ring.append(top - 1 - i)
		for r in range(columns - 2, -1, -1):# west edge S->N
			ring.append(r * columns)
		for k in range(ring.size()):
			var a: Vector3 = verts[ring[k]]
			var b: Vector3 = verts[ring[(k + 1) % ring.size()]]
			var ua: Vector3 = a.normalized() * 2500.0
			var ub: Vector3 = b.normalized() * 2500.0
			im.surface_add_vertex(a + ua)
			im.surface_add_vertex(b + ub)
	im.surface_end()
	print("[probe] overlay tiles=" + str(n_tiles) + " census=" + str(census))

func _apply_inset() -> void:
	var count := 0
	for t in _tiles():
		var m: ShaderMaterial = t.get_material_override() as ShaderMaterial
		if m == null:
			continue
		if not bool(m.get_shader_parameter("u_has_texture")):
			continue
		var tex: Texture2D = m.get_shader_parameter("u_albedo_texture")
		if tex == null:
			continue
		var sc: Vector2 = m.get_shader_parameter("u_uv_scale")
		var off: Vector2 = m.get_shader_parameter("u_uv_offset")
		_saved.append([m, sc, off])
		var iw: float = 1.0 / float(tex.get_width())
		var ih: float = 1.0 / float(tex.get_height())
		m.set_shader_parameter("u_uv_scale", Vector2(sc.x - iw, sc.y - ih))
		m.set_shader_parameter("u_uv_offset", Vector2(off.x + 0.5 * iw, off.y + 0.5 * ih))
		count += 1
	print("[probe] inset applied to " + str(count) + " tiles")

func _restore_and_strip_textures() -> void:
	for e in _saved:
		var m: ShaderMaterial = e[0]
		m.set_shader_parameter("u_uv_scale", e[1])
		m.set_shader_parameter("u_uv_offset", e[2])
	var count := 0
	for t in _tiles():
		var m2: ShaderMaterial = t.get_material_override() as ShaderMaterial
		if m2 != null and bool(m2.get_shader_parameter("u_has_texture")):
			m2.set_shader_parameter("u_has_texture", false)
			count += 1
	print("[probe] textures stripped on " + str(count) + " tiles")

func _process(_delta: float) -> bool:
	frames += 1
	var cam: Node3D = scene.get_node_or_null("GlobeCameraController")
	if frames == 5 and cam != null:
		cam.far = 4.0e9
	if frames == 30 and cam != null:
		cam.call("orbit_to", 15.0, 35.0, 7600000.0)
	var flying: bool = cam != null and bool(cam.call("is_flying"))
	_cooldown = maxi(_cooldown - 1, 0)
	if _stage == 0 and frames > 120 and not flying and _cooldown == 0:
		_stage = 1
		_cooldown = 40
		_shot("A_baseline")
		_build_overlay()
		return false
	if _stage >= 1 and _cooldown == 0 and not flying:
		match _stage:
			1:
				_shot("B_overlay")
				_apply_inset()
			2:
				_shot("C_inset")
				_restore_and_strip_textures()
			3:
				_shot("D_notex")
				print("[probe] done")
				quit()
				return true
		_stage += 1
		_cooldown = 40
	if frames > 900:
		print("[probe] FAIL(timeout) stage=" + str(_stage))
		quit()
		return true
	return false
