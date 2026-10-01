#version 450
/* T4.7 - one slanted white triangle, no axis-aligned edges, so many pixels are
 * only partly covered. Vertices kept in sync with TRI[] in msaa_test.c. */
void main() {
    vec2 p[3] = vec2[](vec2(-0.8, -0.7), vec2(0.75, -0.3), vec2(-0.2, 0.85));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
