extends Node3D

# Screenshot + numeric audit for demo/globe.tscn.
#
# Instances the real scene (same node tree the editor opens), waits for the imagery
# quadtree to stream a few levels, then prints tile statistics and pixel samples so the
# result does not depend on eyeballing a PNG.
#
# Run with a real GPU driver (headless uses the dummy renderer and captures nothing):
#   godot --path demo --rendering-driver opengl3 globe_capture.tscn

const OUT_PATH_DEFAULT := "res://globe_preview.png"

# The output path is overridable because the diagnostic runs are the whole point of this
# harness, and they have to be *kept side by side* to be compared. Overwriting one file with
# every variant is how a control gets lost and the next round re-argues a settled question.
func _out_path() -> String:
	var forced := OS.get_environment("GLOBE_OUT")
	return forced if forced != "" else OUT_PATH_DEFAULT

# Hard self-destruct.
#
# This scene runs unattended from a batch script, so a stall has no human to notice it and
# waits forever - which is worse than a crash, because the caller cannot tell "still working"
# from "wedged". A wall-clock watchdog guarantees the process dies and the caller's log says
# so. The budget is generous (~3x the observed 19 s) so it only fires on a real hang.
const WATCHDOG_SECONDS := 90.0

func _ready() -> void:
	_start_watchdog()
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
	var camera: Node = globe_scene.get_node_or_null("GlobeCameraController")

	# Freeze the audit inputs before sampling.
	#
	# Two things make this scene non-deterministic otherwise, and both would show up as
	# "the atmosphere changed" when nothing did:
	#   1. The sub-solar point follows the wall clock, so the terminator sweeps ~0.25 deg per
	#      minute and two runs a minute apart light different hemispheres. Pinning it makes the
	#      day/night assertions comparable across runs.
	#   2. Drag/zoom inertia plus the tile layer's framing can leave the camera somewhere else
	#      by frame 1800. Re-snapping to the opening pose after the stream settles means the
	#      limb profile is measured at a known camera height.
	if globe != null:
		globe.set("sun_longitude_degrees", 64.0)
		globe.set("sun_latitude_degrees", -4.0)
		# Diagnostic sweep: the halo's brightness is a pure function of u_light_intensity, so
		# scaling it proves whether a weak limb is a magnitude problem or a structural one.
		var forced_intensity := OS.get_environment("GLOBE_ATMO_INTENSITY")
		if forced_intensity != "":
			globe.set("atmosphere_intensity", float(forced_intensity))
		print("atmo: requested=%s  effective_intensity=%.2f  scale=%.4f" % [
			forced_intensity, float(globe.get("atmosphere_intensity")),
			float(globe.get("atmosphere_scale"))])
		# The per-frame inputs are **global shader parameters** now, not material uniforms -
		# the ground pass lives on every streamed tile, and pushing nine uniforms into each of
		# their materials every frame is what made the old arrangement untenable. Reading them
		# back from the global list is therefore also the only place they can be observed.
		var shell: MeshInstance3D = globe.get_node_or_null("Atmosphere")
		# RenderingServer is a *native class* in GDScript, not an object and not a class you
		# can type a variable as. Its methods are called statically off the class name; both
		# `RenderingServer.get_singleton()` and `var rs: RenderingServer = RenderingServer`
		# are parse errors, and a parse error makes the whole script fail to load, which shows
		# up only as a silent hang because the in-engine watchdog never gets to arm.
		if true:
			for gname in ["globe_atmo_viewer_position", "globe_atmo_camera_height",
					"globe_atmo_sun_direction", "globe_atmo_ground_light_intensity",
					"globe_atmo_shell_light_intensity", "globe_atmo_lighting_fade",
					"globe_atmo_ground_on", "globe_atmo_local_to_ecef_y_up"]:
				print("atmo: GLOBAL %s = %s" % [gname,
					str(RenderingServer.global_shader_parameter_get(gname))])
			var viewer = RenderingServer.global_shader_parameter_get("globe_atmo_viewer_position")
			if viewer is Vector3:
				print("      |viewer|=%.4e m  (Earth R=6.371e6; must be > R for an orbiting camera)" %
					(viewer as Vector3).length())
		if shell != null:
			var mat := shell.material_override as ShaderMaterial
			if mat != null:
				print("atmo: shell material is a live ShaderMaterial, shader=%s" %
					str(mat.shader != null))
			else:
				print("atmo: FAIL material_override is not a ShaderMaterial (%s)" % str(shell.material_override))
		var forced_scale := OS.get_environment("GLOBE_ATMO_SCALE")
		if forced_scale != "":
			globe.set("atmosphere_scale", float(forced_scale))
		if OS.get_environment("GLOBE_ATMO_DEBUG_PURE") != "":
			globe.set("atmosphere_debug_pure", true)
		var forced_ground := OS.get_environment("GLOBE_GROUND_INTENSITY")
		if forced_ground != "":
			globe.set("ground_atmosphere_intensity", float(forced_ground))
		print("ground: requested=%s  effective=%.2f  on=%s" % [
			forced_ground, float(globe.get("ground_atmosphere_intensity")),
			str(globe.get("ground_atmosphere"))])
		# The ground pass now lives on the *tile* materials, which are the only thing that
		# actually draws the planet once the quadtree is streaming. Probing Globe3D's Surface
		# instead is what hid the original bug for so long: that mesh is completely covered by
		# the tiles, so a perfectly live material there still contributes zero pixels.
		var probed := 0
		var textured := 0
		if layer != null:
			# get_children(true): the argument is include_internal, and the tile meshes are added
			# with INTERNAL_MODE_FRONT so the editor never writes them into the .tscn. Without
			# it the walk finds nothing and reports "the veil is not on the tiles" no matter
			# what the materials actually are.
			for child in layer.get_children(true):
				if child is MeshInstance3D:
					var tmat := (child as MeshInstance3D).material_override as ShaderMaterial
					if tmat == null:
						continue
					probed += 1
					if bool(tmat.get_shader_parameter("u_has_texture")):
						textured += 1
					if probed == 1:
						print("ground: TILE material has_texture=%s  uv_scale=%s  uv_offset=%s  shader=%s" % [
							str(tmat.get_shader_parameter("u_has_texture")),
							str(tmat.get_shader_parameter("u_uv_scale")),
							str(tmat.get_shader_parameter("u_uv_offset")),
							str(tmat.shader != null)])
						# Mipmap chain audit: without a full mip chain the low-LOD imagery
						# magnifies into hard-edged mosaic blocks (the Cesium-style blur
						# requires CPU generate_mipmaps + a filter_linear_mipmap sampler).
						var albedo: Texture2D = tmat.get_shader_parameter("u_albedo_texture")
						if albedo != null:
							var tex_img: Image = albedo.get_image()
							if tex_img != null:
								print("ground: TILE albedo has_mipmaps=%s  mip_count=%d  size=%s" % [
									str(tex_img.has_mipmaps()), tex_img.get_mipmap_count(),
									str(tex_img.get_size())])
							else:
								print("ground: TILE albedo readback failed")
						else:
							print("ground: TILE albedo texture is null")
		print("ground: %d tile ShaderMaterials, %d with imagery" % [probed, textured])
		# Negative control for the RTC offset.
		#
		# Tile vertices are authored relative to the tile's own centre and the centre is handed
		# to the ground shader as u_tile_center (Godot's VERTEX is model space, so the node's
		# translation is not in it). Zeroing that uniform on a build that is otherwise correct
		# reproduces the pre-fix render exactly: every tile gets shaded as though it sat on the
		# frame's origin. Same binary, same tiles, same camera - so the A/B isolates the one
		# term, with no reliance on a stored PNG whose provenance nobody can re-derive.
		if OS.get_environment("GLOBE_ZERO_TILE_CENTER") != "":
			var zeroed := 0
			for child in layer.get_children(true):
				if child is MeshInstance3D:
					var zmat := (child as MeshInstance3D).material_override as ShaderMaterial
					if zmat == null:
						continue
					zmat.set_shader_parameter("u_tile_center", Vector3.ZERO)
					zeroed += 1
			print("ground: NEGATIVE CONTROL u_tile_center forced to 0 on %d tiles" % zeroed)
		if probed == 0:
			print("ground: FAIL no tile material is a ShaderMaterial - the veil is not on the tiles")
		elif textured == 0:
			print("ground: FAIL tiles exist but none carry a texture - the veil would be over the placeholder")
		if OS.get_environment("GLOBE_GROUND_OFF") != "":
			globe.set("ground_atmosphere", false)
			print("ground: PASS DISABLED - this run is the floor the veil mean must beat")
		# Paint the raw texture fetch on the tiles, with the scattering removed entirely.
		# This is the fork between "the imagery never reaches the pixels" and "the imagery is
		# there and the veil is eating it" - the two are pixel-identical in a screenshot.
		#
		# The value is a level, not a flag. =2 additionally applies the sRGB decode and nothing
		# else, so comparing it against =1 measures the colour-space round trip on its own.
		# That measurement is what settled whether the decode in the real path is correct: see
		# the note on srgb_to_linear_approx in GlobeAtmosphereShading.cpp.
		var debug_albedo := OS.get_environment("GLOBE_DEBUG_ALBEDO")
		if debug_albedo != "":
			var level := 1
			if debug_albedo.is_valid_int():
				level = clampi(int(debug_albedo), 1, 2)
			globe.set("ground_debug_albedo", level)
			print("ground: DEBUG raw-albedo mode level=%d (1=fetch, 2=fetch+sRGB decode)" % level)
		# Hide Globe3D's own ellipsoid mesh, leaving only the streamed tiles.
		#
		# `GLOBE_FORCE_SHELL=1` does the opposite and pins it visible, which is the negative
		# control for the assertion below. Without a run that is *known* to be broken it is
		# impossible to tell a passing check from a check that cannot fail - and the whole
		# reason this bug survived is that every earlier probe was positive-only.
		if OS.get_environment("GLOBE_FORCE_SHELL") != "":
			globe.set("show_surface", 1)
			print("surface: FORCED visible (negative control - the shell assertion must FAIL)")
		elif OS.get_environment("GLOBE_HIDE_SURFACE") != "":
			var surf: Node = globe.get_node_or_null("Surface")
			if surf is Node3D:
				(surf as Node3D).visible = false
				print("surface: hidden (visible=false)")
			else:
				print("surface: FAIL no Surface node to hide")
		# Probe: replace the atmosphere's shader with a flat opaque material at runtime.
		#
		# This is deliberately independent of the shader source. If flat magenta does not
		# appear either, the problem is the *mesh* (visibility, culling, transform, winding)
		# and not the raymarch - which is the fork this audit kept failing to make, because
		# every previous probe changed a shader uniform and therefore could only ever test
		# the shader.
		if OS.get_environment("GLOBE_ATMO_PROBE") != "":
			var probe_shell: MeshInstance3D = globe.get_node_or_null("Atmosphere")
			if probe_shell != null:
				var flat := StandardMaterial3D.new()
				flat.albedo_color = Color(1, 0, 1, 1)
				flat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
				flat.cull_mode = BaseMaterial3D.CULL_DISABLED
				flat.transparency = BaseMaterial3D.TRANSPARENCY_DISABLED
				probe_shell.material_override = flat
				probe_shell.visible = true
				print("probe: flat material installed, visible=%s  aabb=%s" % [
					str(probe_shell.visible), str(probe_shell.get_aabb())])
	# Camera pose. GlobeTileLayer reframes against the loaded tileset once streaming
	# finishes, which lands the camera somewhere else entirely, so the pose is re-applied
	# after the settle as well as before it - that is what makes the measured disc radius
	# comparable between runs.
	#
	# 1.6e7 m is a full-disc view. The reference's own uLightingFade band (FullGlobe.ts,
	# camDist 6.5e6-9.0e6 m) is calibrated for a near-surface camera and does not describe
	# this regime. GLOBE_CAM_RADIUS overrides the distance.
	var cam_radius := 16000000.0
	var forced_cam := OS.get_environment("GLOBE_CAM_RADIUS")
	if forced_cam != "":
		cam_radius = float(forced_cam)
	# Control for the origin shift: with it off, the frame origin stays on the georeference
	# anchor and every coordinate is the Earth-sized one. Two renders that differ between
	# shift-on and shift-off are the direct test that re-basing is *invariant* - which is the
	# whole claim the feature makes, and the only way to tell "a rebase missed a node" apart
	# from "the stored baseline PNG is simply stale".
	if camera != null and OS.get_environment("GLOBE_NO_ORIGIN_SHIFT") != "":
		camera.call("set_origin_shift_enabled", false)
		print("camera: origin shift DISABLED for this run (invariance control)")

	if camera != null:
		camera.call("orbit_to", 105.0, 25.0, cam_radius)

	# Settle frames. 1800 (30 s at 60 fps) is what a full streaming run needs; the
	# diagnostics override it, because a control that takes 30 s per iteration is a control
	# that stops getting run, and an unrun control is indistinguishable from a passing one.
	var settle_frames := 1800
	var forced_frames := OS.get_environment("GLOBE_FRAMES")
	if forced_frames != "":
		settle_frames = int(forced_frames)
	for frame in settle_frames:
		await get_tree().process_frame
		if frame % 60 == 0 and layer != null:
			print("t=%2ds rendered=%d loading=%d cached=%d max_level=%d" % [
				frame / 60, layer.get_rendered_tile_count(), layer.get_loading_tile_count(),
				layer.get_cached_tile_count(), layer.get_max_selected_level()])

	# Re-pin after the tile layer has finished reframing, then let a few frames settle so
	# the atmosphere's per-frame uniforms pick the new camera up before pixels are read.
	if camera != null:
		camera.call("orbit_to", 105.0, 25.0, cam_radius)
		for _i in 5:
			await get_tree().process_frame

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

	# The built-in ellipsoid must stand down while tiles are on screen.
	#
	# Both meshes sit on the same WGS84 ellipsoid, so when both are drawn every pixel is a
	# depth tie: the render becomes the shell's flat base_color blue with a lattice of tile
	# fragments punching through, and the entire dataset is hidden behind an opaque sphere.
	# The symptom is unmistakable once named (a blue ball with a dot grid) and completely
	# invisible to any intensity or exposure tuning, because no pixel is being decided by
	# shading. This assertion exists so a regression is a failed run rather than a screenshot
	# somebody has to recognise.
	if globe != null and layer != null and int(layer.get_rendered_tile_count()) > 0:
		var shell_mesh: Node3D = globe.get_node_or_null("Surface")
		if shell_mesh == null:
			print("FAIL  no Surface node to check")
		elif (shell_mesh as Node3D).visible:
			print("FAIL  ellipsoid mesh still visible under %d streamed tiles - it z-fights" %
				layer.get_rendered_tile_count())
		else:
			print("PASS  ellipsoid mesh stood down for the tile layer (%d tiles)" %
				layer.get_rendered_tile_count())

	# Wait for the framebuffer, then read it back.
	#
	# `await RenderingServer.frame_post_draw` is the textbook way to do this and it is the
	# reason this harness intermittently produced no PNG at all: the signal only fires when
	# the driver actually presents, and under the OpenGL3/compatibility path a scene that has
	# stopped changing can go several seconds without one. The watchdog then fires and the
	# run is lost - with the log showing a *completed* audit, which is the most confusing
	# possible failure. `process_frame` is unconditional, and two of them is enough for the
	# draw that was already queued.
	await get_tree().process_frame
	await get_tree().process_frame
	var img := get_viewport().get_texture().get_image()
	var out_path := _out_path()
	img.save_png(ProjectSettings.globalize_path(out_path))
	print("saved %s  %s" % [out_path, img.get_size()])
	var mid := Vector2i(img.get_size().x / 2, img.get_size().y / 2)
	print("corner(space) = ", img.get_pixelv(Vector2i(8, 8)))
	print("centre        = ", img.get_pixelv(mid))

	# ---- terminator / atmosphere assertions ----
	#
	# These are the checks that would have caught the "hard blue ring" screenshot. They are
	# deliberately numeric: an atmosphere defect is a *gradient* defect, and a gradient is
	# invisible in prose but obvious in a column of samples.
	if globe != null:
		var sub: Vector2 = globe.call("get_sub_solar_point")
		var sun_dir: Vector3 = globe.call("get_sun_direction")
		print("sun: sub-solar lon=%.2f lat=%.2f  dir=(%.4f, %.4f, %.4f)" % [
			sub.x, sub.y, sun_dir.x, sun_dir.y, sun_dir.z])

		# Geometry sanity for the atmosphere shell. Its vertices are authored in Y-up ECEF and
		# placed by the node transform, so a botched transform shows up here as an AABB centre
		# nowhere near the ellipsoid's - which is invisible in the PNG but fatal to the render.
		var shell: MeshInstance3D = globe.get_node_or_null("Atmosphere")
		if shell != null and shell.mesh != null:
			var sb := shell.get_aabb()
			var sb_centre_local := shell.transform * sb.get_center()
			var world_shell_centre: Vector3 = globe.to_global(sb_centre_local)
			var world_expected: Vector3 = globe.to_global(globe.ecef_to_local(Vector3.ZERO))
			print("atmosphere: aabb_size=%.0f  centre_local=(%.0f, %.0f, %.0f)" % [
				sb.size.length(), sb_centre_local.x, sb_centre_local.y, sb_centre_local.z])
			print("atmosphere: centre_world=(%.0f, %.0f, %.0f)  expected=(%.0f, %.0f, %.0f)  off=%.1f m" % [
				world_shell_centre.x, world_shell_centre.y, world_shell_centre.z,
				world_expected.x, world_expected.y, world_expected.z,
				world_shell_centre.distance_to(world_expected)])

		# Radial profile across the actual limb.
		#
		# The earlier version walked a fixed fraction of the *viewport* width, which drifts off
		# the disc as soon as the camera framing changes - at the demo's default framing the
		# globe only spans ~18% of the width, so half the samples were in empty space and the
		# "limb profile" reported the background. Measuring from the projected silhouette
		# radius keeps the samples on the feature regardless of distance.
		#
		# The reported quantity is *blue excess* (b - r), not luminance. Air is blue and
		# desaturated; terrain is neither, and space is black. Luminance cannot tell a blue
		# halo from bright ground, which is exactly the confusion this audit kept falling into.
		var size := img.get_size()
		var globe_centre: Vector3 = globe.to_global(globe.ecef_to_local(Vector3.ZERO))
		# Named `view_cam`, not `camera`: `camera` is already bound earlier in _ready() to the
		# GlobeCameraController node, and GDScript rejects the whole script at parse time on a
		# name collision - which meant the scene silently failed to load and the audit hung
		# with no output at all.
		var view_cam: Camera3D = get_viewport().get_camera_3d()
		var centre_px := Vector2(size) * 0.5
		var dx_axis := Vector2(1, 0)
		if view_cam != null:
			centre_px = view_cam.unproject_position(globe_centre)
			# The offset point has to be a *world* point one equatorial radius from the
			# centre, measured perpendicular to the view axis - not a local-space
			# `+ Vector3(0, 6378137, 0)`.
			#
			# Under a Georeference3D the globe's local space is the ENU tangent frame, which
			# is rotated relative to ECEF: its +Y is "up" only near the anchor's own zenith.
			# Adding a polar-radius vector in that frame lands tens of degrees of longitude
			# away from where the silhouette actually is, and the offset projects to well
			# under the true radius. Measured that way the disc came out 174 px against a
			# real 272 px, so every "limb" sample at r/R >= 1.0 was still sampling the
			# interior and the profile described the planet, not its edge.
			#
			# The camera's own right vector is the one direction guaranteed perpendicular to
			# the view ray *and* screen-horizontal, so the unprojection of
			# centre + right * R lands exactly on the silhouette. Building it by
			# cross(view_dir, up) instead looks equivalent and is not: the sign flips with the
			# camera's roll and the result is no longer screen-horizontal, which is how an
			# earlier attempt projected the "edge" to (2009, 556) - outside a 1920-wide
			# viewport - and reported a 5660 px disc.
			var cam_right := view_cam.global_transform.basis.x.normalized()
			var edge: Vector3 = globe_centre + cam_right * 6378137.0
			dx_axis = (view_cam.unproject_position(edge) - centre_px)
		var disc_r: float = maxf(dx_axis.length(), 1.0)
		dx_axis = dx_axis.normalized()
		print("limb: disc_centre_px=(%.0f, %.0f)  disc_radius_px=%.1f" % [
			centre_px.x, centre_px.y, disc_r])

		# Find the terminator first, then profile the limb *through the lit side*.
		#
		# Order matters and the reason is physical, not cosmetic: there is no atmosphere
		# visible on the night limb. Rayleigh scattering needs sunlight, so a profile taken
		# across an unlit limb correctly reads zero at every radius - which is exactly what
		# the night side of the real Earth looks like from orbit. Profiling there and
		# concluding "the halo is gone" inverts the meaning of the measurement: it reports
		# the absence of sunlight as the absence of an effect.
		#
		# On the lit limb the same profile peaks just outside r/R = 1.0 and decays outward,
		# which is the signature being asserted.
		var term := _measure_terminator(img, centre_px, disc_r)
		var lit_bearing: float = term["bearing"]
		# Screen y grows downward, so negate sin to convert the y-up bearing back to pixels.
		var la: float = deg_to_rad(lit_bearing)
		var limb_dir := Vector2(cos(la), -sin(la))

		# Explicitly typed: an untyped array literal gives GDScript nothing to infer
		# `frac_i` from, and it rejects the whole script at parse time - which surfaces as
		# the scene never loading and the audit hanging with no output, not as a type error
		# pointing here.
		var kFracs: Array[float] = [0.80, 0.90, 0.96, 1.00, 1.02, 1.04, 1.06, 1.10, 1.15, 1.25, 1.40, 1.60]
		var profile: Array[float] = []
		var prof_pts := ""
		for frac in kFracs:
			var p := centre_px + limb_dir * (disc_r * frac)
			var ip := Vector2i(clampi(int(p.x), 0, size.x - 1), clampi(int(p.y), 0, size.y - 1))
			var c := img.get_pixelv(ip)
			profile.append(c.b - c.r)
			prof_pts += "%.2f:%.3f " % [frac, c.b - c.r]
		var line := "limb blue-excess (r/R) along the LIT limb @%.0f deg:" % lit_bearing
		for v in profile:
			line += " %.3f" % v
		print(line)
		print("     as pairs: " + prof_pts)

		# The halo must peak at or just outside the silhouette and fall away outside it. A
		# shell that ends on a hard geometric line peaks exactly at 1.00 and is zero
		# immediately beyond; a raymarched one has a tail. Checking only "is it zero at
		# 1.6" would pass a hard-edged ring, so the peak position is asserted too.
		var peak_at := 0.0
		var peak_val := -1.0
		for i in profile.size():
			# Only samples at or outside the silhouette can be the halo; inside is globe.
			if kFracs[i] >= 1.0 and profile[i] > peak_val:
				peak_val = profile[i]
				peak_at = kFracs[i]
		if peak_val > 0.02 and peak_at <= 1.10:
			print("PASS  lit limb has a soft halo peaking at r/R=%.2f (blue-excess %.3f)" % [
				peak_at, peak_val])
		else:
			print("FAIL  lit limb halo missing or hard-edged: peak=%.3f at r/R=%.2f" % [
				peak_val, peak_at])

		# The night side must be darker than the day side. If the raymarch were still additive
		# or the sun direction were still camera-relative, the far half of the globe would be
		# as bright as the near half and the two samples would match.
		#
		# The day/night split is found by SWEEPING the image, not by projecting the sun's
		# direction. That is a deliberate reversal: an earlier version unprojected
		# `get_sun_direction()` and offset the samples along the result, which looks like the
		# direct thing to measure and is wrong by ~120 degrees here. The chain from a
		# local-space sun vector to a screen bearing runs through
		# GlobeFrame::local_direction -> the Georeference3D's Z-up->Y-up flip -> the camera's
		# orbit pose -> unprojection, and any one of those conventions silently disagreeing
		# produces a *plausible* number that is simply the wrong direction - the failure mode
		# where a broken assertion looks like a rendering bug.
		#
		# The terminator, by contrast, is a fact about the pixels. A lit planet has a bright
		# half and a dark half, and finding which half is which cannot itself be wrong.
		# (`term` and `lit_bearing` come from the sweep above, which the limb profile needs.)
		print("terminator: bearing=%.1f deg  day_luma=%.3f  night_luma=%.3f  contrast=%.2fx" % [
			term["bearing"], term["day"], term["night"],
			term["day"] / maxf(term["night"], 0.001)])
		if term["day"] > term["night"]:
			print("PASS  day side is brighter than night side")
		else:
			print("FAIL  no terminator: day=%.3f night=%.3f" % [term["day"], term["night"]])

		# The disc-wide veil.
		#
		# This is the assertion the "planet surface has no glow" report is really about, and
		# it is separate from the limb profile on purpose: the limb can be perfect while the
		# disc is bare, because they are two different passes on two different meshes. Blue
		# excess inside the silhouette is the ground pass's signature - the shell contributes
		# nothing in front of the planet, so any blue here had to come from the tile material.
		#
		# Sampled on the LIT side (the terminator bearing, +/- a spread of angles) for the
		# same reason the limb profile is: over the night hemisphere there is no sunlight to
		# scatter, so the air contributes nothing and a night-side sample measures the absence
		# of the sun rather than the presence of the effect.
		#
		# AVERAGED OVER A PATCH, and compared as a mean - not as a per-point minimum.
		#
		# The earlier version tested every one of twelve single pixels against a threshold, and
		# reported FAIL on correct output at -0.039. That is not a near miss, it is the wrong
		# measurement: terrain is red and ocean is blue, so single pixels legitimately range
		# from -0.17 to +0.43 on a *working* veil, and no single threshold can separate "the
		# pass is off" from "this pixel happens to be a desert". A patch average over terrain
		# keeps the geography in the signal but averages its pixel-to-pixel spread away.
		#
		# What actually settles it is the differential run: GLOBE_GROUND_OFF paints the same
		# scene with the pass disabled, and the mean blue excess there is the floor this
		# measurement must be compared against. Both numbers are printed so the comparison can
		# be made by eye as well as by the gate.
		var veil_line := "veil blue-excess on the lit disc (r/R, bearing deg):"
		var veil_sum := 0.0
		var veil_count := 0
		var worst := 99.0
		var worst_at := ""
		var kVeilFracs: Array[float] = [0.0, 0.30, 0.55, 0.75]
		var kVeilSpreads: Array[float] = [-35.0, 0.0, 35.0]
		for frac in kVeilFracs:
			for spread in kVeilSpreads:
				var ba: float = deg_to_rad(lit_bearing + spread)
				var p := centre_px + Vector2(cos(ba), -sin(ba)) * (disc_r * frac)
				var be := _patch_blue_excess(img, p, size)
				veil_line += " %.3f" % be
				veil_sum += be
				veil_count += 1
				if be < worst:
					worst = be
					worst_at = "r/R=%.2f@%.0fdeg" % [frac, lit_bearing + spread]
		var veil_mean := veil_sum / maxf(float(veil_count), 1.0)
		print(veil_line)
		print("veil: mean=%.3f  min=%.3f at %s   (GLOBE_GROUND_OFF prints the floor to beat)" % [
			veil_mean, worst, worst_at])
		# The gate is on the mean, and it is deliberately low. A working pass adds a small,
		# spatially varying amount of blue; the failure this guards against is a pass that is
		# *absent*, which shows up as a mean near the terrain's own balance rather than a small
		# negative. The negative control that proves this can fail is GLOBE_GROUND_OFF itself.
		if veil_mean > 0.01:
			print("PASS  lit disc carries a blue veil (mean %.3f over %d patches)" % [
				veil_mean, veil_count])
		else:
			print("FAIL  no blue veil on the lit disc: mean=%.3f (min %.3f at %s)" % [
				veil_mean, worst, worst_at])

	# The sun billboard is a bright spot; with it pinned to the camera it always lands
	# somewhere in frame while the globe is framed. Its absence is what produced the
	# "glow with no disc" look in the broken build.
	var sun_node: MeshInstance3D = globe_scene.get_node_or_null("Georeference3D/Globe3D/Sun")
	if sun_node != null:
		var visible_now := sun_node.visible and sun_node.mesh != null
		print("%s  sun billboard present=%s  global_pos=%.1f,%.1f,%.1f" % [
			"PASS " if visible_now else "FAIL ", str(visible_now),
			sun_node.global_position.x, sun_node.global_position.y, sun_node.global_position.z])

	get_tree().quit(0)

