#version 460
// Clustered forward shading: look up this fragment's froxel, shade with only
// the lights assigned to it.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_world;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) flat in uint in_material;
layout(location = 0) out vec4 out_color;

const float PI = 3.14159265;

// GGX / Smith / Schlick, metallic = 0.
vec3 Brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float roughness)
{
    vec3 h = normalize(v + l);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 1e-4), nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    float a = roughness * roughness, a2 = a * a;
    float d = a2 / (PI * pow(nh * nh * (a2 - 1.0) + 1.0, 2.0));
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float g = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
    vec3 f = vec3(0.04) + (1.0 - vec3(0.04)) * pow(1.0 - vh, 5.0);
    vec3 spec = d * g * f / max(4.0 * nv * nl, 1e-4);
    return ((1.0 - f) * albedo / PI + spec) * nl;
}

void main()
{
    FrameBuf f = pc.frame;
    Material mat = pc.materials.m[in_material];
    vec3 albedo = texture(textures[nonuniformEXT(mat.albedo_texture)], in_uv).rgb * mat.tint.rgb;

    vec3 n = normalize(in_normal);
    vec3 v = normalize(f.camera_pos.xyz - in_world);
    float view_depth = -(f.view * vec4(in_world, 1.0)).z;
    uint cluster = ClusterIndex(gl_FragCoord.xy * f.screen.zw, view_depth);
    uint base = cluster * (MAX_LIGHTS_PER_CLUSTER + 1u);
    uint count = pc.clusters.v[base];

    vec3 color = albedo * f.ambient.rgb;
    for (uint i = 0u; i < count; ++i) {
        Light light = pc.lights.l[pc.clusters.v[base + 1u + i]];
        vec3 to_light = light.position - in_world;
        float dist = length(to_light);
        if (dist >= light.radius) continue;
        // Smooth windowed inverse-square falloff, reaching 0 at the radius.
        float window = pow(clamp(1.0 - pow(dist / light.radius, 4.0), 0.0, 1.0), 2.0);
        float atten = window / max(dist * dist, 0.01);
        color += Brdf(n, v, to_light / dist, albedo, mat.roughness) * light.color * light.intensity * atten;
    }
    out_color = vec4(color, mat.tint.a);      // alpha only matters for the blended transparent pipeline
}
