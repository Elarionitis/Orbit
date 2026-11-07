add_library(orbit_common STATIC
    crc32c.cpp
    log.cpp
    metrics.cpp
    timer_wheel.cpp)

target_include_directories(orbit_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(orbit_common PUBLIC ORBIT_VERSION="${PROJECT_VERSION}")
target_link_libraries(orbit_common PUBLIC orbit_options Threads::Threads)
