@tool
extends EditorPlugin
##
## Viewport HUD — draws live camera / performance readouts in the bottom-right
## corner of the active 3D editor viewport.
##
## Godot's built-in "View Information" overlay (camera transform, FPS, draw calls)
## was removed from the editor, which makes tuning 3D Tiles LOD / camera framing
## needlessly blind. This plugin restores the useful part of it, deliberately
## smaller: it draws into the overlay Control that the engine hands to
## [method EditorPlugin._forward_3d_draw_over_viewport], so it is clipped to the
## viewport and needs no horizontal layout of its own.
##
## Usage: enable the addon in Project > Project Settings > Plugins. Press
## [member toggle_keycode] (F9 by default) with the 3D viewport focused to show or
## hide the HUD.

const HUD_MARGIN := 12.0
const HUD_PADDING := 8.0
const LINE_HEIGHT := 16.0
const FONT_SIZE := 13

## Seconds between forced redraws. The engine only calls the forward-draw callback
## when *it* decides the viewport changed; a context-free plugin cannot rely on
## that, so we poll. 0.1s is below the noise floor of reading the numbers by eye
## and costs nothing measurable.
const REDRAW_INTERVAL := 0.1

## Whether the HUD is currently drawn.
var enabled := true

## Toggle the HUD with this key while the 3D viewport has focus.
@export var toggle_keycode: Key = KEY_F9

var _redraw_timer: Timer

## Frame index of the last frame we painted. Guards against both forward-draw
## callbacks firing in the same frame, which would stack two translucent backdrops
## and double-print the text.
var _last_draw_frame := -1


func _enter_tree() -> void:
	# The force-draw variant paints on top of everything, which is what we want:
	# gizmos and the axis indicator should not punch holes in the HUD.
	set_force_draw_over_forwarding_enabled()
	# Guarantee _forward_3d_gui_input keeps firing so the toggle hotkey works even
	# when nothing in the scene is selected/handled by this plugin.
	set_input_event_forwarding_always_enabled()

	_redraw_timer = Timer.new()
	_redraw_timer.name = "ViewportHudRedrawTimer"
	_redraw_timer.wait_time = REDRAW_INTERVAL
	_redraw_timer.autostart = true
	_redraw_timer.one_shot = false
	_redraw_timer.timeout.connect(update_overlays)
	add_child(_redraw_timer)


func _exit_tree() -> void:
	if _redraw_timer:
		_redraw_timer.queue_free()
		_redraw_timer = null


func _get_plugin_name() -> String:
	return "Viewport HUD"


func _forward_3d_force_draw_over_viewport(viewport_control: Control) -> void:
	if not enabled:
		return
	_draw_hud(viewport_control)


## Engine entry point when force-draw forwarding is disabled. Both callbacks funnel
## here; when force-draw is enabled the engine supplements rather than replaces the
## plain variant, so the two must not both run in the same frame.
func _forward_3d_draw_over_viewport(viewport_control: Control) -> void:
	if not enabled:
		return
	_draw_hud(viewport_control)


func _forward_3d_gui_input(_camera: Camera3D, event: InputEvent) -> int:
	# Hotkey rather than an EditorSettings shortcut: EditorSettings.add_shortcut()
	# wants a Shortcut resource and would leave a stale entry behind on reload,
	# whereas this callback is already scoped to 3D viewport input.
	if event is InputEventKey and event.pressed and not event.echo \
			and event.keycode == toggle_keycode and not event.ctrl_pressed:
		enabled = not enabled
		update_overlays()
		return AFTER_GUI_INPUT_STOP
	return AFTER_GUI_INPUT_PASS


# --- drawing -------------------------------------------------------------------

