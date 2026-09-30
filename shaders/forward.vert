#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

invariant gl_Position;   // depth prepass + EQUAL test: both passes must produce identical depth

layout(location = 0) out vec3 out_world;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) flat out uint out_material;

void main()
{
    Vertex v = pc.vertices.v[gl_VertexIndex];
    Instance inst = pc.instances.i[gl_InstanceIndex];
    vec4 world = inst.model * vec4(v.position, 1.0);
    out_world = world.xyz;
    out_normal = normalize(mat3(inst.model) * v.normal);   // uniform scale only
    out_uv = v.uv;
    out_material = inst.material;
    gl_Position = pc.frame.view_proj * world;
}