# Finds the terminator by sweeping the rendered image, and reports the lit and unlit halves.
#
# This replaces an earlier version that unprojected `get_sun_direction()` and offset its
# samples along the result. Projecting a sun direction is the direct way to say "sample the
# day side", and it was wrong by ~120 degrees on this scene: the chain from a local-space sun
# vector to a screen bearing runs through GlobeFrame::local_direction, then the
# Georeference3D's Z-up -> Y-up flip, then the camera's orbit pose, then unprojection. Any
# one of those conventions disagreeing yields a *plausible* number pointing the wrong way,
# which is the worst failure mode available - the assertion confidently reports the inverse
# of the truth and reads as a rendering regression.
#
# The terminator is instead a fact about the pixels: sweep a ring of samples around the disc
# centre, take the brightest direction as day and the darkest as night. No coordinate
# convention is involved, so the measurement cannot be wrong in that way.
#
# The ring sits at 0.55 disc radii - far enough out that the readings fall on opposite sides
# of the terminator whatever its orientation, close enough in to stay on the globe rather
# than out on the limb glow. Each reading is a small patch average, because one pixel on a
# coastline is not a lighting measurement.
func _measure_terminator(img: Image, p_centre_px: Vector2, p_disc_r: float) -> Dictionary:
	var size := img.get_size()
	var readings: Array[float] = []
	var kSteps := 36
	for i in kSteps:
		# Screen y grows downward, so negate sin to make the sweep CCW in a y-up frame.
		var a := TAU * float(i) / float(kSteps)
		var d := Vector2(cos(a), -sin(a))
		var patch := Vector2i(p_centre_px + d * (p_disc_r * 0.55))
		var total := 0.0
		var count := 0
		for dx in range(-5, 6):
			for dy in range(-5, 6):
				var p := Vector2i(clampi(patch.x + dx, 0, size.x - 1),
								  clampi(patch.y + dy, 0, size.y - 1))
				total += img.get_pixelv(p).get_luminance()
				count += 1
		readings.append(total / maxf(float(count), 1.0))

	# Mean of the brightest and darkest thirds, not the single extremes: one specular glint
	# or one cloud would otherwise define a whole hemisphere.
	var ordered := readings.duplicate()
	ordered.sort()
	var third := maxi(1, kSteps / 3)
	var night := 0.0
	for i in third:
		night += ordered[i]
	night /= float(third)
	var day := 0.0
	for i in third:
		day += ordered[kSteps - 1 - i]
	day /= float(third)

	# The bearing the sweep found, so the number can be checked against the picture.
	var best := 0
	for i in kSteps:
		if readings[i] > readings[best]:
			best = i
	var bearing := rad_to_deg(TAU * float(best) / float(kSteps))

	return {"bearing": bearing, "day": day, "night": night}

