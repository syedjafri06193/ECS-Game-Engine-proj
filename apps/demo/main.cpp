// Demo: every pillar in one loop.
//
//   simulation   SystemPipeline over the ECS (parallel chunk jobs, sync points)
//   gameplay     native module, hot reloaded when its build output changes
//   scripting    scripts/lights.lua, reloaded on save
//   extraction   World -> RenderPacket (double-buffered)
//   rendering    render graph: depth prepass, light culling, clustered forward,
//                tonemap, UI - with per-pass GPU timings in the overlay
//
// Keys: R = force module reload, G = print the render graph as Graphviz, Esc = quit.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include "ecs/components.h"
#include "jobs/system_graph.h"
#include "modules/loader.h"
#include "render/extract.h"
#include "render/vk/vk_renderer.h"
#include "scripting/script_system.h"

using namespace engine;
using namespace game;

namespace {

void BuildScene(World& w)
{
    w.Register<Position, Velocity, Orientation, Scale, Spin, Health, Lifetime, MeshRenderer, PointLight, Camera>();
    w.Create(Camera{1.0f, 0.1f, 200.0f, true}, Position{0, 12, 30}, Orientation{});
    w.Create(Position{0, -0.5f, 0}, Scale{80}, MeshRenderer{vk::Renderer::kMeshPlane, 0, false});
    for (int x = -20; x < 20; ++x)
        for (int z = -20; z < 20; ++z) {
            const uint32_t mat = uint32_t((x * 7 + z * 13) & 3);
            Entity e = w.Create(Position{float(x) * 1.6f, 0.1f, float(z) * 1.6f}, Orientation{}, Scale{0.8f},
                                MeshRenderer{vk::Renderer::kMeshCube, mat, mat == 3});
            if ((x + z) % 3 == 0) w.Add(e, Spin{{0, 1, 0}, 0.5f + float((x * z) & 7) * 0.2f});
        }
    for (int i = 0; i < 400; ++i) {
        const float h = float(i) * 0.61803f;
        w.Create(Position{}, PointLight{{0.5f + 0.5f * std::sin(h * 6.28f), 0.5f + 0.5f * std::sin(h * 6.28f + 2.1f),
                                         0.5f + 0.5f * std::sin(h * 6.28f + 4.2f)}, 6.0f, 4.0f});
    }
}

void OrbitCamera(World& w, float t)
{
    for (auto [e, cam, pos] : w.Query<const Camera, Position>()) {
        (void)cam;
        const float a = t * 0.1f;
        pos = {std::sin(a) * 34.0f, 14.0f, std::cos(a) * 34.0f};
        auto* o = w.Get<Orientation>(e);
        if (!o) break;
        // Look at the origin: yaw around Y, then pitch down.
        const Quat yaw = QuatAxisAngle({0, 1, 0}, a);
        const Quat pitch = QuatAxisAngle({1, 0, 0}, -std::atan2(14.0f, 34.0f));
        const Quat q = yaw * pitch;
        o->q[0] = q.x; o->q[1] = q.y; o->q[2] = q.z; o->q[3] = q.w;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string module_path = argc > 1 ? argv[1] : DEMO_GAMEPLAY_MODULE;
    const std::string script_path = std::string(DEMO_SCRIPTS_DIR) + "/lights.lua";

    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW init failed (no display?)\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "ECS Engine - clustered forward", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "window creation failed\n");
        return 1;
    }

    vk::Context ctx;
    vk::ContextDesc desc;
#ifdef NDEBUG
    desc.validation = false;
#endif
    if (!ctx.Init(window, desc)) return 1;
    vk::Renderer renderer;
    renderer.Init(ctx, DEMO_SHADER_DIR);

    // ImGui (design doc §10: debug UI from day one), dynamic rendering.
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(window, true);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VkDescriptorPool imgui_pool;
    VK_CHECK(vkCreateDescriptorPool(ctx.Device(), &pi, nullptr, &imgui_pool));
    VkFormat swap_format = ctx.SwapchainFormat();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = ctx.Instance();
    ii.PhysicalDevice = ctx.Physical();
    ii.Device = ctx.Device();
    ii.QueueFamily = ctx.QueueFamily();
    ii.Queue = ctx.Queue();
    ii.DescriptorPool = imgui_pool;
    ii.MinImageCount = 2;
    ii.ImageCount = ctx.ImageCount();
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_format;
    ImGui_ImplVulkan_Init(&ii);

