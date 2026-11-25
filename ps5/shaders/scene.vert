#version 450
// The scene's vertices are in the game's output pixels with a view depth (post-projection
// perspective: clip w is the view depth). Same transform as the OpenGL renderer; the depth buffer
// holds 1 - near/depth, which the projection passes read back.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(push_constant) uniform PC {
    vec4 output_size;  // width, height, near plane
    vec4 fog_color;
    vec4 fog_range;    // start, end, enabled
    int mode;
} pc;
layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(location = 2) out float view_depth;
void main() {
    float d = max(in_pos.z, pc.output_size.z);
    vec2 n = in_pos.xy * 2.0 / pc.output_size.xy - 1.0;
    gl_Position = vec4(n * d, d - pc.output_size.z, d);
    color = in_color;
    uv = in_uv;
    view_depth = d;
}
