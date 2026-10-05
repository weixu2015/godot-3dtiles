extends Control
# Split-screen floating-origin comparator.
#
# Two INDEPENDENT side scenes (each in its own SubViewport with its own world), because
# the whole experiment is that one frame keeps its origin at lon 0 / lat 0 while the
# other chases the camera with it. Sharing one world would mean the two frames are not
# two frames but one frame and a flag, and the comparison would measure nothing.
#
#   LEFT  origin shift ON   - the frame origin jumps onto the camera whenever the camera
#                             strays more than the threshold (1000 m) from it, so the
#                             coordinates of everything near the test geometry collapse
#                             from ~6.4e6 m to whatever the camera is away from origin.
#   RIGHT origin shift OFF  - the origin never moves, so the test geometry keeps the
#                             worst float32 magnitudes, |coordinate| ~ 6.4e6 m, for as
#                             long as the camera stays.
#
# The only difference between the two panes is therefore the magnitude of the numbers,
# and everything else - camera pose, probe position, probe children, world - is identical
# by construction. Every mouse event over the whole window drives the LEFT camera; the
# RIGHT camera mirrors the left pose every frame, so drag and wheel are synced too.
#
# Zero C++ involved.

const EARTH_EQUATOR_RADIUS := 6378137.0
const EARTH_POLAR_RADIUS := 6356752.3142451793
const START_DISTANCE := 14000000.0       # centres the whole globe in both panes
const NET_ALTITUDE := 3000.0             # graticule hover height, clear of the surface
const CUBE_SIZE := 4.0
const TRIANGLE_GAP := 30.0               # metres north of the cube, so the two never overlap
const TRIANGLE_HALF := 6.0              # half width of each triangle, metres
const TRIANGLE_GAP_INNER := 5.0         # the 1 m -> 5 m gap chapter 08 asks for, at this scale
const CLOSE_ALTITUDE := 18.0            # eye height for the close-up framing, metres
const CLOSE_BACK := 55.0                # eye offset north of the focus, metres
# The scripted flight and the dolly test. Not fly_to(): its end height is referenced to
# surface_radius_along() while the distance it clamps against is referenced to
# geodeticToYUp(), and those two radii disagree by ~500 m, so a probe cannot aim with it.
# This route lerps longitude, latitude and height the way Cesium's camera.flyTo does and
# ends exactly on the pose it asked for.
const FLY_SECONDS := 2.4
const FLY_ARC_FRACTION := 0.15          # of the height difference, as a bulge
const FLY_ARC_MAX := 3.0e6
const DOLLY_NEAR := 13.0                 # closest standoff in the test, metres
const DOLLY_FAR := 130.0                # furthest standoff, metres
const DOLLY_PERIOD := 5.0               # seconds for one there-and-back cycle
const DOLLY_MID := (DOLLY_NEAR + DOLLY_FAR) * 0.5
const DOLLY_AMP := (DOLLY_FAR - DOLLY_NEAR) * 0.5
# The sweep descends in equal ratios and is sampled whenever it crosses one of these
# altitudes, metres. Log-ish spacing: the interesting range for a 0.5 m grid is the last
# few hundred metres, where one grid step is a visible fraction of the test geometry.
const FLOAT32_MANTISSA := 16777216.0     # 2^24

const TEST_IDLE := 0
const TEST_FLY_IN := 1
const TEST_DOLLY := 2

var _sides: Array = []                   # one dict per pane, see _build_side()
var _viewports: Array = []
var _containers: Array = []
var _buttons: Array = []
var _button: Button = null
var _test_button: Button = null
# The jitter test. A static frame cannot show this effect: both panes are looking at the
# same point from the same pose, so what differs is a FIXED offset of about 0.165 m, and a
# fixed offset in a still frame just looks like "the cube is a bit off centre". The jitter
# only exists as the offset CHANGING, in quanta of the right pane's 0.485 m float32 grid,
# while the camera moves - so the test has to move the camera, continuously, on its own.
var _test_mode := TEST_IDLE
var _test_elapsed := 0.0
var _test_from := Vector3.ZERO           # geodetic lon/lat/height the flight starts from
var _test_to := Vector3.ZERO             # geodetic lon/lat/height the flight ends at
var _test_back := CLOSE_BACK             # standoff the flight aims for
var _test_after := TEST_IDLE             # mode to hand over to on arrival
var _pixels_previous := 0.0
var _jitter_px := 0.0
var _worst_jitter_px := 0.0
var _auto_zoom := false
var _shots := false
# PRECISION_TEST=1 starts the jitter test on load and reports it once a second, then quits:
# the same path the button drives, so the whole flight-in-then-dolly chain can be checked
# without a human watching the window.
var _test_report := false
var _flew := false
var _elapsed := 0.0
var _next_report := 1.0
var _next_shot := 0.0
var _shot_index := 0
var _last_disagreement := 0.0
var _worst_disagreement := 0.0
var _last_pixels := 0.0
var _worst_pixels := 0.0
var _sweep_started := false
var _sweep_index := 0
var _sweep_settle := 0

