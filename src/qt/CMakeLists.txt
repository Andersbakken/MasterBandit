cmake_minimum_required(VERSION 2.8)
include_directories(${CMAKE_CURRENT_LIST_DIR})
set(CMAKE_AUTOMOC ON)
find_package(Qt5Widgets CONFIG REQUIRED)
add_library(platform_qt PlatformQt.cpp)
target_link_libraries(platform_qt Qt5::Widgets)
target_include_directories(platform_qt PUBLIC ${CMAKE_CURRENT_LIST_DIR})

