add_executable(engine_tests
  main.cpp
  test_ecs.cpp
  test_jobs.cpp
  test_render.cpp
  test_scripting.cpp
  test_modules.cpp
)
target_link_libraries(engine_tests PRIVATE engine doctest)
target_compile_definitions(engine_tests PRIVATE
  GAMEPLAY_V1_PATH="$<TARGET_FILE:gameplay_v1>"
  GAMEPLAY_V2_PATH="$<TARGET_FILE:gameplay_v2>")
add_dependencies(engine_tests gameplay_v1 gameplay_v2)
engine_warnings(engine_tests)
add_test(NAME engine_tests COMMAND engine_tests)

# The module contract verifier must accept good modules and reject the bad one.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME module_verify_accepts_gameplay
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/module_verify.py $<TARGET_FILE:gameplay_v2>)
  add_test(NAME module_verify_rejects_bad_module
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/module_verify.py $<TARGET_FILE:gameplay_bad>)
  set_tests_properties(module_verify_rejects_bad_module PROPERTIES WILL_FAIL TRUE)
endif()
