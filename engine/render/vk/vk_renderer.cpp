#include "render/vk/vk_renderer.h"
#include <cmath>
#include <cstring>
#include "render/cluster/cluster.h"

namespace engine::vk {

using namespace engine::rg;

namespace {

// GPU layouts: must match shaders/common.glsl (scalar block layout).
struct GpuVertex { float pos[3]; float normal[3]; float uv[2]; };
struct GpuInstance { float model[16]; uint32_t material, pad[3]; };
struct GpuLight { float pos[3]; float radius; float color[3]; float intensity; };
struct GpuMaterial { float tint[4]; uint32_t albedo; float roughness; uint32_t pad[2]; };
struct GpuFrame {
    float view[16], proj[16], view_proj[16];
    float camera_pos[4];
    uint32_t grid[4];
    float grid_params[4];
    float screen[4];
    float ambient[4];
};
struct Push {
    VkDeviceAddress frame, instances, vertices, lights, clusters, materials;
    uint32_t hdr_texture, pad;
};
static_assert(sizeof(GpuVertex) == 32 && sizeof(GpuInstance) == 80 && sizeof(GpuLight) == 32);
static_assert(sizeof(GpuMaterial) == 32 && sizeof(GpuFrame) == 272 && sizeof(Push) == 56);

constexpr uint32_t kMaxLightsPerCluster = 256;

void AddQuad(std::vector<GpuVertex>& v, std::vector<uint32_t>& idx, Vec3 c, Vec3 u, Vec3 w, Vec3 n, float uv_scale)
{
    const uint32_t base = uint32_t(v.size());
    const Vec3 p[4] = {c - u - w, c + u - w, c + u + w, c - u + w};
    const float uv[4][2] = {{0, 0}, {uv_scale, 0}, {uv_scale, uv_scale}, {0, uv_scale}};
    for (int i = 0; i < 4; ++i) v.push_back({{p[i].x, p[i].y, p[i].z}, {n.x, n.y, n.z}, {uv[i][0], uv[i][1]}});
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + i);
}

std::vector<uint32_t> Checker(uint32_t size, uint32_t a, uint32_t b, uint32_t cells)
{
    std::vector<uint32_t> px(size * size);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) px[y * size + x] = (((x * cells / size) + (y * cells / size)) & 1) ? a : b;
    return px;
}

}  // namespace

