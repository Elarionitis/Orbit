add_executable(nexus_client main.cpp)

# Ensure modern compiler flags are used
target_compile_features(nexus_client PRIVATE cxx_std_17)
target_compile_options(nexus_client PRIVATE -Wall -Wextra -Werror -pedantic)
