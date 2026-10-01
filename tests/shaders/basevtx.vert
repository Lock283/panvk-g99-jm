#version 450
#extension GL_ARB_shader_draw_parameters : require
/* One bar whose position comes from gl_BaseVertexARB, so the image depends on
 * the first_vertex sysval and nothing else. The corner within the triangle uses
 * gl_VertexIndex - gl_BaseVertexARB, which is 0..2 whatever firstVertex is.
 * A draw with firstVertex = k therefore puts its bar at slot k only if the
 * sysval carries k. */
void main() {
    int v = (gl_VertexIndex - gl_BaseVertexARB) % 3;
    float x0 = -0.9 + float(gl_BaseVertexARB) * 0.125;
    float x1 = x0 + 0.0625;
    vec2 p[3] = vec2[]( vec2(x0, -0.5), vec2(x1, -0.5), vec2(x0, 0.5) );
    gl_Position = vec4(p[v], 0.0, 1.0);
}