func _ready() -> void:
	_auto_zoom = OS.get_environment("PRECISION_ZOOM") != ""
	_shots = OS.get_environment("PRECISION_SHOT") != ""
	_test_report = OS.get_environment("PRECISION_TEST") != ""
	set_anchors_preset(Control.PRESET_FULL_RECT)

	var split := HSplitContainer.new()
	split.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(split)
	for i in 2:
		_build_side(split, i)

	# The two buttons live in a bar across the top-right rather than floating individually:
	# a bare Control under a CanvasLayer gets its size from its offsets, so two of them
	# would have to be positioned by hand against each other. The bar itself ignores the
	# mouse so it only eats clicks in its own empty space - the buttons are hit-tested
	# independently of it.
	var layer := CanvasLayer.new()
	add_child(layer)
	var bar := HBoxContainer.new()
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	bar.offset_top = 10.0
	bar.offset_bottom = 42.0
	bar.offset_right = -12.0
	bar.alignment = BoxContainer.ALIGNMENT_END
	bar.add_theme_constant_override("separation", 8)
	bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(bar)

	_button = Button.new()
	_button.text = "fly to cube"
	bar.add_child(_button)
	_button.pressed.connect(_on_fly_to_geometry)
	_buttons.append(_button)

	_test_button = Button.new()
	_test_button.text = "jitter test"
	bar.add_child(_test_button)
	_test_button.pressed.connect(_on_toggle_test)
	_buttons.append(_test_button)
	if _test_report:
		_on_toggle_test()

func _build_side(parent: Node, index: int) -> void:
	var shift_on := index == 0
	var side := {"shift_on": shift_on}

	var panel := VBoxContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	parent.add_child(panel)

	var label := Label.new()
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.add_child(label)
	side["label"] = label

	var container := SubViewportContainer.new()
	container.stretch = true
	container.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# The container must NOT forward mouse events: _input() below captures every event
	# over the whole window and pushes it into the LEFT viewport itself. Left with the
	# default filter, the container under the cursor forwards the same event a second
	# time and the left camera is driven twice per gesture.
	container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_child(container)
	side["container"] = container
	_containers.append(container)

	var viewport := SubViewport.new()
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	# A separate world per side: the left's rebase must not be able to touch the right's
	# content, and two cameras in one world would fight over one Georeference3D.
	viewport.own_world_3d = true
	container.add_child(viewport)
	side["viewport"] = viewport
	_viewports.append(viewport)

	# Bare environment: flat black, no sky, no sun, no atmosphere - but a full AMBIENT
	# term, because the globe surface is a LIT material and renders pitch black with no
	# light source of any kind in the world.
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(1.0, 1.0, 1.0)
	environment.ambient_light_energy = 1.0
	world_env.environment = environment
	viewport.add_child(world_env)

	# The frame is anchored thousands of kilometres AWAY from the test geometry, on the
	# demo's own Philadelphia anchor. That distance is the whole experiment: float32 has a
	# 24-bit mantissa, so the size of the step a coordinate can land on is |coordinate| /
	# 2^24, and with the origin on the test point itself that step is zero on BOTH sides -
	# the right pane would be as precise as the left and the comparison would prove nothing.
	# Anchored in Philadelphia the test point sits at |xyz| ~ 4.6e6 m, a 0.27 m grid, which
	# is the same order as the real case (a dataset flown to from the demo's anchor).
	var authority := LongitudeLatitudeHeight.new()
	authority.longitude = -75.596707
	authority.latitude = 40.038796
	authority.height = 0.0

	var georeference := Georeference3D.new()
	georeference.name = "Georeference3D"
	georeference.set("origin_authority", authority)
	georeference.transform = Transform3D(
		Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0), Vector3(0, 0, 0))
	viewport.add_child(georeference)
	side["georeference"] = georeference

	var globe := Globe3D.new()
	globe.name = "Globe3D"
	globe.set("show_graticule", true)
	globe.set("show_atmosphere", false)
	globe.set("show_sun", false)
	georeference.add_child(globe)
	side["globe"] = globe

	# The test geometry. Ordinary Node3D children are NOT carried by a rebase -
	# GlobeCameraController::rebase_content() only touches the Georeference3D's direct
	# children (GlobeTileLayer / Globe3D / Tileset3D) - so the probe's own position is
	# re-derived from the frame every frame instead. That expression is the frame's own
	# answer for the lon 0 / lat 0 point, which is exactly where the just-rebased surface
	# put it, so the probe stays glued to the ground through any number of shifts.
	var probe := Node3D.new()
	probe.name = "Probe"
	globe.add_child(probe)
	side["probe"] = probe
	_build_probe_content(globe, probe)

	# The camera hangs under the frame it drives. This is not a style choice: with two
	# frames in one tree resolve_frame_node()'s ancestor walk hands a sibling camera WHICH
	# frame it belongs to, and its whole-tree fallback finds whichever Georeference3D comes
	# first - which would silently render the right pane through the left, shifted, frame.
	var camera := GlobeCameraController.new()
	camera.name = "Camera"
	camera.set_origin_shift_enabled(shift_on)
	# manage_clip derives near/far from the camera's altitude against the UNMOVED
	# ellipsoid; on the shifted side that altitude stays large even while the camera sits
	# next to the probe, and the derived near plane clips the whole test geometry.
	camera.set_manage_clip(false)
	camera.near = 0.02
	camera.far = 6.0e7
	georeference.add_child(camera)
	# The initial pose is set explicitly rather than with orbit_to(). orbit_to() aims with
	# Godot's look_at() and a hardcoded world +Y up, and at the one place this test cares
	# about - lon 0, lat 0, the prime meridian on the equator - the outward normal happens to
	# line up with a world axis, so look_at() reports "target and up vectors are colinear"
	# and picks a roll for us. The frame can say exactly what up is here: the tangent towards
	# the north pole, which is perpendicular to the radius by construction.
	var pivot: Vector3 = camera.resolve_ellipsoid_center()
	var target: Vector3 = globe.geodetic_to_local(0.0, 0.0, 0.0)
	var north: Vector3 = (globe.geodetic_to_local(0.0, 1.0, 0.0) - target).normalized()
	var outward: Vector3 = (target - pivot).normalized()
	camera.set_camera_pose(pivot + outward * START_DISTANCE, -outward, north)
	side["camera"] = camera

	_sides.append(side)

