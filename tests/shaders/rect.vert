#version 450
/* Axis-aligned rectangle in pixel coordinates on a 64x64 target, from push
 * constants: rect = (x0, y0, x1, y1), z = depth. Two triangles, no buffers. */
layout(push_constant) uniform PC { vec4 rect; vec4 color; float z; } pc;
void main() {
    const vec2 c[6] = vec2[](vec2(0,0), vec2(1,0), vec2(1,1), vec2(0,0), vec2(1,1), vec2(0,1));
    vec2 p = mix(pc.rect.xy, pc.rect.zw, c[gl_VertexIndex]);
    gl_Position = vec4(p / 32.0 - 1.0, pc.z, 1.0);
}
