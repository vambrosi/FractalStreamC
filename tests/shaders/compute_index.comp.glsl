// compute_index.comp.glsl - compute shader used by tests/test_gfx.c.
//   out[i] = i * 3 + add   for i = gl_GlobalInvocationID.x < n
#version 450
layout(local_size_x = 64) in;

layout(push_constant) uniform Push { uint add; uint n; } pc;
layout(std430, set = 0, binding = 1) writeonly buffer Out { uint v[]; } o;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.n) o.v[i] = i * 3u + pc.add;
}
