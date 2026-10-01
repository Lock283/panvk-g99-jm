#version 450
/* T4.6 - vertex buffer + varying workload.
 * location 0: per-vertex position inside a cell, from binding 0
 * location 1: per-vertex colour, from binding 0
 * location 2: per-INSTANCE cell origin, from binding 1 (input rate instance)
 * The colour is passed to the fragment shader as a smooth varying, so a correct
 * image needs attribute fetch, instance-rate fetch and varying interpolation all
 * to work. No gl_VertexIndex or gl_InstanceIndex is used. */
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inCell;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = inColor;
    gl_Position = vec4(inCell + inPos, 0.0, 1.0);
}
