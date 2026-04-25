cmake_minimum_required(VERSION 3.16)
project(masterbandit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Where bundled assets (shaders, fonts, scripts) get copied. On macOS this is
# inside the .app bundle's Contents/Resources so the binary picks them up via
# [NSBundle mainBundle]; elsewhere it's flat next to the executable.
if(APPLE)
    set(MB_RESOURCES_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mb.app/Contents/Resources")
else()
    set(MB_RESOURCES_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

option(ASAN "Enable AddressSanitizer" OFF)
option(TSAN "Enable ThreadSanitizer" OFF)
if(ASAN AND TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
endif()
if(ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_subdirectory(3rdparty)
add_subdirectory(src)
add_subdirectory(tests)
