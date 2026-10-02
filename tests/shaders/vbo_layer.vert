#version 450
#extension GL_ARB_shader_viewport_layer_array : require
/* vbo.vert plus a gl_Layer write, as vk_meta's rect VS does. */
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = inColor;
    gl_Position = vec4(inCell + inPos, 0.0, 1.0);
    gl_Layer = 0;
}
