# Compile options shared by every Orbit target. Linking against orbit_options
# is how a target opts in to the project's warning and sanitizer settings.
add_library(orbit_options INTERFACE)

target_compile_features(orbit_options INTERFACE cxx_std_20)
target_compile_options(orbit_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)

# GCC 13 reports a false positive inside std::vector::push_back after inlining.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(orbit_options INTERFACE -Wno-stringop-overflow)
endif()

if(ORBIT_WERROR)
    target_compile_options(orbit_options INTERFACE -Werror)
endif()

# Sanitizers apply globally rather than through orbit_options: dependencies such
# as GoogleTest must be instrumented too, because mixing instrumented and plain
# builds of the same standard library containers gives false reports.
if(ORBIT_SANITIZE)
    add_compile_options(-fsanitize=${ORBIT_SANITIZE} -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${ORBIT_SANITIZE})
endif()
