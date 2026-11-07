add_library(orbit_protocol STATIC messages.cpp)
target_link_libraries(orbit_protocol PUBLIC orbit_common)
