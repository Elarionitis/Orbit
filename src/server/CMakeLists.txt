add_executable(orbit_server main.cpp server.cpp)
target_link_libraries(orbit_server PRIVATE Threads::Threads)
