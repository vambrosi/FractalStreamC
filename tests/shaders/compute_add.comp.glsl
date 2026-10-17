// compute_add.comp.glsl - two storage buffers (tests/test_gfx.c):
//   dst[i] = src[i] + add   for i < n      (src at binding 1, dst at binding 2)
#version 450
layout(local_size_x = 64) in;

layout(push_constant) uniform Push { uint add; uint n; } pc;
layout(std430, set = 0, binding = 1) readonly buffer Src { uint v[]; } src;
layout(std430, set = 0, binding = 2) writeonly buffer Dst { uint v[]; } dst;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.n) dst.v[i] = src.v[i] + pc.add;
}
