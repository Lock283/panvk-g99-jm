#version 450
/* uv computed from the pixel centre, so the sampled coordinate is known
 * exactly on the CPU; no varying interpolation involved. */
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { vec2 scale; vec2 off; float lod; } pc;
layout(location = 0) out vec4 o;
void main() {
    vec2 uv = gl_FragCoord.xy * pc.scale + pc.off;
    o = textureLod(tex, uv, pc.lod);
}
