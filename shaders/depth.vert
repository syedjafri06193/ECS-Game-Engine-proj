#version 460
// Depth prepass: position only.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

invariant gl_Position;   // depth prepass + EQUAL test: both passes must produce identical depth

void main()
{
    Vertex v = pc.vertices.v[gl_VertexIndex];
    Instance inst = pc.instances.i[gl_InstanceIndex];
    // Same expression, same order as forward.vert - invariance needs both.
    vec4 world = inst.model * vec4(v.position, 1.0);
    gl_Position = pc.frame.view_proj * world;
}