func _build_probe_content(globe: Node3D, probe: Node3D) -> void:
	var up := _local_axis(globe, 0.0, 1.0)
	var north := _local_axis(globe, 1.0, 0.0)

	# The translucent test cube, sitting on the equator, half above the surface.
	var cube := MeshInstance3D.new()
	cube.name = "Cube"
	var box := BoxMesh.new()
	box.size = Vector3(CUBE_SIZE, CUBE_SIZE, CUBE_SIZE)
	cube.mesh = box
	var cube_material := StandardMaterial3D.new()
	cube_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	cube_material.albedo_color = Color(1.0, 0.55, 0.1, 0.45)
	cube_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	cube_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	cube.material_override = cube_material
	cube.position = up * (CUBE_SIZE * 0.5)
	probe.add_child(cube)

	# The chapter-08 experiment: two coplanar triangles with a gap between them, plus a
	# fixed red reference dot in the gap. Placed north of the cube by TRIANGLE_GAP so the
	# two never overlap at any zoom level, and lifted a little clear of the surface so the
	# faceted globe mesh (inscribed, so it sags up to a kilometre below the ellipsoid
	# between its vertices) cannot swallow them.
	var triangles := Node3D.new()
	triangles.name = "Triangles"
	triangles.position = north * TRIANGLE_GAP + up * 0.3
	probe.add_child(triangles)

	var surface_mesh := ArrayMesh.new()
	var vertices := PackedVector3Array()
	vertices.push_back(Vector3(-TRIANGLE_HALF - TRIANGLE_GAP_INNER, 0.0, -TRIANGLE_HALF))
	vertices.push_back(Vector3(-TRIANGLE_GAP_INNER, 0.0, -TRIANGLE_HALF))
	vertices.push_back(Vector3(-TRIANGLE_HALF, 0.0, TRIANGLE_HALF))
	vertices.push_back(Vector3(TRIANGLE_GAP_INNER, 0.0, -TRIANGLE_HALF))
	vertices.push_back(Vector3(TRIANGLE_HALF + TRIANGLE_GAP_INNER, 0.0, -TRIANGLE_HALF))
	vertices.push_back(Vector3(TRIANGLE_HALF, 0.0, TRIANGLE_HALF))
	var packed := []
	packed.resize(Mesh.ARRAY_MAX)
	packed[Mesh.ARRAY_VERTEX] = vertices
	surface_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, packed)
	var triangle_material := StandardMaterial3D.new()
	triangle_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	triangle_material.albedo_color = Color(0.95, 0.85, 0.2)
	# Godot's front face is clockwise; the CCW winding above is invisible with default
	# culling, so the test material is double sided.
	triangle_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	surface_mesh.surface_set_material(0, triangle_material)
	var triangle_node := MeshInstance3D.new()
	triangle_node.name = "TrianglesMesh"
	triangle_node.mesh = surface_mesh
	triangles.add_child(triangle_node)

	var dot := MeshInstance3D.new()
	dot.name = "CentreDot"
	var dot_mesh := SphereMesh.new()
	dot_mesh.radius = 0.25
	dot_mesh.height = 0.5
	dot.mesh = dot_mesh
	var dot_material := StandardMaterial3D.new()
	dot_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	dot_material.albedo_color = Color(0.9, 0.1, 0.1)
	dot_material.no_depth_test = true
	dot.material_override = dot_material
	triangles.add_child(dot)

	# The 20-degree net, authored as offsets from the probe so that it is invariant to a
	# shift: a rebase subtracts the same delta from the probe's position and from every
	# one of these vertices, and the difference is what is drawn.
	var net := MeshInstance3D.new()
	net.name = "Graticule20"
	net.mesh = _build_net_mesh(globe)
	var net_material := StandardMaterial3D.new()
	net_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	net_material.albedo_color = Color(1.0, 1.0, 1.0, 0.4)
	net_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	net_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	net.material_override = net_material
	net.position = up * NET_ALTITUDE
	probe.add_child(net)

