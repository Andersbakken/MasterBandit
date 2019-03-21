cmake_minimum_required(VERSION 3.10)
include_directories(${CMAKE_CURRENT_LIST_DIR})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

set(CMAKE_AUTOMOC ON)
find_package(Qt5Widgets CONFIG REQUIRED)
add_executable(mb main.cpp Terminal.cpp Log.cpp Window.cpp)
target_link_libraries(mb PRIVATE Qt5::Widgets)
