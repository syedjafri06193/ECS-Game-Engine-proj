// Shared declarations. Bindless (design doc §6.2): every buffer is reached
// through a 64-bit device address in the push constants; every texture
// through one unbounded descriptor array, bound once per frame.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_LIGHTS_PER_CLUSTER 256u

struct Light    { vec3 position; float radius; vec3 color; float intensity; };
struct Instance { mat4 model; uint material; uint pad0, pad1, pad2; };
struct Vertex   { vec3 position; vec3 normal; vec2 uv; };
struct Material { vec4 tint; uint albedo_texture; float roughness; uint pad0, pad1; };

layout(buffer_reference, scalar) readonly buffer FrameBuf {
    mat4  view;
    mat4  proj;
    mat4  view_proj;
    vec4  camera_pos;
    uvec4 grid;          // x, y, z clusters, light count
    vec4  grid_params;   // z_near, z_far, tan(fov_y/2), aspect
    vec4  screen;        // width, height, 1/width, 1/height
    vec4  ambient;
};
layout(buffer_reference, scalar) readonly buffer LightBuf    { Light l[]; };
layout(buffer_reference, scalar) readonly buffer InstanceBuf { Instance i[]; };
layout(buffer_reference, scalar) readonly buffer VertexBuf   { Vertex v[]; };
layout(buffer_reference, scalar) readonly buffer MaterialBuf { Material m[]; };
// Per cluster: [count, index0, index1, ... index255]
layout(buffer_reference, scalar) buffer ClusterBuf { uint v[]; };

layout(push_constant, scalar) uniform Push {
    FrameBuf    frame;
    InstanceBuf instances;
    VertexBuf   vertices;
    LightBuf    lights;
    ClusterBuf  clusters;
    MaterialBuf materials;
    uint        hdr_texture;     // bindless index (tonemap)
    uint        pad;
} pc;

layout(set = 0, binding = 0) uniform sampler2D textures[];

// Identical to engine::cluster::Slice / ClusterIndex (render/cluster/cluster.cpp).
uint ClusterSlice(float d)
{
    FrameBuf f = pc.frame;
    float zn = f.grid_params.x, zf = f.grid_params.y;
    if (d <= zn) return 0u;
    float s = log(d / zn) / log(zf / zn) * float(f.grid.z);
    return min(uint(s), f.grid.z - 1u);
}

uint ClusterIndex(vec2 uv, float d)
{
    FrameBuf f = pc.frame;
    uint tx = min(uint(clamp(uv.x, 0.0, 1.0) * float(f.grid.x)), f.grid.x - 1u);
    uint ty = min(uint(clamp(uv.y, 0.0, 1.0) * float(f.grid.y)), f.grid.y - 1u);
    return (ClusterSlice(d) * f.grid.y + ty) * f.grid.x + tx;
}
