cmake_minimum_required(VERSION 3.21)
project(Orbit
    VERSION 2.0.0
    DESCRIPTION "Sharded, durable real-time chat server"
    LANGUAGES CXX)

option(ORBIT_BUILD_TESTS "Build the unit and integration tests" ${PROJECT_IS_TOP_LEVEL})
option(ORBIT_BUILD_BENCH "Build the load generator" ON)
option(ORBIT_BUILD_FUZZERS "Build libFuzzer targets (requires clang)" OFF)
option(ORBIT_ENABLE_TLS "Enable TLS support through OpenSSL" ON)
option(ORBIT_WERROR "Treat compiler warnings as errors" OFF)
set(ORBIT_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(cmake/OrbitOptions.cmake)

find_package(Threads REQUIRED)

if(ORBIT_ENABLE_TLS)
    if(APPLE AND NOT OPENSSL_ROOT_DIR)
        execute_process(COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE ORBIT_BREW_OPENSSL
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(ORBIT_BREW_OPENSSL)
            set(OPENSSL_ROOT_DIR "${ORBIT_BREW_OPENSSL}")
        endif()
    endif()
    find_package(OpenSSL REQUIRED)
endif()

add_subdirectory(src)

if(ORBIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ORBIT_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
