add_library(orbit_client_core STATIC
    chat_client.cpp
    text.cpp)
target_link_libraries(orbit_client_core PUBLIC orbit_net orbit_protocol)

add_executable(orbit_client main.cpp terminal_app.cpp)
target_link_libraries(orbit_client PRIVATE orbit_client_core)
