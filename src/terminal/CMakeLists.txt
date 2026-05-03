add_library(terminal OBJECT
    Terminal.cpp
    TerminalEmulator.cpp
    ParseToActions.cpp
    Keyboard.cpp
    MouseAndSelection.cpp
    SGR.cpp
    OSC.cpp
    KittyGraphics.cpp
    DCS.cpp
    CellGrid.cpp
    Document.cpp
    LineBuffer.cpp
    TerminalSnapshot.cpp
    PtyMux.cpp
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
    grapheme
    # PtyMux pulls in FdPoller_kqueue / FdPoller_epoll from the
    # eventloop OBJECT lib. Marking PUBLIC so consumers (mb-tests,
    # scroll-bench, mb itself) inherit the eventloop objects.
    eventloop
)

target_include_directories(terminal PRIVATE ${Stb_INCLUDE_DIR})
