#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vA;
layout(location = 1) out vec4 vB;
void main(){ vA=vec4(inColor.r,inColor.g,0,0); vB=vec4(0,0,inColor.b,0); gl_Position=vec4(inCell+inPos,0,1); }
