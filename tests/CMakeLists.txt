find_package(doctest CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_executable(mb-tests
    test_terminal.cpp
    test_sgr.cpp
    test_cursor.cpp
    ../src/TerminalEmulator.cpp
    ../src/Document.cpp
    ../src/CellGrid.cpp
    ../src/Log.cpp
)

target_include_directories(mb-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(mb-tests PRIVATE
    doctest::doctest
    spdlog::spdlog
)

include(doctest)
doctest_discover_tests(mb-tests)
