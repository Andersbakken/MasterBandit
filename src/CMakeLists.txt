cmake_minimum_required(VERSION 2.8)
include_directories(${CMAKE_CURRENT_LIST_DIR})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

include_directories(${CMAKE_CURRENT_LIST_DIR})
add_subdirectory(qt)
add_executable(mb main.cpp Terminal.cpp Log.cpp)
target_link_libraries(mb qtwindow)
