find_package(Threads REQUIRED)

add_executable(orbit_client main.cpp client.cpp)

# Ensure modern compiler flags are used
target_compile_features(orbit_client PRIVATE cxx_std_17)
target_compile_options(orbit_client PRIVATE -Wall -Wextra -Werror -pedantic)

# Link thread library
target_link_libraries(orbit_client PRIVATE Threads::Threads)
