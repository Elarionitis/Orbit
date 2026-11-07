add_executable(orbit_client main.cpp client.cpp)
target_link_libraries(orbit_client PRIVATE Threads::Threads)
