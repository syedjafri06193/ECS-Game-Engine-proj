#include "render/cluster/cluster.h"
#include <algorithm>
#include <cmath>

namespace engine::cluster {

uint32_t Slice(const Grid& g, float d)
{
    if (d <= g.z_near) return 0;
    const float s = std::log(d / g.z_near) / std::log(g.z_far / g.z_near) * float(g.z);
    return std::min(uint32_t(s), g.z - 1);
}

float SliceNear(const Grid& g, uint32_t k)
{
    return g.z_near * std::pow(g.z_far / g.z_near, float(k) / float(g.z));
}

uint32_t SliceLinear(const Grid& g, float d)
{
    const float s = (d - g.z_near) / (g.z_far - g.z_near) * float(g.z);
    return std::min(uint32_t(std::max(s, 0.0f)), g.z - 1);
}

uint32_t ClusterIndex(const Grid& g, float u, float v, float d)
{
    const uint32_t tx = std::min(uint32_t(Clamp(u, 0, 1) * float(g.x)), g.x - 1);
    const uint32_t ty = std::min(uint32_t(Clamp(v, 0, 1) * float(g.y)), g.y - 1);
    return (Slice(g, d) * g.y + ty) * g.x + tx;
}

void ClusterCoords(const Grid& g, uint32_t i, uint32_t& cx, uint32_t& cy, uint32_t& cz)
{
    cx = i % g.x;
    cy = (i / g.x) % g.y;
    cz = i / (g.x * g.y);
}

// Symmetric perspective: at view distance d, NDC x maps to view x = ndc * d * tan_x.
// Screen v grows downward, view y grows upward.
Aabb ClusterBounds(const Grid& g, uint32_t cx, uint32_t cy, uint32_t cz)
{
    const float tan_y = std::tan(g.fov_y * 0.5f), tan_x = tan_y * g.aspect;
    const float nx0 = float(cx) / float(g.x) * 2 - 1, nx1 = float(cx + 1) / float(g.x) * 2 - 1;
    const float ny0 = 1 - float(cy) / float(g.y) * 2, ny1 = 1 - float(cy + 1) / float(g.y) * 2;
    const float dn = SliceNear(g, cz), df = cz + 1 >= g.z ? g.z_far : SliceNear(g, cz + 1);
    Aabb b{{1e30f, 1e30f, -df}, {-1e30f, -1e30f, -dn}};
    for (float d : {dn, df})
        for (float nx : {nx0, nx1})
            for (float ny : {ny0, ny1}) {
                const float x = nx * d * tan_x, y = ny * d * tan_y;
                b.min.x = std::min(b.min.x, x); b.max.x = std::max(b.max.x, x);
                b.min.y = std::min(b.min.y, y); b.max.y = std::max(b.max.y, y);
            }
    return b;
}

bool Project(const Grid& g, Vec3 p, float& u, float& v, float& d)
{
    d = -p.z;
    if (d <= 0) return false;
    const float tan_y = std::tan(g.fov_y * 0.5f), tan_x = tan_y * g.aspect;
    u = (p.x / (d * tan_x) + 1) * 0.5f;
    v = (1 - p.y / (d * tan_y)) * 0.5f;
    return true;
}

bool SphereIntersectsAabb(const LightSphere& s, const Aabb& b)
{
    float d2 = 0;
    const float c[3] = {s.view_pos.x, s.view_pos.y, s.view_pos.z};
    const float lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        const float e = c[i] < lo[i] ? lo[i] - c[i] : (c[i] > hi[i] ? c[i] - hi[i] : 0.0f);
        d2 += e * e;
    }
    return d2 <= s.radius * s.radius;
}

Assignment Assign(const Grid& g, std::span<const LightSphere> lights, uint32_t cap)
{
    Assignment a;
    const uint32_t n = g.Count();
    a.offsets.resize(n);
    a.counts.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t cx, cy, cz;
        ClusterCoords(g, i, cx, cy, cz);
        const Aabb box = ClusterBounds(g, cx, cy, cz);
        a.offsets[i] = uint32_t(a.indices.size());
        uint32_t count = 0;
        for (uint32_t l = 0; l < lights.size(); ++l) {
            if (!SphereIntersectsAabb(lights[l], box)) continue;
            if (count == cap) { ++a.overflowed; break; }
            a.indices.push_back(l);
            ++count;
        }
        a.counts[i] = count;
        a.max_per_cluster = std::max(a.max_per_cluster, count);
    }
    return a;
}

}  // namespace engine::cluster
