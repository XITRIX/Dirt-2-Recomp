#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D ty;
layout(binding = 1) uniform sampler2D tcr;
layout(binding = 2) uniform sampler2D tcb;
layout(push_constant) uniform P { vec2 scale; float alpha; } pc;  // video size / buffer size; 1 = planes in .a (A8)
void main() {
    vec2 t = uv * pc.scale;
    vec4 sy = texture(ty, t), sv = texture(tcr, t), su = texture(tcb, t);
    float y = 1.164 * ((pc.alpha > 0.5 ? sy.a : sy.r) - 16.0 / 255.0);
    float v = (pc.alpha > 0.5 ? sv.a : sv.r) - 0.5, u = (pc.alpha > 0.5 ? su.a : su.r) - 0.5;
    color = vec4(y + 1.596 * v, y - 0.392 * u - 0.813 * v, y + 2.017 * u, 1.0);
}
