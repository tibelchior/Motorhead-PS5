#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(push_constant) uniform PC {
    vec4 output_size;
    vec4 fog_color;
    vec4 fog_range;
    int mode;  // 0 opaque, 1 alpha, 2 additive, 3 hidden-line fill, 4 hidden-line outline
} pc;
layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 2) in float view_depth;
layout(location = 0) out vec4 out_color;
void main() {
    if (pc.mode == 3) { out_color = vec4(0.0, 0.0, 0.5, 1.0); return; }
    if (pc.mode == 4) { out_color = vec4(0.44, 0.58, 1.0, 1.0); return; }
    vec4 t = texture(image, uv) * color;
    if (t.a < (pc.mode == 0 ? 0.5 : 0.00001)) discard;
    float f = 0.0;
    if (pc.fog_range.z > 0.5 && view_depth > pc.fog_range.x) {
        float cue = clamp((view_depth - pc.fog_range.x) / max(0.0001, pc.fog_range.y - pc.fog_range.x), 0.0, 1.0);
        f = 1.0 - (1.0 - cue) * (1.0 - cue);
    }
    t.rgb = pc.mode == 2 ? t.rgb * (1.0 - f) : mix(t.rgb, pc.fog_color.rgb, f);
    out_color = vec4(t.rgb, pc.mode == 0 ? 1.0 : t.a);
}
