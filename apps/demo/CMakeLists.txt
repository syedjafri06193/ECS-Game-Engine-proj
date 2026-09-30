add_executable(engine_demo main.cpp)
target_link_libraries(engine_demo PRIVATE engine_vk)
target_compile_definitions(engine_demo PRIVATE
  DEMO_GAMEPLAY_MODULE="$<TARGET_FILE:gameplay>"
  DEMO_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
  DEMO_SCRIPTS_DIR="${PROJECT_SOURCE_DIR}/scripts")
add_dependencies(engine_demo gameplay engine_shaders)
engine_warnings(engine_demo)
