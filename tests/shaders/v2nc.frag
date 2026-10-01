#version 450
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 0) out vec4 o;
void main(){ o = vec4(vA.r, vA.g, vB.r, 1.0); }