# A unit axis of the frame, recovered from the frame itself rather than assumed: the
# difference of two geodetic_to_local calls is a real tangent of the ellipsoid at lon 0.
func _local_axis(globe: Node3D, latitude_degrees: float, height: float) -> Vector3:
	var origin: Vector3 = globe.geodetic_to_local(0.0, 0.0, 0.0)
	var tip: Vector3 = globe.geodetic_to_local(0.0, latitude_degrees, height)
	return (tip - origin).normalized()

func _build_net_mesh(globe: Node3D) -> ArrayMesh:
	var origin: Vector3 = globe.geodetic_to_local(0.0, 0.0, 0.0)
	var up: Vector3 = _local_axis(globe, 0.0, 1.0)

	var mesh := ArrayMesh.new()
	var vertices := PackedVector3Array()
	var add_parallels := func(lat_degrees: float) -> void:
		var lon := -180.0
		while lon < 180.0:
			vertices.push_back(_net_vertex(globe, origin, up, lon, lat_degrees))
			vertices.push_back(_net_vertex(globe, origin, up, lon + 2.0, lat_degrees))
			lon += 2.0
	var add_meridians := func(lon_degrees: float) -> void:
		var lat := -90.0
		while lat < 90.0:
			vertices.push_back(_net_vertex(globe, origin, up, lon_degrees, lat))
			vertices.push_back(_net_vertex(globe, origin, up, lon_degrees, lat + 2.0))
			lat += 2.0
	for lat in [-80.0, -60.0, -40.0, -20.0, 0.0, 20.0, 40.0, 60.0, 80.0]:
		add_parallels.call(lat)
	for lon in [-180.0, -160.0, -140.0, -120.0, -100.0, -80.0, -60.0, -40.0, -20.0,
			0.0, 20.0, 40.0, 60.0, 80.0, 100.0, 120.0, 140.0, 160.0]:
		add_meridians.call(lon)

	var packed := []
	packed.resize(Mesh.ARRAY_MAX)
	packed[Mesh.ARRAY_VERTEX] = vertices
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, packed)
	return mesh

func _net_vertex(globe: Node3D, origin: Vector3, up: Vector3,
		lon_degrees: float, lat_degrees: float) -> Vector3:
	var point: Vector3 = globe.geodetic_to_local(lon_degrees, lat_degrees, 0.0)
	return point - origin + up * NET_ALTITUDE

