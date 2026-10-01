#version 450
/* One thin vertical bar per instance, marching left to right by gl_InstanceIndex.
 * Coverage therefore scales with instanceCount, which is what makes instancing
 * observable at all: with a shader that ignores gl_InstanceIndex, N instances
 * would draw N identical overlapping triangles and the pixel count would be the
 * same for every N, so the test would pass whether or not instancing worked.
 *
 * Each bar is 2 pixels wide and separated by a gap, so bars cannot merge into one
 * another. Bar i occupies NDC x from -0.9 + i*0.125, width 0.0625.
 */
void main() {
    int v = gl_VertexIndex % 3;
    float x0 = -0.9 + float(gl_InstanceIndex) * 0.125;
    float x1 = x0 + 0.0625;
    vec2 p[3] = vec2[]( vec2(x0, -0.5),
                        vec2(x1, -0.5),
                        vec2(x0,  0.5) );
    gl_Position = vec4(p[v], 0.0, 1.0);
}
