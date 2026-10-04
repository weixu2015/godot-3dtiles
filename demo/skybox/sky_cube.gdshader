// Sky that samples the six cube faces directly - the three.js CubeTextureLoader path, minus
// any stitching. EYEDIR is the world-space direction the camera looks at a sky pixel, Y-up,
// the same convention an OpenGL cubemap (and therefore the source face images) uses.
shader_type sky;

uniform samplerCube sky_cubemap : source_color, filter_linear;

void sky() {
	COLOR = texture(sky_cubemap, EYEDIR).rgb;
}
