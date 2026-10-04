extends CanvasLayer

# Runtime debug HUD for the game window (bottom-right), hotkey F3.
#
# demo/addons/viewport_hud/ already draws a status line *in the editor's 3D viewport* - that
# is an EditorPlugin, it does not exist in a running game. This is the runtime counterpart:
# a CanvasLayer created programmatically by globe.gd (no .tscn edit needed), polling the
# same numbers the QA probes measure so a hitch seen on screen can be attributed live.
#
# Everything shown is read, never driven: the HUD owns no state the scene depends on, so
# deleting it (or F3-hiding it) cannot change behaviour.

var _panel: PanelContainer = null
var _label: Label = null
# Throttle: rebuilding the text every frame costs a string format per poll and makes the
# numbers unreadable anyway. 10 Hz is enough to watch a hitch land.
var _accum := 0.0
const REFRESH_SECONDS := 0.1

# Smoothed frame time (the raw delta jitters more than the thing being measured).
var _frame_ms_avg := 16.6

var _scene: Node3D = null
var _camera: Node3D = null
var _camera3d: Camera3D = null
var _tileset: Node = null
var _globe: Node3D = null
var _layer: Node3D = null


static func create(parent: Node, scene: Node3D) -> CanvasLayer:
	var hud: CanvasLayer = load("res://globe_hud.gd").new()
	hud._scene = scene
	parent.add_child(hud)
	return hud


func _ready() -> void:
	layer = 100
	_panel = PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.0, 0.0, 0.0, 0.62)
	style.set_corner_radius_all(4)
	style.content_margin_left = 8.0
	style.content_margin_right = 8.0
	style.content_margin_top = 6.0
	style.content_margin_bottom = 6.0
	_panel.add_theme_stylebox_override("panel", style)

	_label = Label.new()
	_label.add_theme_font_size_override("font_size", 13)
	_label.add_theme_color_override("font_color", Color(0.88, 0.93, 0.88))
	_panel.add_child(_label)

	# Pin to the bottom-right corner and grow up-left, so a widening readout never pushes
	# the panel off-screen. set_anchors_and_offsets_preset (not set_anchors_preset): the
	# latter leaves the offsets alone, which anchored the panel to a zero-size rect and
	# parked it at (-size, -size), off the top-left corner.
	add_child(_panel)
	_panel.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN

	if _scene != null:
		_camera = _scene.get_node_or_null("GlobeCameraController")
		if _camera is Camera3D:
			_camera3d = _camera
		_tileset = _scene.get_node_or_null("Georeference3D/Tileset3D")
		_globe = _scene.get_node_or_null("Georeference3D/Globe3D")
		_layer = _scene.get_node_or_null("Georeference3D/GlobeTileLayer")


func _unhandled_key_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if (event as InputEventKey).keycode == KEY_F3:
			_panel.visible = not _panel.visible


func _process(delta: float) -> void:
	_frame_ms_avg = lerpf(_frame_ms_avg, delta * 1000.0, 0.1)
	_accum += delta
	if _accum < REFRESH_SECONDS:
		return
	_accum = 0.0
	_label.text = _build_text()


func _fmt_float(value: float, decimals: int) -> String:
	return "%.*f" % [decimals, value]


func _human_bytes(bytes: int) -> String:
	if bytes >= 1048576:
		return "%.1f MiB" % (float(bytes) / 1048576.0)
	if bytes >= 1024:
		return "%.1f KiB" % (float(bytes) / 1024.0)
	return "%d B" % bytes


func _fmt_lonlat(degrees: float, decimals: int, positive: String, negative: String) -> String:
	var hemisphere := positive if degrees >= 0.0 else negative
	return "%s%.*f" % [hemisphere, decimals, absf(degrees)]


func _build_text() -> String:
	var lines: PackedStringArray = []

	var fps := Engine.get_frames_per_second()
	var draw_calls := RenderingServer.get_rendering_info(
		RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)
	var primitives := RenderingServer.get_rendering_info(
		RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)
	lines.append("fps %d  frame %s ms  draws %d  prims %s" % [
		fps, _fmt_float(_frame_ms_avg, 1), draw_calls, _human_bytes(primitives)])

	if _camera3d != null:
		lines.append("near %s  far %s" % [
			_fmt_float(_camera3d.near, 2), _fmt_float(_camera3d.far, 0)])

	if _camera != null and _globe != null and _camera3d != null:
		# Godot 4 has no Transform3D.xform(): the * operator is the transform.
		var local: Vector3 = _globe.get_global_transform().affine_inverse() \
			* _camera3d.get_global_position()
		var geo: Vector3 = _globe.call("local_to_geodetic", local)
		lines.append("cam  %s %s  alt %s m%s" % [
			_fmt_lonlat(float(geo.x), 4, "E", "W"),
			_fmt_lonlat(float(geo.y), 4, "N", "S"),
			_fmt_float(float(geo.z), 1),
			"  (flying)" if bool(_camera.call("is_flying")) else ""])
		lines.append("dist %s m  shift thr %s m" % [
			_fmt_float(float(_camera.call("get_distance")), 0),
			_fmt_float(float(_camera.call("get_origin_shift_threshold")), 0)])
		lines.append("origin shift  n=%d  last %s ms / %s m" % [
			int(_camera.call("get_origin_shift_count")),
			_fmt_float(float(_camera.call("get_origin_shift_milliseconds")), 2),
			_fmt_float(float(_camera.call("get_origin_shift_distance")), 0)])

	if _tileset != null:
		var url: String = String(_tileset.get("url"))
		var slash := url.rfind("/")
		var short := url.substr(slash + 1) if slash >= 0 else url
		lines.append("tileset %s" % short)
		lines.append("  loaded %d / rendered %d / in-flight %d  %s" % [
			int(_tileset.call("get_loaded_tile_count")),
			int(_tileset.call("get_last_rendered_count")),
			int(_tileset.call("get_in_flight_count")),
			_human_bytes(int(_tileset.call("get_loaded_bytes")))])

	if _layer != null:
		lines.append("layer  rendered %d / loading %d" % [
			int(_layer.call("get_rendered_tile_count")),
			int(_layer.call("get_loading_tile_count"))])

	return "\n".join(lines)
