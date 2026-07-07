find_package(doctest CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# SheenBidi comes from 3rdparty submodule (target SheenBidi::SheenBidi)
find_package(Stb REQUIRED)
find_package(libwebsockets CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)

set(TEST_SOURCES
    test_terminal.cpp
    test_sgr.cpp
    test_sgr_extended.cpp
    test_cursor.cpp
    test_screen.cpp
    test_modes.cpp
    test_scrollback.cpp
    test_osc.cpp
    test_wide.cpp
    test_inverse.cpp
    test_selection.cpp
    test_shaping.cpp
    test_font_fallback.cpp
    test_bindings.cpp
    test_action_args.cpp
    ../src/Bindings.cpp
    ../src/Action.cpp
    test_kitty_keyboard.cpp
    test_reflow.cpp
    test_prompt.cpp
    test_archive.cpp
    test_linebuffer.cpp
    test_scroll_region.cpp
    test_render.cpp
    test_cwd.cpp
    test_osc_palette.cpp
    test_decrqss.cpp
    test_kitty_graphics.cpp
    test_osc_1337.cpp
    test_popup.cpp
    test_embedded_terminals.cpp
    test_output_capture.cpp
    test_tabs.cpp
    test_charset.cpp
    test_layout_tree.cpp
    test_tree_shape.cpp
    test_tabs_uuid.cpp
    test_tabs_multibar.cpp
    test_uuid.cpp
    test_pty_mux.cpp
    test_findtext.cpp
    test_url_detection.cpp
    test_animation.cpp
    test_colr_atlas.cpp
    MBConnection.cpp
    ../src/text.cpp
    ../src/ColrEncoder.cpp
    ../src/ColrAtlas.cpp
    ../src/LayoutTree.cpp
    ../src/Uuid.cpp
    ../src/script/ScriptEngine_Terminals.cpp
    # Permission parsing / formatting. Pulled in so the permission-name unit
    # tests can run without standing up a full ScriptEngine. The TU has zero
    # deps on QuickJS.
    ../src/script/ScriptPermissions.cpp
    # SHA-256 helper used by the script allowlist and by the
    # known-vectors / streaming tests below.
    ../src/Sha256.cpp
    test_permissions.cpp
    # Cross-platform detached spawn. Self-contained (libc + spdlog),
    # so we link it directly here rather than dragging in the rest of
    # the platform OBJECT library (Dawn / Cocoa / DBus / etc.).
    ../src/platform/PlatformSpawn.cpp
    test_platform_spawn.cpp
    test_shell_integration.cpp
)

if(APPLE)
    list(APPEND TEST_SOURCES ../src/FontResolver_CoreText.mm ../src/FontFallback_CoreText.mm)
else()
    list(APPEND TEST_SOURCES ../src/FontResolver_FontConfig.cpp ../src/FontFallback_FontConfig.cpp)
endif()

add_executable(mb-tests ${TEST_SOURCES})
mb_enable_arc_on_mm(TEST_SOURCES)

target_include_directories(mb-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/terminal
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/script
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(mb-tests PRIVATE
    terminal
    eventloop
    doctest::doctest
    spdlog::spdlog
    harfbuzz
    harfbuzz-gpu
    SheenBidi::SheenBidi
    libunibreak::libunibreak
    websockets
    glaze::glaze
)

target_compile_definitions(mb-tests PRIVATE
    MB_BINARY="$<TARGET_FILE:mb>"
    MB_TEST_FONT="${MB_RESOURCES_ROOT}/fonts/fira/FiraCodeNerdFontMono-Regular.ttf"
    MB_TEST_TEXT_FONT="${MB_RESOURCES_ROOT}/fonts/inconsolata/Inconsolata-Regular.ttf"
    MB_TEST_FALLBACK_FONT="${MB_RESOURCES_ROOT}/fonts/test-fallback/DejaVuSansMono-subset.ttf"
    MB_TEST_EMOJI_FONT="${MB_RESOURCES_ROOT}/fonts/emoji/NotoColorEmoji-subset.ttf"
    # Hand-crafted ~800 B emoji font that REPRODUCES the NotoColorEmoji
    # VS16 .null-phantom GSUB pattern (warn + VS16 → warn + space). Built
    # by tests/scripts/make_buggy_vs16_font.py. Used by the VS16 glyph-count
    # regression test — the standard subset above is too minimal to
    # exercise that path.
    MB_TEST_BUGGY_VS16_FONT="${MB_RESOURCES_ROOT}/fonts/test-buggy-vs16/buggy-vs16.ttf"
    MB_TEST_SHELL_INTEGRATION_DIR="${MB_RESOURCES_ROOT}/shell-integration"
    MB_REF_DIR="${CMAKE_SOURCE_DIR}/tests/reference"
)

if(APPLE)
    target_link_libraries(mb-tests PRIVATE "-framework CoreText" "-framework Cocoa")
    # CommonCrypto is part of libSystem on macOS, no extra link needed.
else()
    find_package(Fontconfig REQUIRED)
    # OpenSSL is the SHA-256 backend on non-Apple — same dep ScriptPermissions
    # picks up in the main target via implicit linkage. Made explicit here
    # because mb-tests doesn't transitively pull in the platform link block.
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(mb-tests PRIVATE
        Fontconfig::Fontconfig
        OpenSSL::Crypto
    )
endif()

include(doctest)
doctest_discover_tests(mb-tests)
