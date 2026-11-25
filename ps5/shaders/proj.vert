#version 450
layout(location = 0) in vec3 in_pos;
layout(set = 0, binding = 1) uniform Params {
    vec4 output_size, view_position, view_right, view_up, view_forward;
    vec4 world_bounds, cue, cue_color, vehicle_position;
    vec4 body0, body1, body2, source0, direction0, source1, direction1;
    vec4 cell_xz[18];
    vec4 cell_y[9];
    ivec4 mode;
} u;
void main() { gl_Position = vec4(in_pos.xy * 2.0 / u.output_size.xy - 1.0, 0.0, 1.0); }
