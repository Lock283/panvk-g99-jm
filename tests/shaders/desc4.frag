#version 450
/* T4.5.18 - four bindings in ONE set, one colour channel each, alpha included.
 * Whether a binding is static or dynamic is decided by the set layout, not the
 * shader, so the same binary serves both the static and dynamic cases. Every
 * binding reads .x of a vec4 whose four lanes hold the same value, so a channel
 * names exactly one binding. */
layout(set = 0, binding = 0) uniform UA { vec4 v; } ua;
layout(set = 0, binding = 1) readonly buffer SB { vec4 v; } sb;
layout(set = 0, binding = 2) uniform UC { vec4 v; } uc;
layout(set = 0, binding = 3) readonly buffer SD { vec4 v; } sd;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(ua.v.x, sb.v.x, uc.v.x, sd.v.x);
}
