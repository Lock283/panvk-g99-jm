#version 450
layout(location = 0) in vec4 vA; layout(location = 1) in vec4 vB;
layout(location = 2) in vec4 vC; layout(location = 3) in vec4 vD;
layout(location = 0) out vec4 o;
void main(){ o = vec4(vA.r, vB.g, vC.b + vD.w, 1.0); }
