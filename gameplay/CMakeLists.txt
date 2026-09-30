# Reloadable gameplay modules. They compile against engine HEADERS only and
# never link the engine library: a module with its own copy of the engine
# would carry its own statics - exactly what the module contract forbids.
find_package(Python3 COMPONENTS Interpreter)

function(add_gameplay_module target version source verify)
  add_library(${target} MODULE ${source})
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/engine)
  target_compile_definitions(${target} PRIVATE GAMEPLAY_VERSION=${version})
  set_target_properties(${target} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib")
  if(APPLE)
    # Modules resolve nothing from the host by link-time symbol, but a
    # sanitized build references the sanitizer runtime the host provides.
    target_link_options(${target} PRIVATE -undefined dynamic_lookup)
  elseif(NOT ENGINE_SANITIZE STREQUAL "")
    target_link_options(${target} PRIVATE -Wl,--unresolved-symbols=ignore-all)
  endif()
  if(verify AND Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/module_verify.py $<TARGET_FILE:${target}>
      COMMENT "Verifying module contract for ${target}")
  endif()
endfunction()

add_gameplay_module(gameplay     2 gameplay.cpp ON)    # the one you edit and hot reload
add_gameplay_module(gameplay_v1  1 gameplay.cpp ON)    # test fixtures
add_gameplay_module(gameplay_v2  2 gameplay.cpp ON)
add_gameplay_module(gameplay_bad 1 gameplay_bad.cpp OFF)