func _process(delta: float) -> void:
	if _sides.size() < 2:
		return
	_elapsed += delta

	# ---- 1. whatever drives the LEFT camera has to happen BEFORE the mirror below, or the
	# right pane is measured against a pose the left has already left behind. The sweep
	# moves the left camera by hundreds of metres per step, a thousand times the effect
	# being measured.
	if _shots and not _flew and _elapsed > 0.4:
		_flew = true
		_on_fly_to_geometry()

	# The probe rides the ground. Node.position is a frame-local number, so re-deriving it
	# through the frame is the same delta the rebase applied to the surface vertices -
	# which is what keeps the cube, the triangles and the net attached to the planet
	# across an unlimited number of origin shifts. It runs before the sweep drives the
	# camera, so every framing is computed from the probe's settled position.
	for i in _sides.size():
		var side: Dictionary = _sides[i]
		side["probe"].position = side["globe"].geodetic_to_local(0.0, 0.0, 0.0)

	if _auto_zoom:
		_drive_sweep()
	elif _test_mode == TEST_FLY_IN:
		_fly_in(delta)
	elif _test_mode == TEST_DOLLY:
		_dolly(delta)

	# The right pane mirrors the left. The two frames have the same node transform but NOT
	# the same origin: after a shift the left frame's origin sits on its camera, so a number
	# that means "1.4e7 m up" on the left means "0" on the right - copying it verbatim put
	# the right camera at the right frame's origin, i.e. under the ground. The pose is
	# therefore bridged through geodetic coordinates, which are the one representation both
	# frames agree on. The two axes need no bridge: a shift is a pure translation with the
	# basis frozen on the declared anchor, which is the same anchor on both sides, so the
	# parent-space axes are identical.
	var left: Dictionary = _sides[0]
	var right: Dictionary = _sides[1]
	var left_camera: Camera3D = left["camera"]
	var geodetic: Vector3 = left["globe"].local_to_geodetic(left_camera.position)
	var mirrored: Vector3 = right["globe"].geodetic_to_local(geodetic.x, geodetic.y, geodetic.z)
	right["camera"].set_camera_pose(mirrored, left_camera.get_camera_direction(),
			left_camera.get_camera_up())

	for i in _sides.size():
		var side: Dictionary = _sides[i]
		var camera: Camera3D = side["camera"]
		var probe: Node3D = side["probe"]
		# Manual clip planes, and the ratio between them matters more than either value.
		# With near 0.64 m and far 6e7 m - a ratio of 9.4e7 - the compatibility renderer
		# stops rasterising the scene altogether: every mesh, lit or unshaded, transparent
		# or opaque, renders as background. At 9.4e6 the same frame draws correctly. The
		# far plane therefore grows with the near plane instead of sitting at a fixed
		# planetary distance, and only reaches the far side of the Earth when the camera
		# is actually far enough away to need it.
		var distance: float = camera.position.distance_to(probe.position)
		camera.near = clampf(distance * 0.1, 1.0, 1.0e5)
		camera.far = maxf(camera.near * 1.0e3, distance + 1.3e7)

		# The number that decides whether the picture can jitter: |coordinate| is what
		# float32 is quantising, and 2^-24 of it is the step size a vertex can land on.
		var magnitude: float = probe.position.length()
		var step: float = magnitude / FLOAT32_MANTISSA
		var shift_count: int = camera.get_origin_shift_count()
		var shift_distance: float = camera.get_origin_shift_distance()
		side["label"].text = ("origin shift %s   n=%d\n" % [
				"ON " if side["shift_on"] else "OFF", shift_count]) + \
			("probe |xyz| %s m   float32 step %s m   cam %.1f m\n" % [
				_sci(magnitude), _sci(step), distance]) + \
			("last shift %.0f m in %.2f ms" % [shift_distance,
				camera.get_origin_shift_milliseconds()])

	# The measurement. Both panes are looking at the same physical point with the same pose,
	# so how far apart the two frames place that point IS the error the left pane does not
	# have: on the left the number is a few hundred metres and lands on the float32 grid to
	# within a micron, on the right it is a coordinate of eight million metres and lands on
	# a 0.5 m grid. It is measured two ways because the two answers are what the user
	# actually sees.
	#
	#   metres  - the same point pushed through each frame's own geodetic conversion. The
	#             two panes live in separate worlds, so their raw node coordinates are not
	#             comparable at all (they differ by the whole 8.1e6 m offset); geodetic
	#             degrees and metres are the one representation both frames agree on.
	#   pixels  - where each pane actually draws the cube. Same pose, same viewport size,
	#             so the screen-space gap between the two is the discrepancy in the picture.
	var left_probe: Node3D = _sides[0]["probe"]
	var right_probe: Node3D = _sides[1]["probe"]
	var left_geo: Vector3 = _sides[0]["globe"].local_to_geodetic(left_probe.position)
	var right_geo: Vector3 = _sides[1]["globe"].local_to_geodetic(right_probe.position)
	var disagreement: float = _geodetic_metres(left_geo, right_geo)
	_last_disagreement = disagreement
	_worst_disagreement = maxf(_worst_disagreement, disagreement)

	var left_cube: Node3D = left_probe.get_node_or_null("Cube")
	var right_cube: Node3D = right_probe.get_node_or_null("Cube")
	var pixel_offset := 0.0
	if left_cube != null and right_cube != null:
		var measure_left: Camera3D = _sides[0]["camera"]
		var measure_right: Camera3D = _sides[1]["camera"]
		pixel_offset = measure_left.unproject_position(left_cube.global_position).distance_to(
				measure_right.unproject_position(right_cube.global_position))
		_last_pixels = pixel_offset
		_worst_pixels = maxf(_worst_pixels, pixel_offset)
	# The same line goes on BOTH labels. It is not decoration: the panes are laid out by an
	# HSplitContainer, so a label that is one line taller on one side makes that side's
	# SubViewport shorter, which changes its aspect and shifts every projected coordinate -
	# the pixel comparison below then measures the layout, not the geometry.
	var measurement := "\npanes disagree by %s m  =  %.1f px   (worst %s m / %.1f px)" % [
			_sci(disagreement), pixel_offset,
			_sci(_worst_disagreement), _worst_pixels]
	# The jitter readout, and the reason the test moves the camera at all. Both panes are
	# geometrically identical, so the offset between them should be a constant. Its change
	# from one frame to the next is therefore nothing but quantisation, in pixels, on
	# screen: the left pane's contribution to it is below a thousandth of a pixel.
	_jitter_px = absf(pixel_offset - _pixels_previous)
	_pixels_previous = pixel_offset
	_worst_jitter_px = maxf(_worst_jitter_px, _jitter_px)
	measurement += "\njitter %.2f px this frame   (worst %.2f px)" % [
			_jitter_px, _worst_jitter_px]
	for i in _sides.size():
		var side: Dictionary = _sides[i]
		(side["label"] as Label).text += measurement

	if _shots:
		if _elapsed >= _next_report:
			_next_report = _elapsed + 1.0
			_save_frames()
			for i in _sides.size():
				var side: Dictionary = _sides[i]
				var camera: Camera3D = side["camera"]
				var probe: Node3D = side["probe"]
				print("[precision_compare] %s shift=%s  cam_local=%s  probe_local=%s  " % [
						"ON " if side["shift_on"] else "OFF",
						camera.get_origin_shift_count(), camera.position, probe.position])
				print("                 cam_world=%s  probe_world=%s  near=%s far=%s  " % [
						camera.global_position, probe.global_position, camera.near, camera.far])
				print("                 geo(cam)=%s  alt=%.1f m  disagree %s m" % [
						side["globe"].local_to_geodetic(camera.position),
						camera.camera_height_above_ellipsoid(),
						_sci(_last_disagreement)])
		if _elapsed >= 5.0:
			_shots = false
			_save_frames()
			get_tree().quit()

	# The sweep reports and writes its pair after the mirror, so every frame pair it saves
	# is a matched pair and every number it prints is measured on a settled pose.
	if _test_report and _elapsed >= _next_report:
		_next_report = _elapsed + 0.5
		var driver: Camera3D = _sides[0]["camera"]
		var probe_node: Node3D = _sides[0]["probe"]
		var magnitude: float = probe_node.position.length()
		var to_cube: float = driver.global_position.distance_to(probe_node.global_position)
		_save_frames()
		print("[test] %s  cam-cube %6.1f m  |probe| %s m  offset %s m  jitter %5.2f px (worst %5.2f)" % [
				["fly in", "fly in", "dolly"][_test_mode], to_cube,
				_sci(magnitude), _sci(_last_disagreement), _jitter_px, _worst_jitter_px])
		if _elapsed > 16.0:
			print("[precision_compare] test done, worst jitter %.2f px/frame" % _worst_jitter_px)
			get_tree().quit()

	if _auto_zoom:
		if _sweep_started and _sweep_settle == 0 and _sweep_index > 0:
			_save_frames()
			var driver: Camera3D = _sides[0]["camera"]
			var magnitude: float = (_sides[0]["probe"] as Node3D).position.length()
			var to_cube: float = driver.global_position.distance_to(
					(_sides[0]["probe"] as Node3D).global_position)
			print("[sweep] standoff %6.1f m  cam-cube %6.1f m  |probe| %s m  float32 step %s m  disagree %s m  %.2f px" % [
					SWEEP_STANDOFFS[_sweep_index - 1], to_cube, _sci(magnitude),
					_sci(magnitude / FLOAT32_MANTISSA),
					_sci(_last_disagreement), _last_pixels])
			if _sweep_index >= SWEEP_STANDOFFS.size():
				print("[precision_compare] sweep done, %d pairs, worst disagreement %s m" % [
						_shot_index, _sci(_worst_disagreement)])
				get_tree().quit()
		elif _elapsed > 0.5:
			# One extra frame for the first shot to settle in the texture before it is read.
			_sweep_started = true

