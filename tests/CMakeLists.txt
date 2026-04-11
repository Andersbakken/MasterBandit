find_package(doctest CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SheenBidi CONFIG REQUIRED)
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
    test_kitty_keyboard.cpp
    test_reflow.cpp
    test_prompt.cpp
    test_render.cpp
    test_cwd.cpp
    test_osc_palette.cpp
    test_decrqss.cpp
    test_kitty_graphics.cpp
    MBConnection.cpp
    ../src/text.cpp
    ../src/ColrEncoder.cpp
)

if(APPLE)
    list(APPEND TEST_SOURCES ../src/FontResolver_CoreText.mm ../src/FontFallback_CoreText.mm)
else()
    list(APPEND TEST_SOURCES ../src/FontResolver_FontConfig.cpp ../src/FontFallback_FontConfig.cpp)
endif()

add_executable(mb-tests ${TEST_SOURCES})

target_include_directories(mb-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/terminal
    ${CMAKE_SOURCE_DIR}/src/platform
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(mb-tests PRIVATE
    terminal
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
    MB_TEST_FONT="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/fira/FiraCodeNerdFontMono-Regular.ttf"
    MB_TEST_TEXT_FONT="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/inconsolata/Inconsolata-Regular.ttf"
    MB_TEST_FALLBACK_FONT="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/test-fallback/DejaVuSansMono-subset.ttf"
    MB_TEST_EMOJI_FONT="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/emoji/NotoColorEmoji-subset.ttf"
    MB_REF_DIR="${CMAKE_SOURCE_DIR}/tests/reference"
)

if(APPLE)
    target_link_libraries(mb-tests PRIVATE "-framework CoreText" "-framework Cocoa")
else()
    find_package(Fontconfig REQUIRED)
    target_link_libraries(mb-tests PRIVATE Fontconfig::Fontconfig)
endif()

include(doctest)
doctest_discover_tests(mb-tests)
