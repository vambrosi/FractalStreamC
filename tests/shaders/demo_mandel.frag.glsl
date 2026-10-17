// demo_mandel.frag.glsl - Mandelbrot viewer for the windowed demo (tests/gfx_demo.c).
// Pixel -> plane mapping as in agents/architecture.md:
//   z = center + scale * ((p.x - w/2), -(p.y - h/2)),  scale = radius / (min(w,h)/2)
#version 450

layout(push_constant) uniform PushConstants {
    vec2 size;
    vec2 offset;
} pc;

layout(std430, set = 0, binding = 0) readonly buffer Globals {
    vec2 center;
    float radius;
    float time;
} g;

layout(location = 0) out vec4 color;

void main() {
    vec2 p = gl_FragCoord.xy + pc.offset;
    float scale = g.radius / (min(pc.size.x, pc.size.y) * 0.5);
    vec2 c = g.center + scale * vec2(p.x - 0.5 * pc.size.x, -(p.y - 0.5 * pc.size.y));
    vec2 z = vec2(0.0);
    int n = 0;
    for (; n < 200; n++) {
        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        if (dot(z, z) > 16.0) break;
    }
    if (n == 200) {
        color = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        float t = float(n) / 200.0 + 0.1 * g.time;
        color = vec4(0.5 + 0.5 * cos(6.2831 * (t + vec3(0.0, 0.33, 0.67))), 1.0);
    }
}
