find_package(benchmark REQUIRED)
add_library(hustle_src_benchmark
        ssb_workload.cc ssb_workload.h
        ssb_workload_lip.cc)

target_include_directories(hustle_src_benchmark PUBLIC ${ARROW_INCLUDE_DIR})
target_link_libraries(hustle_src_benchmark PUBLIC
        hustle_src_storage
        hustle_src_operators
        hustle_src_scheduler_Scheduler
        hustle_src_optimizer_ExecutionPlan
        hustle_src_utils_EventProfiler
        hustle_src_utils_skew
        ${ARROW_SHARED_LIB}
        )

add_executable(hustle_src_benchmark_main main.cc
        ../utils/bloom_filter.h
        ../utils/xxHash.h)
target_link_libraries(hustle_src_benchmark_main PUBLIC
        hustle_src_benchmark
        benchmark::benchmark
        )

add_definitions(-DHUSTLE_BUILD_DIRECTORY="\\"${CMAKE_SOURCE_DIR}\\"")
