#version 450
layout(location = 0) in vec4 vA;
layout(location = 1) flat in int vOne;
layout(location = 0) out vec4 o;
void main(){ o = vec4(vA.rgb,1.0)*float(vOne); }
