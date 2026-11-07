add_library(orbit_server_core STATIC
    admin_http.cpp
    chat_server.cpp
    config.cpp
    room.cpp
    session.cpp
    shard.cpp)
target_link_libraries(orbit_server_core PUBLIC orbit_storage)

add_executable(orbit_server main.cpp)
target_link_libraries(orbit_server PRIVATE orbit_server_core)
