#version 450
// Screen-space projections over the finished scene (vehicle shadows, headlights): each pixel's world
// position is rebuilt from a snapshot of the depth buffer.
layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform Params {
    vec4 output_size, view_position, view_right, view_up, view_forward;
    vec4 world_bounds, cue, cue_color, vehicle_position;
    vec4 body0, body1, body2, source0, direction0, source1, direction1;
    vec4 cell_xz[18];
    vec4 cell_y[9];
    ivec4 mode;
} u;
layout(location = 0) out vec4 out_color;
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
bool triangle(vec2 a, vec2 b, vec2 c, vec2 p) {
    float e0 = cross2(b - a, p - a), e1 = cross2(c - b, p - b), e2 = cross2(a - c, p - c);
    return (e0 >= -0.0001 && e1 >= -0.0001 && e2 >= -0.0001) || (e0 <= 0.0001 && e1 <= 0.0001 && e2 <= 0.0001);
}
void main() {
    vec2 pixel = gl_FragCoord.xy;
    float hd = texture(scene_depth, pixel / u.output_size.xy).r;
    if (hd >= 0.9999999) discard;
    float d = u.output_size.w / max(0.0000001, 1.0 - hd);
    vec2 xy = vec2((pixel.x - u.output_size.z) * d / u.cue.w, (u.view_position.w - pixel.y) * d / u.cue.w);
    vec3 world = u.view_position.xyz + u.view_right.xyz * xy.x + u.view_up.xyz * xy.y + u.view_forward.xyz * d;
    float normalized_cue = u.cue.z > 0.5 ? clamp((d - u.cue.y) / max(0.0001, u.cue.x - u.cue.y), 0.0, 1.0) : 0.0;
    float clear_fraction = 1.0 - normalized_cue;
    float fade = 1.0 - clear_fraction * clear_fraction * clear_fraction;
    if (u.mode.x == 0) {
        if (world.x < u.world_bounds.x || world.x > u.world_bounds.y ||
            world.z < u.world_bounds.z || world.z > u.world_bounds.w) discard;
        bool accepted = false;
        for (int c = 0; c < 9; c++) {
            if (u.cell_y[c].z < 0.5 || world.y < u.cell_y[c].x || world.y > u.cell_y[c].y) continue;
            vec4 a = u.cell_xz[c * 2], b = u.cell_xz[c * 2 + 1];
            if (triangle(a.xy, a.zw, b.xy, world.xz) || triangle(a.xy, b.xy, b.zw, world.xz)) accepted = true;
        }
        if (!accepted) discard;
        out_color = vec4(mix(vec3(10.0 / 255.0), u.cue_color.rgb, fade), 1.0);
    } else {
        vec3 delta = world - u.vehicle_position.xyz;
        vec3 local = vec3(dot(delta, u.body0.xyz), dot(delta, u.body1.xyz), dot(delta, u.body2.xyz));
        float dz = (u.direction0.z + u.direction1.z) * 0.5;
        if (abs(dz) < 0.0000001) discard;
        float distance = (local.z - (u.source0.z + u.source1.z) * 0.5) / dz;
        if (distance < 0.35 || distance > 20.0) discard;
        float left = u.source0.x + u.direction0.x * distance, right = u.source1.x + u.direction1.x * distance;
        if (abs(right - left) < 0.0000001) discard;
        float lateral = ((local.x - left) / (right - left) + 0.5) * 0.5;
        if (lateral < 0.0 || lateral > 1.0) discard;
        float edge = clamp(1.0 - abs(lateral * 2.0 - 1.0), 0.0, 1.0);
        float remaining = clamp(1.0 - distance / 20.0, 0.0, 1.0);
        float intensity = clamp((distance - 0.35) / 1.65, 0.0, 1.0) * remaining * remaining * edge * edge * (3.0 - 2.0 * edge) * 0.46 * (1.0 - fade);
        if (intensity <= 0.0) discard;
        out_color = vec4(vec3(150.0 / 255.0) * intensity, 1.0);
    }
}
