include(FetchContent)

# The tests pull in GoogleTest, which is built without the project's strict warnings.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(orbit_unit_tests
    unit/byte_buffer_test.cpp
    unit/connection_test.cpp
    unit/crc32c_test.cpp
    unit/event_loop_test.cpp
    unit/metrics_test.cpp
    unit/protocol_test.cpp
    unit/timer_wheel_test.cpp
    unit/token_bucket_test.cpp
    unit/wal_test.cpp)
target_include_directories(orbit_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbit_unit_tests PRIVATE orbit_storage GTest::gtest_main)
if(ORBIT_ENABLE_TLS)
    target_sources(orbit_unit_tests PRIVATE unit/tls_test.cpp)
endif()
gtest_discover_tests(orbit_unit_tests DISCOVERY_MODE PRE_TEST)
