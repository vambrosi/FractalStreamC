// fullscreen.vert.glsl - vertex shader of every viewer pipeline.
// Draw 3 vertices: one triangle that covers the whole viewport, with corners
// (-1,-1), (3,-1), (-1,3) in normalized device coordinates. Viewer fragment
// shaders only use gl_FragCoord, so nothing else is passed on.
#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
