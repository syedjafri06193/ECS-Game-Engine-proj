# Third-party dependencies, pinned. Everything comes from source via
# FetchContent so a fresh clone builds with only a compiler and CMake.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- Lua 5.4 (scripting, §3.1 / M7). The official GitHub mirror ships onelua.c.
FetchContent_Declare(lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG        v5.4.7
  GIT_SHALLOW    TRUE)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
  FetchContent_Populate(lua)
endif()
add_library(lua STATIC ${lua_SOURCE_DIR}/onelua.c)
target_compile_definitions(lua PRIVATE MAKE_LIB LUA_USE_POSIX)
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
set_target_properties(lua PROPERTIES LINKER_LANGUAGE C)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lua PRIVATE -w)
endif()
if(UNIX)
  target_link_libraries(lua PUBLIC m)
endif()

# --- doctest (tests)
if(ENGINE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(doctest)
  if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
  endif()
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})
endif()

# --- Tracy (profiling, §9.5). Off by default; zones compile to nothing.
if(ENGINE_TRACY)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.11.1
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(tracy)
endif()

# --- Vulkan stack (§2.1: use volk, vk-bootstrap and VMA unconditionally)
if(ENGINE_BUILD_VULKAN)
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.3.296
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(vulkan_headers)

  FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        1.3.295
    GIT_SHALLOW    TRUE
    SYSTEM)
  set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(volk)
  target_link_libraries(volk PUBLIC Vulkan::Headers)

  FetchContent_Declare(vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.3.296
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(vk_bootstrap)

  FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(vma)
  if(NOT vma_POPULATED)
    FetchContent_Populate(vma)
  endif()
  add_library(vma INTERFACE)
  target_include_directories(vma SYSTEM INTERFACE ${vma_SOURCE_DIR}/include)

  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    SYSTEM)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)

  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  # Route ImGui's Vulkan calls through volk's loaded function pointers.
  target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK GLFW_INCLUDE_NONE)
  target_link_libraries(imgui PUBLIC glfw volk)
endif()
