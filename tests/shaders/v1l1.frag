#version 450
layout(location = 1) in vec4 vA;
layout(location = 0) out vec4 o;
void main(){ o = vec4(vA.rgb,1.0); }
