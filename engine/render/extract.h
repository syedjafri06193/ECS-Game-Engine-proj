#pragma once
// Extraction (design doc §8.1): simulation -> flat, POD render packet.
//
// The renderer never walks the World. Extraction runs as parallel jobs over
// chunks, writes into a RenderPacket, and the packet is double-buffered so the
// renderer consumes frame N while simulation produces N+1. It also means hot
// reload only has to quiesce simulation jobs, never the renderer.
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>
#include "core/math/math.h"
#include "ecs/world.h"
#include "jobs/scheduler.h"

namespace engine {

struct DrawItem {
    Mat4     model;
    uint32_t mesh = 0;
    uint32_t material = 0;
    float    view_depth = 0;          // for sorting
    uint32_t entity_index = 0;        // debugging / picking
};

struct LightItem {
    Vec3  position;                   // world space
    float radius = 1;
    Vec3  color{1, 1, 1};
    float intensity = 1;
};

struct CameraData {
    Mat4  view, projection;
    Vec3  position;
    float fov_y = 1, z_near = 0.1f, z_far = 200, aspect = 1;
    bool  valid = false;
};

struct RenderPacket {
    uint64_t frame = 0;
    CameraData camera;
    std::vector<DrawItem> opaque;       // sorted: material, mesh, then front-to-back
    std::vector<DrawItem> transparent;  // sorted back-to-front
    std::vector<LightItem> lights;
    void Clear() { opaque.clear(); transparent.clear(); lights.clear(); camera = {}; }
};

// Fills `out`. Parallel over chunks; output order is independent of thread
// timing (per-chunk results are concatenated in chunk order, then sorted).
void Extract(World& world, Scheduler& scheduler, float aspect, RenderPacket& out);

// Two packets: the simulation writes one while the renderer reads the other.
class PacketQueue {
public:
    RenderPacket& BeginWrite();           // the slot not being read
    void          Publish();              // make it the latest
    // Renderer: latest published packet, or null if none newer than last time.
    const RenderPacket* AcquireLatest();
    void          Release();

private:
    std::mutex m_;
    RenderPacket slots_[2];
    int  writing_ = 0;
    int  published_ = -1;
    int  reading_ = -1;
    uint64_t last_read_frame_ = 0;
    uint64_t frame_counter_ = 0;
};

}  // namespace engine
