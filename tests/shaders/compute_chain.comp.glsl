// compute_chain.comp.glsl - ring-style dependency test (tests/test_gfx.c).
// Dispatch k computes row k of a w-wide table from row k-1, which the previous
// dispatch wrote (different invocations and workgroups read each element):
//   v[k*w + i] = v[(k-1)*w + i] * 1664525 + v[(k-1)*w + (i+1) % w] + 1013904223
// The table is bound twice, read-only at binding 1 and write-only at binding 2,
// so that the validation layer's synchronization checks see both kinds of access.
#version 450
layout(local_size_x = 32) in;

layout(push_constant) uniform Push { uint k; uint w; } pc;
layout(std430, set = 0, binding = 1) readonly buffer Src { uint v[]; } src;
layout(std430, set = 0, binding = 2) writeonly buffer Dst { uint v[]; } dst;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.w) return;
    uint prev = (pc.k - 1u) * pc.w;
    dst.v[pc.k * pc.w + i] = src.v[prev + i] * 1664525u + src.v[prev + (i + 1u) % pc.w] + 1013904223u;
}