# The sweep dollies in along the raking view, one properly framed shot per standoff, using
# the same _frame_probe() the close-up button uses. Driving it with set_distance() instead
# looked tidier but did not move the camera at all in this pose - the altitude sat at 8808 m
# for the whole run - and a probe that cannot reproduce its own close-up is not measuring
# anything. Standoff rather than altitude is the interesting axis: the eye height barely
# changes how big a 0.5 m grid step is on screen, the distance does, and the grid step is
# what does not shrink.
const SWEEP_STANDOFFS := [130.0, 100.0, 80.0, 68.0, 58.0, 52.0, 47.0, 44.0, 41.0, 39.0, 37.0]
func _drive_sweep() -> void:
	if not _sweep_started:
		# Hold still for half a second first: the first frames are the ones where the origin
		# shift lands and the surface is rebuilt, and a shot taken across that is not a
		# measurement of anything.
		if _elapsed > 0.5:
			_sweep_started = true
		return
	if _sweep_settle > 0:
		# One frame of settle after each reframe, so the frame that gets written is one the
		# new pose was actually rendered with.
		_sweep_settle -= 1
		return
	if _sweep_index >= SWEEP_STANDOFFS.size():
		return
	_frame_probe(_sides[0]["camera"], _sides[0]["globe"], _sides[0]["probe"],
			CLOSE_ALTITUDE, SWEEP_STANDOFFS[_sweep_index])
	_sweep_index += 1
	_sweep_settle = 1

