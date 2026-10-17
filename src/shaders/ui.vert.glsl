// ui.vert.glsl - vertex shader of the 2D draw-list pipeline.
// Vertices are in framebuffer pixels (origin top-left); the push constants map
// them to normalized device coordinates: ndc = pos * scale + translate,
// with scale = 2 / framebuffer size and translate = -1 (Vulkan's y points down).
#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;   // R8G8B8A8_UNORM, straight alpha

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 translate;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = vec4(in_pos * pc.scale + pc.translate, 0.0, 1.0);
}
