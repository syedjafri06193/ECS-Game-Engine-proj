# Vulkan renderer. Built only with -DENGINE_BUILD_VULKAN=ON.
add_library(engine_vk STATIC
  vk_context.cpp
  vk_graph_backend.cpp
  vk_renderer.cpp
  vma_impl.cpp
)
target_link_libraries(engine_vk PUBLIC engine volk vk-bootstrap::vk-bootstrap vma glfw imgui)
target_compile_definitions(engine_vk PUBLIC VK_NO_PROTOTYPES)
engine_warnings(engine_vk)
# Vulkan code initialises structs as {VK_STRUCTURE_TYPE_...}; the rest is
# value-initialised on purpose.
target_compile_options(engine_vk PUBLIC $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>)
# VMA's implementation trips pedantic warnings we don't own.
set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---- shaders: GLSL -> SPIR-V at build time
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(NOT GLSLANG_VALIDATOR)
  message(STATUS "glslangValidator not found - building glslang from source")
  include(FetchContent)
  FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        15.0.0
    GIT_SHALLOW    TRUE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glslang)
  set(GLSLANG_VALIDATOR $<TARGET_FILE:glslang-standalone>)
  set(GLSLANG_DEP glslang-standalone)
endif()

set(SHADER_DIR ${PROJECT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
set(SHADERS depth.vert forward.vert forward.frag fullscreen.vert tonemap.frag cluster_cull.comp)
set(SPV_FILES)
foreach(s ${SHADERS})
  set(out ${SHADER_OUT}/${s}.spv)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 -g ${SHADER_DIR}/${s} -o ${out}
    DEPENDS ${SHADER_DIR}/${s} ${SHADER_DIR}/common.glsl ${GLSLANG_DEP}
    COMMENT "Compiling shader ${s}")
  list(APPEND SPV_FILES ${out})
endforeach()
add_custom_target(engine_shaders ALL DEPENDS ${SPV_FILES})
add_dependencies(engine_vk engine_shaders)
set(ENGINE_SHADER_OUT ${SHADER_OUT} PARENT_SCOPE)
