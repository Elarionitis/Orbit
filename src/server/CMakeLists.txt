find_package(Threads REQUIRED)

add_executable(nexus_server main.cpp server.cpp)

# Ensure modern compiler flags are used
target_compile_features(nexus_server PRIVATE cxx_std_17)
target_compile_options(nexus_server PRIVATE -Wall -Wextra -Werror -pedantic)

# Link thread library
target_link_libraries(nexus_server PRIVATE Threads::Threads)
