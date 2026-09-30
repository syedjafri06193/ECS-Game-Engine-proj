add_library(engine STATIC
  core/reflection/reflection.cpp
  core/platform/platform.cpp
  ecs/registry.cpp
  ecs/archetype.cpp
  ecs/world.cpp
  ecs/snapshot.cpp
  jobs/scheduler.cpp
  jobs/system_graph.cpp
  render/graph/render_graph.cpp
  render/cluster/cluster.cpp
  render/extract.cpp
  scripting/script_system.cpp
  modules/loader.cpp
)
target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine PUBLIC lua ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(engine PUBLIC Threads::Threads)
engine_warnings(engine)
if(ENGINE_TRACY)
  target_link_libraries(engine PUBLIC TracyClient)
  target_compile_definitions(engine PUBLIC ENGINE_TRACY)
endif()

if(ENGINE_BUILD_VULKAN)
  add_subdirectory(render/vk)
endif()
