#version 450
/* vbo.vert with the position fetched as R32G32_UINT and bit-cast to float, the
 * way vk_meta's rect shader reads its R32G32B32A32_UINT attribute. */
layout(location = 0) in uvec2 inPosBits;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = inColor;
    gl_Position = vec4(inCell + uintBitsToFloat(inPosBits), 0.0, 1.0);
}