bool Renderer::Init(Context& ctx, const std::string& shader_dir)
{
    ctx_ = &ctx;
    shader_dir_ = shader_dir;
    backend_ = std::make_unique<GraphBackend>(ctx);

    // One layout for everything: the bindless set + push constants (§6.2).
    VkPushConstantRange pr{VK_SHADER_STAGE_ALL, 0, sizeof(Push)};
    VkDescriptorSetLayout set = ctx.BindlessLayout();
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &set;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(ctx.Device(), &li, nullptr, &layout_));

    // Pipelines are created up front, never mid-frame (§6.5).
    VkShaderModule depth_vs = ctx.LoadShader(shader_dir + "/depth.vert.spv");
    VkShaderModule fwd_vs = ctx.LoadShader(shader_dir + "/forward.vert.spv");
    VkShaderModule fwd_fs = ctx.LoadShader(shader_dir + "/forward.frag.spv");
    VkShaderModule fs_vs = ctx.LoadShader(shader_dir + "/fullscreen.vert.spv");
    VkShaderModule tm_fs = ctx.LoadShader(shader_dir + "/tonemap.frag.spv");
    VkShaderModule cull_cs = ctx.LoadShader(shader_dir + "/cluster_cull.comp.spv");
    depth_ = MakeGraphics(depth_vs, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_COMPARE_OP_LESS, true, false);
    forward_ = MakeGraphics(fwd_vs, fwd_fs, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_COMPARE_OP_EQUAL, false, false);
    transparent_ = MakeGraphics(fwd_vs, fwd_fs, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_COMPARE_OP_LESS, false, true);
    tonemap_format_ = ctx.SwapchainFormat();
    tonemap_ = MakeGraphics(fs_vs, tm_fs, tonemap_format_, VK_FORMAT_UNDEFINED, VK_COMPARE_OP_ALWAYS, false, false);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cull_cs, "main", nullptr};
    ci.layout = layout_;
    VK_CHECK(vkCreateComputePipelines(ctx.Device(), ctx.PipelineCache(), 1, &ci, nullptr, &cull_));
    for (VkShaderModule m : {depth_vs, fwd_vs, fwd_fs, fs_vs, tm_fs, cull_cs}) vkDestroyShaderModule(ctx.Device(), m, nullptr);

    // Meshes: one vertex buffer (pulled by device address) + one index buffer.
    std::vector<GpuVertex> v;
    std::vector<uint32_t> idx;
    const uint32_t cube_first = 0;
    AddQuad(v, idx, {0, 0, 0.5f}, {0.5f, 0, 0}, {0, 0.5f, 0}, {0, 0, 1}, 1);
    AddQuad(v, idx, {0, 0, -0.5f}, {-0.5f, 0, 0}, {0, 0.5f, 0}, {0, 0, -1}, 1);
    AddQuad(v, idx, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 0.5f, 0}, {1, 0, 0}, 1);
    AddQuad(v, idx, {-0.5f, 0, 0}, {0, 0, 0.5f}, {0, 0.5f, 0}, {-1, 0, 0}, 1);
    AddQuad(v, idx, {0, 0.5f, 0}, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 1, 0}, 1);
    AddQuad(v, idx, {0, -0.5f, 0}, {0.5f, 0, 0}, {0, 0, 0.5f}, {0, -1, 0}, 1);
    const uint32_t cube_count = uint32_t(idx.size());
    const uint32_t plane_first = uint32_t(idx.size());
    AddQuad(v, idx, {0, 0, 0}, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 1, 0}, 16);
    meshes_ = {{cube_first, cube_count, 0}, {plane_first, uint32_t(idx.size()) - plane_first, 0}};
    vertices_ = ctx.CreateBuffer(v.size() * sizeof(GpuVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    ctx.UploadBuffer(vertices_, v.data(), vertices_.size);
    indices_ = ctx.CreateBuffer(idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    ctx.UploadBuffer(indices_, idx.data(), indices_.size);

    // Materials: procedural textures registered in the bindless array.
    const uint32_t colors[kMaterialCount][2] = {
        {0xffd0d0d0u, 0xff909090u}, {0xff3050e0u, 0xff2030a0u}, {0xff40c060u, 0xff208040u}, {0xffe0a030u, 0xff905010u}};
    std::vector<GpuMaterial> mats;
    for (uint32_t m = 0; m < kMaterialCount; ++m) {
        auto px = Checker(64, colors[m][0], colors[m][1], 8);
        Image img = ctx.UploadTexture(64, 64, px.data());
        textures_.push_back(img);
        GpuMaterial gm{};
        gm.tint[0] = gm.tint[1] = gm.tint[2] = 1.0f;
        gm.tint[3] = m == kMaterialCount - 1 ? 0.45f : 1.0f;     // last material: transparent glass-ish
        gm.albedo = ctx.RegisterTexture(img.view);
        gm.roughness = 0.35f + 0.15f * float(m);
        mats.push_back(gm);
    }
    materials_ = ctx.CreateBuffer(mats.size() * sizeof(GpuMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    ctx.UploadBuffer(materials_, mats.data(), materials_.size);

    cluster::Grid g;
    for (auto& c : clusters_)
        c = ctx.CreateBuffer(VkDeviceSize(g.Count()) * (kMaxLightsPerCluster + 1) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    return true;
}

VkPipeline Renderer::MakeGraphics(VkShaderModule vs, VkShaderModule fs, VkFormat color, VkFormat depth,
                                  VkCompareOp depth_op, bool depth_write, bool blend)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   // vertex pulling
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;         // TODO(M5): enable back-face culling once winding is verified on hardware
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth != VK_FORMAT_UNDEFINED;
    ds.depthWriteEnable = depth_write;
    ds.depthCompareOp = depth_op;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    if (blend) {
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
    cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    // Dynamic rendering (§6.1): no render pass or framebuffer objects.
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = fs ? 2 : 1;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout_;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->Device(), ctx_->PipelineCache(), 1, &gi, nullptr, &p));
    return p;
}

void Renderer::DrawRuns(VkCommandBuffer cmd, const std::vector<DrawItem>& items, uint32_t first_instance)
{
    // Items are sorted by material then mesh: consecutive runs of one mesh are
    // one instanced draw; gl_InstanceIndex indexes the instance buffer.
    size_t i = 0;
    while (i < items.size()) {
        size_t j = i + 1;
        while (j < items.size() && items[j].mesh == items[i].mesh) ++j;
        const MeshRange& m = meshes_[items[i].mesh < meshes_.size() ? items[i].mesh : 0];
        vkCmdDrawIndexed(cmd, m.index_count, uint32_t(j - i), m.first_index, m.vertex_offset, first_instance + uint32_t(i));
        ++stats_.draw_calls;
        i = j;
    }
}

void Renderer::Render(const RenderPacket& packet, const std::function<void(VkCommandBuffer)>& ui)
{
    ENGINE_ZONE("Renderer::Render");
    Context& ctx = *ctx_;
    FrameResources& fr = ctx.Frame();
    VkCommandBuffer cmd = fr.cmd;
    const VkExtent2D ext = ctx.Extent();
    stats_ = RendererStats{};

    // ---- upload this frame's data into the per-frame arena
    Push push{};
    const size_t n_inst = packet.opaque.size() + packet.transparent.size();
    auto* inst = static_cast<GpuInstance*>(fr.arena.Allocate(std::max<size_t>(n_inst, 1) * sizeof(GpuInstance), &push.instances));
    auto* lights = static_cast<GpuLight*>(fr.arena.Allocate(std::max<size_t>(packet.lights.size(), 1) * sizeof(GpuLight), &push.lights));
    auto* frame = static_cast<GpuFrame*>(fr.arena.Allocate(sizeof(GpuFrame), &push.frame));
    if (!inst || !lights || !frame) {
        std::fprintf(stderr, "frame arena exhausted (%zu instances, %zu lights)\n", n_inst, packet.lights.size());
        return;
    }
    size_t k = 0;
    for (const auto* list : {&packet.opaque, &packet.transparent})
        for (const DrawItem& d : *list) {
            std::memcpy(inst[k].model, d.model.m, sizeof d.model.m);
            inst[k].material = d.material % kMaterialCount;
            ++k;
        }
    for (size_t l = 0; l < packet.lights.size(); ++l) {
        const LightItem& L = packet.lights[l];
        lights[l] = {{L.position.x, L.position.y, L.position.z}, L.radius, {L.color.x, L.color.y, L.color.z}, L.intensity};
    }
    const CameraData& cam = packet.camera;
    const Mat4 vp = cam.projection * cam.view;
    std::memcpy(frame->view, cam.view.m, 64);
    std::memcpy(frame->proj, cam.projection.m, 64);
    std::memcpy(frame->view_proj, vp.m, 64);
    frame->camera_pos[0] = cam.position.x; frame->camera_pos[1] = cam.position.y; frame->camera_pos[2] = cam.position.z;
    const cluster::Grid g;
    frame->grid[0] = g.x; frame->grid[1] = g.y; frame->grid[2] = g.z; frame->grid[3] = uint32_t(packet.lights.size());
    frame->grid_params[0] = cam.z_near; frame->grid_params[1] = cam.z_far;
    frame->grid_params[2] = std::tan(cam.fov_y * 0.5f); frame->grid_params[3] = cam.aspect;
    frame->screen[0] = float(ext.width); frame->screen[1] = float(ext.height);
    frame->screen[2] = 1.0f / float(ext.width); frame->screen[3] = 1.0f / float(ext.height);
    frame->ambient[0] = frame->ambient[1] = frame->ambient[2] = 0.03f;
    push.vertices = vertices_.address;
    push.materials = materials_.address;
    Buffer& clusters = clusters_[ctx.FrameIndex()];
    push.clusters = clusters.address;
    stats_.instances = uint32_t(n_inst);
    stats_.lights = uint32_t(packet.lights.size());

    // ---- declare the frame (compile is cached while its shape is unchanged)
    graph_.Reset();
    const TextureDesc screen{ext.width, ext.height, Format::RGBA16F};
    const ResourceId depth = graph_.CreateTexture("depth", {ext.width, ext.height, Format::D32F});
    const ResourceId hdr = graph_.CreateTexture("hdr", screen);
    const ResourceId cl = graph_.ImportBuffer("cluster-lights", {clusters.size}, State{}, State{});
    const ResourceId swap = graph_.ImportTexture("swapchain", {ext.width, ext.height, Format::BGRA8_SRGB},
                                                 State{StageNone, AccessNone, Layout::Undefined},
                                                 State{StageBottomOfPipe, AccessNone, Layout::Present});
    const VkViewport viewport{0, 0, float(ext.width), float(ext.height), 0, 1};
    const VkRect2D scissor{{0, 0}, ext};
    GraphBackend* be = backend_.get();
    const uint32_t n_opaque = uint32_t(packet.opaque.size());
    const RenderPacket* pk = &packet;       // capture by pointer: [=] on a reference would COPY the packet

    auto begin_rendering = [&, be](VkCommandBuffer c, ResourceId color, VkAttachmentLoadOp color_load, ResourceId depth_res,
                                   VkAttachmentLoadOp depth_load, bool depth_read_only) {
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        if (color != kInvalidResource) {
            ca.imageView = be->Resource(color).view;
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = color_load;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca.clearValue.color = {{0.01f, 0.012f, 0.02f, 1.0f}};
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
        }
        if (depth_res != kInvalidResource) {
            da.imageView = be->Resource(depth_res).view;
            da.imageLayout = depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = depth_load;
            da.storeOp = depth_read_only ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil = {1.0f, 0};
            ri.pDepthAttachment = &da;
        }
        vkCmdBeginRendering(c, &ri);
        vkCmdSetViewport(c, 0, 1, &viewport);
        vkCmdSetScissor(c, 0, 1, &scissor);
    };

    graph_.AddPass("depth-prepass", QueueType::Graphics, [&](PassBuilder& b) {
        b.WriteDepth(depth);
        return [=, this](PassContext& pc) {
            auto c = static_cast<VkCommandBuffer>(pc.native_command_buffer);
            begin_rendering(c, kInvalidResource, VK_ATTACHMENT_LOAD_OP_DONT_CARE, depth, VK_ATTACHMENT_LOAD_OP_CLEAR, false);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_);
            vkCmdPushConstants(c, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);
            vkCmdBindIndexBuffer(c, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
            DrawRuns(c, pk->opaque, 0);                     // transparent geometry writes no depth
            vkCmdEndRendering(c);
        };
    });
    graph_.AddPass("light-culling", QueueType::Compute, [&](PassBuilder& b) {
        b.WriteBuffer(cl, ShaderCompute);
        return [=, this](PassContext& pc) {
            auto c = static_cast<VkCommandBuffer>(pc.native_command_buffer);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cull_);
            vkCmdPushConstants(c, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);
            vkCmdDispatch(c, (g.Count() + 63) / 64, 1, 1);
        };
    });
    graph_.AddPass("forward", QueueType::Graphics, [&](PassBuilder& b) {
        b.ReadBuffer(cl, ShaderFragment);
        b.ReadDepth(depth);
        b.WriteColor(hdr);
        return [=, this](PassContext& pc) {
            auto c = static_cast<VkCommandBuffer>(pc.native_command_buffer);
            begin_rendering(c, hdr, VK_ATTACHMENT_LOAD_OP_CLEAR, depth, VK_ATTACHMENT_LOAD_OP_LOAD, true);
            vkCmdPushConstants(c, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);
            vkCmdBindIndexBuffer(c, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, forward_);
            DrawRuns(c, pk->opaque, 0);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, transparent_);   // back-to-front
            DrawRuns(c, pk->transparent, n_opaque);
            vkCmdEndRendering(c);
        };
    });
    graph_.AddPass("tonemap", QueueType::Graphics, [&](PassBuilder& b) {
        b.ReadTexture(hdr, ShaderFragment);
        b.WriteColor(swap);
        return [=, this](PassContext& pc) {
            auto c = static_cast<VkCommandBuffer>(pc.native_command_buffer);
            Push p = push;
            p.hdr_texture = be->Resource(hdr).bindless;
            begin_rendering(c, swap, VK_ATTACHMENT_LOAD_OP_DONT_CARE, kInvalidResource, VK_ATTACHMENT_LOAD_OP_DONT_CARE, false);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_);
            vkCmdPushConstants(c, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &p);
            vkCmdDraw(c, 3, 1, 0, 0);
            vkCmdEndRendering(c);
        };
    });
    graph_.AddPass("ui", QueueType::Graphics, [&](PassBuilder& b) {
        b.WriteColor(swap, /*load_existing=*/true);
        return [=](PassContext& pc) {
            auto c = static_cast<VkCommandBuffer>(pc.native_command_buffer);
            begin_rendering(c, swap, VK_ATTACHMENT_LOAD_OP_LOAD, kInvalidResource, VK_ATTACHMENT_LOAD_OP_DONT_CARE, false);
            if (ui) ui(c);
            vkCmdEndRendering(c);
        };
    });

    const CompiledGraph& compiled = graph_.Compile();
    if (!compiled.Ok()) {
        for (const auto& e : compiled.errors) std::fprintf(stderr, "render graph: %s\n", e.c_str());
        return;
    }
    backend_->SetImportedImage(swap, ctx.SwapchainImage(), ctx.SwapchainView(), VK_IMAGE_ASPECT_COLOR_BIT);
    backend_->SetImportedBuffer(cl, clusters.buffer, clusters.address);
    backend_->Prepare(graph_, cmd);

    // Bindless set: bound once per frame for both bind points.
    VkDescriptorSet set = ctx.BindlessSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);

    PassContext pc;
    pc.native_command_buffer = cmd;
    graph_.Execute(*backend_, pc);

    stats_.passes = uint32_t(compiled.passes.size());
    stats_.culled_passes = uint32_t(compiled.culled.size());
    stats_.barriers = uint32_t(compiled.BarrierCount());
    stats_.graph_compiles = graph_.CompileCount();
}

void Renderer::Shutdown()
{
    if (!ctx_) return;
    Context& ctx = *ctx_;
    ctx.WaitIdle();
    backend_.reset();
    for (auto& c : clusters_) ctx.DestroyBuffer(c);
    ctx.DestroyBuffer(vertices_);
    ctx.DestroyBuffer(indices_);
    ctx.DestroyBuffer(materials_);
    for (auto& t : textures_) ctx.DestroyImage(t);
    for (VkPipeline p : {depth_, forward_, transparent_, tonemap_, cull_}) vkDestroyPipeline(ctx.Device(), p, nullptr);
    vkDestroyPipelineLayout(ctx.Device(), layout_, nullptr);
    ctx_ = nullptr;
}

}  // namespace engine::vk
