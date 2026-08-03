cmake_minimum_required(VERSION 3.10)
project(Nexus VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard to 17 and make it required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add subdirectories
add_subdirectory(src/server)
add_subdirectory(src/client)
