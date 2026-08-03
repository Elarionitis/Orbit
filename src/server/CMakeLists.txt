find_package(Threads REQUIRED)

add_executable(orbit_server main.cpp server.cpp)

# Ensure modern compiler flags are used
target_compile_features(orbit_server PRIVATE cxx_std_17)
target_compile_options(orbit_server PRIVATE -Wall -Wextra -Werror -pedantic)

# Link thread library
target_link_libraries(orbit_server PRIVATE Threads::Threads)
