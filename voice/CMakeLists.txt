cmake_minimum_required(VERSION 3.10)
project(cockpit_voice_services)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
  ${CMAKE_SOURCE_DIR}/../zmq-comm-kit/include
)

add_executable(pressure_service pressure_service.cpp)
add_executable(cockpit_voice_service cockpit_voice_service.cpp)

target_link_libraries(pressure_service
  zmq
)

target_link_libraries(cockpit_voice_service
  /usr/local/lib/libzmq_component.so
  zmq
)