func _save_frames() -> void:
	if _viewports.is_empty():
		return
	var dir := ProjectSettings.globalize_path("res://").path_join("../log")
	_shot_index += 1
	for i in _viewports.size():
		var image: Image = _viewports[i].get_texture().get_image()
		image.save_png("%s/frame_%03d_%s.png" % [dir, _shot_index,
				"ON" if i == 0 else "OFF"])
	print("[precision_compare] saved pair %d" % _shot_index)

# GDScript's % operator has no %e, and the numbers this probe exists to show span nine
# orders of magnitude, so they are formatted by hand rather than dropped to a %f.
func _sci(value: float) -> String:
	if value == 0.0:
		return "0"
	var exponent := 0
	var mantissa := value
	while absf(mantissa) >= 10.0:
		mantissa /= 10.0
		exponent += 1
	while absf(mantissa) < 1.0:
		mantissa *= 10.0
		exponent -= 1
	return "%.2fe%d" % [mantissa, exponent]

# Ground distance between two geodetic positions, metres. `a` and `b` are
# (longitude degrees, latitude degrees, height metres) as local_to_geodetic() returns them.
func _geodetic_metres(a: Vector3, b: Vector3) -> float:
	var mean_latitude: float = (a.y + b.y) * 0.5 * PI / 180.0
	var east: float = (a.x - b.x) * PI / 180.0 * EARTH_EQUATOR_RADIUS * cos(mean_latitude)
	var north: float = (a.y - b.y) * PI / 180.0 * EARTH_POLAR_RADIUS
	return Vector3(east, north, a.z - b.z).length()

# Puts the camera at a fixed, known pose next to the test geometry.
#
# Deliberately not fly_to(): this is a measuring instrument, and fly_to() lands about
# 500 m higher than the centre distance it is given (its end height is referenced to
# surface_radius_along(), while the distance it clamps against is referenced to
# geodeticToYUp(), and the two radii disagree by that much). A repeatable close-up wants a
# repeatable pose, and set_camera_pose() also syncs the controller's own distance so the
# wheel keeps working from there.
func _frame_probe(camera: Camera3D, globe: Node3D, probe: Node3D,
		altitude: float = CLOSE_ALTITUDE, back: float = CLOSE_BACK) -> void:
	var pose: Array = _probe_pose(camera, globe, probe, altitude, back)
	camera.set_camera_pose(pose[0], pose[1], pose[2])
	if _shots:
		print("[frame_probe] standoff %.1f m  alt %.1f m  geo=%s" % [
				back, camera.camera_height_above_ellipsoid(),
				globe.local_to_geodetic(camera.position)])

# The close-up pose without applying it: [eye, direction, up] in the frame's local space.
# Split out from _frame_probe() because the scripted flight needs to know where it is
# going before it gets there.
func _probe_pose(camera: Camera3D, globe: Node3D, probe: Node3D,
		altitude: float, back: float) -> Array:
	var pivot: Vector3 = camera.resolve_ellipsoid_center()
	var target: Vector3 = probe.position
	var outward: Vector3 = (target - pivot).normalized()
	var north: Vector3 = _local_axis(globe, 1.0, 0.0)
	# Raking view, almost level with the ground. The test geometry is flat, so a camera
	# looking down at it sees the triangles edge-on and they vanish; a low oblique angle
	# is also the angle at which jitter reads most clearly, because a vertex that hops
	# along the grid slides visibly across the picture instead of just shimmering.
	# Aimed at the cube, not at the midpoint of the two pieces of test geometry: the camera
	# stands north of both, so aiming at the midpoint walks the cube towards the bottom edge
	# as the standoff shrinks. The triangles then sit between the camera and the cube, in
	# the lower half of the frame, which is the arrangement that makes a vertex hopping on
	# the float32 grid easiest to see against a straight edge.
	var eye: Vector3 = target + outward * altitude + north * back
	# Up is the local up, i.e. the radial direction at the test point - it has to be. A raking
	# shot looks south and down, so the north tangent is nearly PARALLEL to the view, and
	# set_camera_pose() orthogonalises whatever it is handed: given the north tangent it keeps
	# a 25-degree sliver aimed at the ground and the whole picture comes out upside down,
	# horizon at the bottom. The radial up is 26 degrees off the view and survives intact.
	return [eye, (target - eye).normalized(), outward]

