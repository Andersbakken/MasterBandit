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

target_link_libraries(mb PRIVATE terminal platform glaze::glaze)

# Copy shaders next to the executable
file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.wgsl")
foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${SHADER_NAME}" COPYONLY)
endforeach()

# Copy bundled fonts next to the executable
file(GLOB NERD_FONT_FILES "${CMAKE_SOURCE_DIR}/assets/fonts/nerd/*.ttf")
foreach(FONT ${NERD_FONT_FILES})
    get_filename_component(FONT_NAME ${FONT} NAME)
    configure_file(${FONT} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/nerd/${FONT_NAME}" COPYONLY)
endforeach()

# Copy hb-gpu WGSL helper shaders from harfbuzz source
configure_file("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-vertex.wgsl"
               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-vertex.wgsl" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-fragment.wgsl"
               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-fragment.wgsl" COPYONLY)
