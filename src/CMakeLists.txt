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
    text.cpp ColrEncoder.cpp ColrAtlas.cpp
    DebugIPC.cpp CLIClient.cpp Config.cpp Bindings.cpp Action.cpp
    script/ScriptEngine.cpp script/ScriptEngine_Terminals.cpp
    script/ScriptPermissions.cpp script/ScriptFsModule.cpp
    script/ScriptWsModule.cpp script/ScriptLayoutBindings.cpp
    ClickDetector.cpp
    Resources.cpp
    Uuid.cpp
    LayoutTree.cpp
)

if(APPLE)
    set_target_properties(mb PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MasterBandit"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.jhanssen.masterbandit"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_INFO_STRING "MasterBandit terminal"
    )

    # Ad-hoc codesign every build so UNUserNotificationCenter and other
    # privacy-gated APIs stop refusing the bundle. The MB_ADHOC_SIGN option
    # is declared at the top of the top-level CMakeLists.txt; CI disables it
    # when running a full Developer ID + notarization pipeline so the proper
    # signature isn't immediately overwritten.
    if(MB_ADHOC_SIGN)
        add_custom_command(TARGET mb POST_BUILD
            COMMAND codesign --sign - --force --deep "$<TARGET_BUNDLE_DIR:mb>"
            VERBATIM
        )
    endif()
endif()

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
    mb_copy_asset(${SHADER} "${MB_RESOURCES_ROOT}/shaders/${SHADER_NAME}")
endforeach()

# Bundled fonts (preserving subdirectory structure)
file(GLOB_RECURSE FONT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/fonts/**/*.ttf")
foreach(FONT ${FONT_FILES})
    file(RELATIVE_PATH FONT_REL "${CMAKE_SOURCE_DIR}/assets/fonts" ${FONT})
    mb_copy_asset(${FONT} "${MB_RESOURCES_ROOT}/fonts/${FONT_REL}")
endforeach()

# Bundled scripts
file(GLOB SCRIPT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/scripts/*.js")
foreach(SCRIPT ${SCRIPT_FILES})
    get_filename_component(SCRIPT_NAME ${SCRIPT} NAME)
    mb_copy_asset(${SCRIPT} "${MB_RESOURCES_ROOT}/scripts/${SCRIPT_NAME}")
endforeach()

# Bundled JS modules
file(GLOB MODULE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/scripts/modules/*.js")
foreach(MOD ${MODULE_FILES})
    get_filename_component(MOD_NAME ${MOD} NAME)
    mb_copy_asset(${MOD} "${MB_RESOURCES_ROOT}/scripts/modules/${MOD_NAME}")
endforeach()

# hb-gpu WGSL helper shaders from harfbuzz source
mb_copy_asset("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-vertex.wgsl"
              "${MB_RESOURCES_ROOT}/shaders/hb-gpu-vertex.wgsl")
mb_copy_asset("${CMAKE_SOURCE_DIR}/3rdparty/harfbuzz/src/hb-gpu-fragment.wgsl"
              "${MB_RESOURCES_ROOT}/shaders/hb-gpu-fragment.wgsl")

add_custom_target(mb_assets ALL DEPENDS ${COPIED_ASSETS})
add_dependencies(mb mb_assets)
