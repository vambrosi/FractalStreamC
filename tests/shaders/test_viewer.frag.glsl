// test_viewer.frag.glsl - viewer used by tests/test_gfx.c.
// Follows the viewer shader contract (src/gfx.h): push constants
// {vec2 size; vec2 offset}, read-only storage buffer at set 0 binding 0.
//   r = (x + offset.x) / size.x   (x = gl_FragCoord.x, a pixel center)
//   g = (y + offset.y) / size.y
//   b = globals.k
//   a = globals.data[int(x)] if int(x) < globals.n, else 1
#version 450

layout(push_constant) uniform PushConstants {
    vec2 size;
    vec2 offset;
} pc;

layout(std430, set = 0, binding = 0) readonly buffer Globals {
    float k;
    uint n;
    float data[];
} g;

layout(location = 0) out vec4 color;

void main() {
    vec2 p = gl_FragCoord.xy + pc.offset;
    uint i = uint(gl_FragCoord.x);
    float a = i < g.n ? g.data[i] : 1.0;
    color = vec4(p.x / pc.size.x, p.y / pc.size.y, g.k, a);
}