# The scripted flight. Route is a lerp of longitude, latitude and height - Cesium's own
# choice, and available here because geodetic_to_local() and local_to_geodetic() bracket
# the frame - with a bulge on the height so the camera climbs over the limb on the way in
# from orbit instead of cutting through the planet.
func _fly_in(delta: float) -> void:
	var side: Dictionary = _sides[0]
	var camera: Camera3D = side["camera"]
	var globe: Node3D = side["globe"]
	var probe: Node3D = side["probe"]
	_test_elapsed += delta
	var t: float = clampf(_test_elapsed / FLY_SECONDS, 0.0, 1.0)
	var ease: float = _quintic_in_out(t)
	var longitude: float = lerpf(_test_from.x, _test_to.x, ease)
	var latitude: float = lerpf(_test_from.y, _test_to.y, ease)
	var arc: float = minf(FLY_ARC_FRACTION * absf(_test_from.z - _test_to.z), FLY_ARC_MAX)
	var height: float = lerpf(_test_from.z, _test_to.z, ease) + arc * sin(PI * t)
	var position: Vector3 = globe.geodetic_to_local(longitude, latitude, height)
	var aim: Array = _probe_pose(camera, globe, probe, CLOSE_ALTITUDE, _test_back)
	if t >= 1.0:
		# Snap onto the pose rather than letting the last in-flight frame define it: by now
		# the camera is already within float32 of the target, so aiming at the target point
		# normalises a near-zero vector and set_camera_pose() silently keeps the old basis.
		camera.set_camera_pose(aim[0], aim[1], aim[2])
		_test_mode = _test_after
		_test_elapsed = 0.0
		return
	# Aimed at the cube, which is a fixed physical point and therefore a stable aim even
	# though the frame origin is moving under the camera for the whole flight.
	var focus: Vector3 = probe.position
	var up: Vector3 = (globe.geodetic_to_local(longitude, latitude, height + 1.0)
			- position).normalized()
	camera.set_camera_pose(position, (focus - position).normalized(), up)

# The dolly. A sine, not a triangle, so the standoff - and therefore the pixel scale - is
# continuous: a rate that jumps would move the whole picture and hide the very effect the
# test is for. From the far end the right pane's grid step is under a pixel; by the near
# end the same 0.485 m is tens of pixels, and the cube visibly hops in whole pixels while
# the left pane's cube stays welded to the ground.
func _dolly(delta: float) -> void:
	_test_elapsed += delta
	var phase: float = fmod(_test_elapsed, DOLLY_PERIOD) / DOLLY_PERIOD
	var back: float = DOLLY_MID + DOLLY_AMP * sin(TAU * phase)
	var side: Dictionary = _sides[0]
	_frame_probe(side["camera"], side["globe"], side["probe"], CLOSE_ALTITUDE, back)

func _on_toggle_test() -> void:
	if _test_mode != TEST_IDLE:
		_stop_test()
		return
	_start_flight(TEST_DOLLY)
	if _test_button != null:
		_test_button.text = "stop test"

# Flies from wherever the left camera is to the close-up pose, then hands over to
# `p_after`. Both entry points go through here so there is exactly one definition of where
# "the cube" is.
func _start_flight(p_after: int) -> void:
	if _sides.is_empty():
		return
	var side: Dictionary = _sides[0]
	var camera: Camera3D = side["camera"]
	var globe: Node3D = side["globe"]
	_test_from = globe.local_to_geodetic(camera.position)
	_test_back = DOLLY_MID if p_after == TEST_DOLLY else CLOSE_BACK
	var aim: Array = _probe_pose(camera, globe, side["probe"], CLOSE_ALTITUDE, _test_back)
	_test_to = globe.local_to_geodetic(aim[0])
	_test_after = p_after
	_test_mode = TEST_FLY_IN
	_test_elapsed = 0.0

func _stop_test() -> void:
	_test_mode = TEST_IDLE
	_test_elapsed = 0.0
	if _test_button != null:
		_test_button.text = "jitter test"

# Cesium's QUINTIC_IN_OUT, the same curve flyTo uses: zero velocity at both ends, so the
# flight neither starts nor stops with a jerk that would be mistaken for jitter.
func _quintic_in_out(t: float) -> float:
	if t < 0.5:
		return 16.0 * pow(t, 5.0)
	var inverse: float = -2.0 * t + 2.0
	return 1.0 - (inverse * inverse * inverse * inverse * inverse) * 0.5

func _on_fly_to_geometry() -> void:
	_start_flight(TEST_IDLE)

func _input(event: InputEvent) -> void:
	# Every mouse event, wherever the cursor is over the window, is captured here at the
	# earliest stage and pushed into the LEFT viewport's own pipeline, so the controller's
	# native drag, wheel easing and inertia behave exactly as they do in globe.tscn. The
	# right pane never sees raw input; it mirrors the left pose in _process().
	if not (event is InputEventMouse) or _viewports.is_empty():
		return
	# Over a button: leave it alone, or the click would never reach it. Checked as a rect
	# rather than by hit-testing because these buttons live in a CanvasLayer, outside the
	# Control tree the rest of this node lives in.
	for i in _buttons.size():
		var button: Button = _buttons[i]
		if button.get_global_rect().has_point(event.position):
			return
	# A click in the viewport hands control back to the mouse: the test drives the camera
	# every frame and would fight a drag for the whole gesture.
	if event is InputEventMouseButton and event.pressed and _test_mode != TEST_IDLE:
		_stop_test()
	var local_event: InputEventMouse = event.duplicate()
	var container: Control = _containers[0]
	local_event.position = local_event.position - container.global_position
	_viewports[0].push_input(local_event)
	get_viewport().set_input_as_handled()
