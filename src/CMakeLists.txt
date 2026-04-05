find_package(Dawn REQUIRED)
find_package(glfw3 REQUIRED CONFIG)
# harfbuzz built from 3rdparty submodule (v14+)
find_package(spdlog REQUIRED CONFIG)
find_package(SheenBidi CONFIG REQUIRED)
find_package(libunibreak CONFIG REQUIRED)
find_package(libuv CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(libwebsockets CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_subdirectory(terminal)
add_subdirectory(platform)

add_executable(mb
    main.cpp
    Pane.cpp Tab.cpp Layout.cpp Log.cpp text.cpp
    DebugIPC.cpp CLIClient.cpp Config.cpp Bindings.cpp
)

target_include_directories(mb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/terminal
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
)

target_link_libraries(mb PRIVATE terminal platform glaze::glaze cxxopts::cxxopts)

# Copy shaders next to the executable
file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.wgsl")
foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${SHADER_NAME}" COPYONLY)
endforeach()

# Copy bundled fonts next to the executable (preserving subdirectory structure)
file(GLOB_RECURSE FONT_FILES "${CMAKE_SOURCE_DIR}/assets/fonts/**/*.ttf")
foreach(FONT ${FONT_FILES})
    file(RELATIVE_PATH FONT_REL "${CMAKE_SOURCE_DIR}/assets/fonts" ${FONT})
    configure_file(${FONT} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/${FONT_REL}" COPYONLY)
endforeach()

# Copy hb-gpu WGSL helper shaders from harfbuzz source
configure_file("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-vertex.wgsl"
               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-vertex.wgsl" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-fragment.wgsl"
               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-fragment.wgsl" COPYONLY)
