add_executable(engine_bench bench.cpp)
target_link_libraries(engine_bench PRIVATE engine)
engine_warnings(engine_bench)
