#version 450
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 2) in vec4 vC;
layout(location = 3) in vec4 vD;
layout(location = 4) flat in int vOne;
layout(location = 0) out vec4 outColor;
void main() {
    /* (vA.y - vD.x) = 0, (vB.x - vD.y) = 0, (vC.y - 4) = 0, vOne = 1 */
    float z = (vA.y - vD.x) + (vB.x - vD.y) + (vC.y - 4.0);
    outColor = vec4(vA.x + z, vB.y + z, vC.z + z, 1.0) * float(vOne);
}