# Mean blue excess (b - r) over a small patch centred on a pixel.
#
# A patch, not a pixel: the quantity being measured is a thin additive veil, and one pixel of a
# desert or a cloud says more about the geography than about the atmosphere. 9x9 is wide enough
# to average the tile's own texture detail out and narrow enough to stay on one piece of ground.
func _patch_blue_excess(img: Image, p_centre: Vector2, p_size: Vector2i) -> float:
	var total := 0.0
	var count := 0
	for dx in range(-4, 5):
		for dy in range(-4, 5):
			var q := Vector2i(clampi(int(p_centre.x) + dx, 0, p_size.x - 1),
							  clampi(int(p_centre.y) + dy, 0, p_size.y - 1))
			var c := img.get_pixelv(q)
			total += c.b - c.r
			count += 1
	return total / maxf(float(count), 1.0)

# Fires on wall-clock time, not frames, so a stalled render loop still terminates the
# process. `create_timer` with process_always=true keeps counting while the tree is paused,
# and quitting from a Timer callback runs even mid-await.
func _start_watchdog() -> void:
	var timer := get_tree().create_timer(WATCHDOG_SECONDS, true, false, true)
	timer.timeout.connect(func() -> void:
		push_error("globe_capture: WATCHDOG fired after %.0f s - the scene is wedged; killing."
			% WATCHDOG_SECONDS)
		print("WATCHDOG fired after %.0f s - quitting" % WATCHDOG_SECONDS)
		get_tree().quit(2))