    // Simulation.
    World world;
    BuildScene(world);
    Scheduler scheduler;
    SystemPipeline pipeline(world, scheduler);
    ModuleHost modules(world, scheduler, pipeline);
    std::string err;
    if (!modules.Load(module_path, &err)) std::fprintf(stderr, "gameplay module: %s (continuing without it)\n", err.c_str());
    ScriptSystem lights_script(world, script_path);
    lights_script.Load();
    pipeline.Add(lights_script.MakeSystem());

    PacketQueue packets;
    auto last = std::chrono::steady_clock::now();
    float time = 0, frame_ms = 16;
    FrameStats sim_stats;
    double sim_ms = 0, extract_ms = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.05f, std::chrono::duration<float>(now - last).count());
        last = now;
        time += dt;
        frame_ms = frame_ms * 0.95f + dt * 1000.0f * 0.05f;

        // Hot reload: between frames, nothing in flight (§3.4).
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) modules.Reload(&err);
        else modules.PollAndReload(150, &err);
        lights_script.ReloadIfChanged();
        if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS) std::printf("%s\n", renderer.GraphDot().c_str());

        // Simulate, then extract into the packet the renderer isn't reading.
        auto t0 = std::chrono::steady_clock::now();
        OrbitCamera(world, time);
        sim_stats = pipeline.Run(dt);
        auto t1 = std::chrono::steady_clock::now();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        Extract(world, scheduler, h ? float(w) / float(h) : 1.0f, packets.BeginWrite());
        packets.Publish();
        auto t2 = std::chrono::steady_clock::now();
        sim_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        extract_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

        if (!ctx.BeginFrame()) continue;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Engine");
        ImGui::Text("%.2f ms  (%.0f fps)", frame_ms, 1000.0f / frame_ms);
        ImGui::SeparatorText("Simulation");
        ImGui::Text("entities %zu   archetypes %zu   chunks %zu", world.EntityCount(), world.Archetypes().size(), world.ChunkCount());
        ImGui::Text("systems %zu  stages %u  sync points %u  jobs %u", pipeline.Systems().size(), sim_stats.stages,
                    sim_stats.sync_points, sim_stats.jobs);
        ImGui::Text("simulate %.2f ms   extract %.2f ms   apply %.3f ms", sim_ms, extract_ms, sim_stats.apply_ms);
        ImGui::SeparatorText("Hot reload");
        ImGui::Text("module '%s' v%u  reloads %u  (last %.1f ms)", modules.ModuleName().c_str(), modules.ModuleVersion(),
                    modules.ReloadCount(), modules.LastReloadMs());
        ImGui::Text("script '%s'  reloads %u  %s", lights_script.Name().c_str(), lights_script.ReloadCount(),
                    lights_script.Healthy() ? "ok" : lights_script.LastError().c_str());
        ImGui::SeparatorText("Render graph");
        const auto& rs = renderer.Stats();
        ImGui::Text("passes %u (culled %u)  barriers %u  compiles %u", rs.passes, rs.culled_passes, rs.barriers, rs.graph_compiles);
        ImGui::Text("draws %u  instances %u  lights %u", rs.draw_calls, rs.instances, rs.lights);
        ImGui::Text("transient memory %.1f MB (unaliased %.1f MB)", double(renderer.Backend().AliasedBytes()) / 1e6,
                    double(renderer.Backend().UnaliasedBytes()) / 1e6);
        for (const auto& [name, ms] : renderer.Backend().PassTimings()) ImGui::Text("  %-14s %.3f ms", name.c_str(), ms);
        ImGui::End();
        ImGui::Render();

        if (const RenderPacket* packet = packets.AcquireLatest()) {
            renderer.Render(*packet, [](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
            packets.Release();
        }
        ctx.EndFrame();
        ENGINE_FRAME_MARK;
    }

    ctx.WaitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(ctx.Device(), imgui_pool, nullptr);
    renderer.Shutdown();
    ctx.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
