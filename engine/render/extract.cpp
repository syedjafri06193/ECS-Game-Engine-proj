#include "render/extract.h"
#include <algorithm>
#include "ecs/components.h"
#include "ecs/query.h"

namespace engine {

using namespace game;

namespace {
Quat OrientationAt(Archetype* a, Chunk* c, ComponentId id, uint32_t row)
{
    if (id == kInvalidComponent || !a->HasColumn(id)) return {};
    const Orientation& o = a->Array<Orientation>(c, id)[row];
    return {o.q[0], o.q[1], o.q[2], o.q[3]};
}
float ScaleAt(Archetype* a, Chunk* c, ComponentId id, uint32_t row)
{
    if (id == kInvalidComponent || !a->HasColumn(id)) return 1.0f;
    return a->Array<Scale>(c, id)[row].value;
}
}  // namespace

void Extract(World& world, Scheduler& scheduler, float aspect, RenderPacket& out)
{
    ENGINE_ZONE("Extract");
    out.Clear();
    const ComponentId orient_id = world.Registry().Find(ComponentTraits<Orientation>::kHash);
    const ComponentId scale_id = world.Registry().Find(ComponentTraits<Scale>::kHash);

    // Camera: first active one.
    for (auto [e, cam, pos] : world.Query<const Camera, const Position>()) {
        if (!cam.active) continue;
        Quat q{};
        if (auto* o = world.Get<Orientation>(e)) q = {o->q[0], o->q[1], o->q[2], o->q[3]};
        const Vec3 eye{pos.x, pos.y, pos.z};
        const Vec3 fwd = Rotate(q, {0, 0, -1}), up = Rotate(q, {0, 1, 0});
        out.camera.view = LookAt(eye, eye + fwd, up);
        out.camera.projection = Perspective(cam.fov_y, aspect, cam.z_near, cam.z_far);
        out.camera.position = eye;
        out.camera.fov_y = cam.fov_y;
        out.camera.z_near = cam.z_near;
        out.camera.z_far = cam.z_far;
        out.camera.aspect = aspect;
        out.camera.valid = true;
        break;
    }
    const Mat4 view = out.camera.view;

    // Draws and lights: one job per chunk, results kept per chunk so the
    // concatenation order never depends on which worker finished first.
    const auto draw_chunks = world.Query<const MeshRenderer, const Position>().Chunks();
    const auto light_chunks = world.Query<const PointLight, const Position>().Chunks();
    const ComponentId mesh_id = world.Id<MeshRenderer>(), pos_id = world.Id<Position>(), light_id = world.Id<PointLight>();

    std::vector<std::vector<DrawItem>> opaque(draw_chunks.size()), transparent(draw_chunks.size());
    std::vector<std::vector<LightItem>> lights(light_chunks.size());
    {
        StructureLock lock(world);
        scheduler.ParallelFor(draw_chunks.size() + light_chunks.size(), 1, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                if (i < draw_chunks.size()) {
                    const ChunkView& v = draw_chunks[i];
                    auto meshes = v.Column<const MeshRenderer>(mesh_id);
                    auto positions = v.Column<const Position>(pos_id);
                    auto ents = v.Entities();
                    for (uint32_t r = 0; r < v.Count(); ++r) {
                        const Position& p = positions[r];
                        const float s = ScaleAt(v.archetype, v.chunk, scale_id, r);
                        DrawItem d;
                        d.model = ComposeTRS({p.x, p.y, p.z}, OrientationAt(v.archetype, v.chunk, orient_id, r), {s, s, s});
                        d.mesh = meshes[r].mesh;
                        d.material = meshes[r].material;
                        d.view_depth = -TransformPoint(view, {p.x, p.y, p.z}).z;
                        d.entity_index = ents[r].index;
                        (meshes[r].transparent ? transparent[i] : opaque[i]).push_back(d);
                    }
                } else {
                    const ChunkView& v = light_chunks[i - draw_chunks.size()];
                    auto ls = v.Column<const PointLight>(light_id);
                    auto positions = v.Column<const Position>(pos_id);
                    auto& dst = lights[i - draw_chunks.size()];
                    for (uint32_t r = 0; r < v.Count(); ++r)
                        dst.push_back({{positions[r].x, positions[r].y, positions[r].z}, ls[r].radius,
                                       {ls[r].color[0], ls[r].color[1], ls[r].color[2]}, ls[r].intensity});
                }
            }
        });
    }
    for (auto& v : opaque) out.opaque.insert(out.opaque.end(), v.begin(), v.end());
    for (auto& v : transparent) out.transparent.insert(out.transparent.end(), v.begin(), v.end());
    for (auto& v : lights) out.lights.insert(out.lights.end(), v.begin(), v.end());

    // Opaque: minimise state changes, then front-to-back for early-Z.
    std::sort(out.opaque.begin(), out.opaque.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.material != b.material) return a.material < b.material;
        if (a.mesh != b.mesh) return a.mesh < b.mesh;
        if (a.view_depth != b.view_depth) return a.view_depth < b.view_depth;
        return a.entity_index < b.entity_index;
    });
    // Transparent: back-to-front for correct blending.
    std::sort(out.transparent.begin(), out.transparent.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.view_depth != b.view_depth) return a.view_depth > b.view_depth;
        return a.entity_index < b.entity_index;
    });
}

// ---------------------------------------------------------------- PacketQueue

RenderPacket& PacketQueue::BeginWrite()
{
    std::lock_guard lock(m_);
    // Prefer the slot that is neither being read nor holding the latest
    // unread packet; if the renderer lags, overwrite the unread one (mailbox).
    int slot = 0;
    if (reading_ == 0) slot = 1;
    else if (reading_ == 1) slot = 0;
    else slot = (published_ == 0) ? 1 : 0;
    if (slot == published_) published_ = -1;
    writing_ = slot;
    return slots_[slot];
}

void PacketQueue::Publish()
{
    std::lock_guard lock(m_);
    slots_[writing_].frame = ++frame_counter_;
    published_ = writing_;
}

const RenderPacket* PacketQueue::AcquireLatest()
{
    std::lock_guard lock(m_);
    if (published_ < 0 || slots_[published_].frame == last_read_frame_) return nullptr;
    reading_ = published_;
    last_read_frame_ = slots_[reading_].frame;
    return &slots_[reading_];
}

void PacketQueue::Release()
{
    std::lock_guard lock(m_);
    reading_ = -1;
}

}  // namespace engine
