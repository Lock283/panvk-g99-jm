#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vA;
void main(){ vA=vec4(inColor.rgb, 0.375); gl_Position=vec4(inCell+inPos,0,1); }
