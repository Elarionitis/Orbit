add_library(orbit_storage STATIC wal.cpp)
target_link_libraries(orbit_storage PUBLIC orbit_net orbit_protocol)
