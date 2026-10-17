// compute_color.comp.glsl - writes the test viewer's picture as packed RGBA8
// pixels (tests/test_gfx.c), reading the globals at binding 0 like a viewer:
//   pixel (x, y) of a w x h image, at px[base + y*w + x]:
//   r = (x + 0.5) / w,  g = (y + 0.5) / h,  b = globals.k,
//   a = globals.data[x] if x < globals.n, else 1
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform Push { uint w; uint h; uint base; } pc;
layout(std430, set = 0, binding = 0) readonly buffer Globals {
    float k;
    uint n;
    float data[];
} g;
layout(std430, set = 0, binding = 1) writeonly buffer Pixels { uint px[]; } o;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= pc.w || p.y >= pc.h) return;
    float a = p.x < g.n ? g.data[p.x] : 1.0;
    vec4 c = vec4((float(p.x) + 0.5) / float(pc.w), (float(p.y) + 0.5) / float(pc.h), g.k, a);
    o.px[pc.base + p.y * pc.w + p.x] = packUnorm4x8(c);
}