func _draw_hud(overlay: Control) -> void:
	var frame := Engine.get_process_frames()
	if frame == _last_draw_frame:
		return
	_last_draw_frame = frame

	var camera := _find_viewport_camera(overlay)
	if camera == null:
		return

	var lines := _collect_lines(camera)
	var font := overlay.get_theme_default_font()
	var font_size := FONT_SIZE

	# Measure so the backdrop hugs the text. draw_string takes a baseline origin,
	# so width comes from Font.get_string_size and the origin is offset by ascent.
	var text_width := 0.0
	for line in lines:
		text_width = maxf(text_width, font.get_string_size(
			line, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x)

	var viewport_size := overlay.size
	var box_size := Vector2(
		text_width + HUD_PADDING * 2.0,
		LINE_HEIGHT * lines.size() + HUD_PADDING * 2.0)
	var box_pos := Vector2(
		viewport_size.x - box_size.x - HUD_MARGIN,
		viewport_size.y - box_size.y - HUD_MARGIN)

	# Backdrop: near-opaque so terrain colours never wash out the text.
	overlay.draw_rect(Rect2(box_pos, box_size), Color(0.06, 0.07, 0.09, 0.78))
	# Hairline border for a defined edge against bright imagery.
	overlay.draw_rect(Rect2(box_pos, box_size), Color(1, 1, 1, 0.14), false, 1.0)

	var ascent := font.get_ascent(font_size)
	var y := box_pos.y + HUD_PADDING + ascent
	var x := box_pos.x + HUD_PADDING
	for line in lines:
		var color := Color(0.86, 0.90, 0.94)
		if line.begins_with("=="):
			color = Color(0.55, 0.80, 1.0)
			line = line.substr(2)
		overlay.draw_string(font, Vector2(x, y), line,
			HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color)
		y += LINE_HEIGHT


func _collect_lines(camera: Camera3D) -> PackedStringArray:
	var lines := PackedStringArray()

	# --- performance ---
	var fps := Performance.get_monitor(Performance.TIME_FPS)
	var process_ms := Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
	var draw_calls := Performance.get_monitor(
		Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)

	lines.append("==PERF")
	lines.append("FPS       %.1f  (%.2f ms)" % [fps, process_ms])
	lines.append("Draw calls %d" % int(draw_calls))

	# --- camera pose ---
	var pos := camera.global_position
	var rot := camera.global_rotation_degrees
	lines.append("==CAMERA")
	lines.append("pos   %8.2f %8.2f %8.2f" % [pos.x, pos.y, pos.z])
	lines.append("pitch %7.2f" % rot.x)
	lines.append("yaw   %7.2f" % rot.y)
	lines.append("roll  %7.2f" % rot.z)

	# --- projection ---
	lines.append("==PROJECTION")
	match camera.projection:
		Camera3D.PROJECTION_PERSPECTIVE:
			lines.append("fov   %7.2f" % camera.fov)
		Camera3D.PROJECTION_ORTHOGONAL:
			lines.append("size  %7.2f" % camera.size)
		_:
			lines.append("fov   %7.2f (frustum)" % camera.fov)
	lines.append("near  %s" % _format_distance(camera.near))
	lines.append("far   %s" % _format_distance(camera.far))

	return lines


## Distances in an editor viewport span six orders of magnitude (0.01 m to 100 km),
## so a fixed %f reads either as "0.01" or as "100000.00". Pick a unit instead.
func _format_distance(value: float) -> String:
	var magnitude := absf(value)
	if magnitude >= 1000.0:
		return "%.3f km" % (value / 1000.0)
	if magnitude < 1.0:
		return "%.3f m" % value
	return "%.2f m" % value


# --- viewport lookup -----------------------------------------------------------

func _find_viewport_camera(overlay: Control) -> Camera3D:
	# The overlay is parented to the viewport it decorates, so walking up gives us
	# the right camera even when several viewports are open. This is cheaper and
	# more robust than guessing an index into EditorInterface.
	var node := overlay.get_parent()
	while node != null:
		if node is SubViewport:
			return (node as SubViewport).get_camera_3d()
		node = node.get_parent()

	# Fallback: the engine-owned viewport for the main 3D screen.
	return get_editor_interface().get_editor_viewport_3d(0).get_camera_3d()
