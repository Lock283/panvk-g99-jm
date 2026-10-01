#version 450
/* T4.5.18 - one binding holding an ARRAY of three uniform buffers. Constant
 * indices, so this tests array-element placement in the descriptor table rather
 * than dynamic indexing. Alpha is a constant so the three array elements are the
 * only descriptor-fed channels. */
layout(set = 0, binding = 0) uniform U { vec4 v; } u[3];
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(u[0].v.x, u[1].v.x, u[2].v.x, 1.0);
}
