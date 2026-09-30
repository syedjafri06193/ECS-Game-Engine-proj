#pragma once
// Clustered forward light assignment (design doc §6.4, §13.3), CPU reference.
//
// The view frustum is cut into X x Y screen tiles and Z depth slices
// ("froxels"). Slices are exponential in depth - fine near the camera, coarse
// far away - which is what makes clustering work. The GPU compute shaders in
// shaders/ implement the same functions; this version is the ground truth
// the tests (and a golden-image comparison) are checked against.
#include <cstdint>
#include <span>
#include <vector>
#include "core/math/math.h"

namespace engine::cluster {

struct Grid {
    uint32_t x = 16, y = 9, z = 24;
    float    z_near = 0.1f, z_far = 200.0f;   // positive view distances
    float    fov_y = 1.0f;                     // radians
    float    aspect = 16.0f / 9.0f;
    uint32_t Count() const { return x * y * z; }
};

struct Aabb { Vec3 min, max; };
struct LightSphere { Vec3 view_pos; float radius; };   // view space (camera looks down -Z)

// Exponential slicing: slice = log(d/near) / log(far/near) * Z.
uint32_t Slice(const Grid& g, float view_distance);
float    SliceNear(const Grid& g, uint32_t slice);     // distance where slice k begins
// Linear slicing, for comparison only.
uint32_t SliceLinear(const Grid& g, float view_distance);

// Cluster of a screen position (uv in [0,1], origin top-left) at a view distance.
uint32_t ClusterIndex(const Grid& g, float u, float v, float view_distance);
void     ClusterCoords(const Grid& g, uint32_t index, uint32_t& cx, uint32_t& cy, uint32_t& cz);

// View-space bounding box of one froxel.
Aabb ClusterBounds(const Grid& g, uint32_t cx, uint32_t cy, uint32_t cz);

// Project a view-space point to (u, v, distance). Returns false behind the camera.
bool Project(const Grid& g, Vec3 view_pos, float& u, float& v, float& distance);

bool SphereIntersectsAabb(const LightSphere& s, const Aabb& b);

struct Assignment {
    std::vector<uint32_t> offsets;   // per cluster: first index into `indices`
    std::vector<uint32_t> counts;    // per cluster
    std::vector<uint32_t> indices;   // light indices
    uint32_t max_per_cluster = 0;
    uint32_t overflowed = 0;         // clusters that hit the cap
};

// Brute-force-per-cluster assignment with a per-cluster cap (the GPU uses a
// fixed-size light-index list; §6.4 sizes it at 256).
Assignment Assign(const Grid& g, std::span<const LightSphere> lights, uint32_t max_lights_per_cluster = 256);

// Bytes of the light index buffer at the cap: 3456 clusters x 256 x 4 B = 3.5 MB.
inline uint64_t IndexBufferBytes(const Grid& g, uint32_t cap = 256) { return uint64_t(g.Count()) * cap * 4; }

}  // namespace engine::cluster
