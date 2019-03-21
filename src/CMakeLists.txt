cmake_minimum_required(VERSION 3.10)
include_directories(${CMAKE_CURRENT_LIST_DIR})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

add_subdirectory(qt)
add_executable(mb main.cpp Terminal.cpp Log.cpp)
target_link_libraries(mb PRIVATE qtwindow)
target_link_libraries(mb PRIVATE utf8::cpp)
