find_package(Dawn REQUIRED)
# harfbuzz built from 3rdparty submodule (v14+)
# SheenBidi built from 3rdparty submodule (patched for U+276F BiDi crash)
find_package(spdlog REQUIRED CONFIG)
find_package(libunibreak CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(libwebsockets CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)

add_subdirectory(terminal)
add_subdirectory(platform)
add_subdirectory(eventloop)

add_executable(mb
    main.cpp
    Tab.cpp Layout.cpp text.cpp ColrEncoder.cpp ColrAtlas.cpp
    DebugIPC.cpp CLIClient.cpp Config.cpp Bindings.cpp Action.cpp
    script/ScriptEngine.cpp script/ScriptEngine_Terminals.cpp
    script/ScriptPermissions.cpp script/ScriptFsModule.cpp
    script/ScriptWsModule.cpp script/ScriptLayoutBindings.cpp
    ClickDetector.cpp
    Uuid.cpp
    LayoutTree.cpp
)

target_include_directories(mb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/terminal
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/script
    ${CMAKE_CURRENT_SOURCE_DIR}/eventloop
)

target_link_libraries(mb PRIVATE terminal platform eventloop glaze::glaze cxxopts::cxxopts qjs)

# Copy assets next to the executable.
#
# configure_file(... COPYONLY) only runs at cmake-configure time; if a source
# asset changes, the build-side copy stays stale until someone re-runs cmake.
# Use add_custom_command with DEPENDS so content changes trigger a re-copy on
# every `cmake --build`. CONFIGURE_DEPENDS on each glob makes the build system
# re-run cmake when a file is added or removed from the watched directory.
set(COPIED_ASSETS)

macro(mb_copy_asset SRC DST)
    add_custom_command(
        OUTPUT ${DST}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST}
        DEPENDS ${SRC}
        VERBATIM
    )
    list(APPEND COPIED_ASSETS ${DST})
endmacro()

# Shaders
file(GLOB SHADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.wgsl")
foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    mb_copy_asset(${SHADER} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${SHADER_NAME}")
endforeach()

# Bundled fonts (preserving subdirectory structure)
file(GLOB_RECURSE FONT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/fonts/**/*.ttf")
foreach(FONT ${FONT_FILES})
    file(RELATIVE_PATH FONT_REL "${CMAKE_SOURCE_DIR}/assets/fonts" ${FONT})
    mb_copy_asset(${FONT} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/${FONT_REL}")
endforeach()

# Bundled scripts
file(GLOB SCRIPT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/scripts/*.js")
foreach(SCRIPT ${SCRIPT_FILES})
    get_filename_component(SCRIPT_NAME ${SCRIPT} NAME)
    mb_copy_asset(${SCRIPT} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/scripts/${SCRIPT_NAME}")
endforeach()

# Bundled JS modules
file(GLOB MODULE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/scripts/modules/*.js")
foreach(MOD ${MODULE_FILES})
    get_filename_component(MOD_NAME ${MOD} NAME)
    mb_copy_asset(${MOD} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/scripts/modules/${MOD_NAME}")
endforeach()

# hb-gpu WGSL helper shaders from harfbuzz source
mb_copy_asset("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-vertex.wgsl"
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-vertex.wgsl")
mb_copy_asset("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-fragment.wgsl"
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/hb-gpu-fragment.wgsl")

add_custom_target(mb_assets ALL DEPENDS ${COPIED_ASSETS})
add_dependencies(mb mb_assets)
