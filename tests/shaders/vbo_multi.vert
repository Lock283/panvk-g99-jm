#version 450
/* Same inputs as vbo.vert, but the colour is spread over four varying slots plus
 * a flat integer, so the varying layout has several entries with different
 * interpolation. The fragment shader reassembles exactly the same colour, so the
 * CPU reference does not change. The decoy components are large and must cancel
 * exactly; a slot read from the wrong location would not cancel. */
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vA;
layout(location = 1) out vec4 vB;
layout(location = 2) out vec4 vC;
layout(location = 3) out vec4 vD;
layout(location = 4) flat out int vOne;
void main() {
    vA = vec4(inColor.r, 7.0, -3.0, 2.0);
    vB = vec4(5.0, inColor.g, 11.0, -1.0);
    vC = vec4(-9.0, 4.0, inColor.b, 6.0);
    vD = vec4(7.0, 5.0, 3.0, 0.0);
    vOne = 1;
    gl_Position = vec4(inCell + inPos, 0.0, 1.0);
}
