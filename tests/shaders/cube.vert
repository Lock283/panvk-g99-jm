#version 450
/* Spinning cube: model-view-projection from a push constant (16 floats,
 * 8 FAU words), position and per-face colour from one interleaved buffer. */
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_col;
layout(location = 0) out vec3 v_col;
void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_col = in_col;
}
