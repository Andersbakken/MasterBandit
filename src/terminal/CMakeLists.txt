add_library(terminal OBJECT
    Terminal.cpp
    TerminalEmulator.cpp
    KittyKeyboard.cpp
    MouseAndSelection.cpp
    SGR.cpp
    OSC.cpp
    KittyGraphics.cpp
    DCS.cpp
    CellGrid.cpp
    Document.cpp
    TerminalSnapshot.cpp
)

target_include_directories(terminal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/eventloop
)

find_package(ZLIB REQUIRED)

target_link_libraries(terminal PUBLIC
    spdlog::spdlog
    ZLIB::ZLIB
)

target_include_directories(terminal PRIVATE ${Stb_INCLUDE_DIR})
