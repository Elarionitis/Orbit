if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ORBIT_POLLER_SOURCE poller_epoll.cpp)
else()
    set(ORBIT_POLLER_SOURCE poller_kqueue.cpp)
endif()

add_library(orbit_net STATIC
    connection.cpp
    event_loop.cpp
    listener.cpp
    socket.cpp
    ${ORBIT_POLLER_SOURCE})
target_link_libraries(orbit_net PUBLIC orbit_common)
