set(EVENTLOOP_SOURCES
    # Platform-independent sources will go here as Input.cpp / Actions.cpp are migrated
)

if(APPLE)
    list(APPEND EVENTLOOP_SOURCES
        mac/EventLoop_nsapp.mm
        mac/Window_cocoa.mm
        kqueue/EventLoop_kqueue.cpp
        kqueue/FdPoller_kqueue.cpp
    )
else()
    list(APPEND EVENTLOOP_SOURCES
        epoll/EventLoop_epoll.cpp
        epoll/FdPoller_epoll.cpp
        xcb/Window_xcb.cpp
        wayland/Window_wayland.cpp
        xkb/Xkb.cpp
    )

    # Wayland xdg-shell codegen runs at configure-discovery time so the
    # generated .c can join EVENTLOOP_SOURCES before add_library is called.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED "wayland-protocols >= 1.32")
    pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner)
    pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)
    pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
    if(NOT WAYLAND_SCANNER_BIN)
        find_program(WAYLAND_SCANNER_BIN wayland-scanner REQUIRED)
    endif()

    set(WAYLAND_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-generated")
    file(MAKE_DIRECTORY "${WAYLAND_GEN_DIR}")

    # Stage 1 needs xdg-shell only; further protocols (cursor-shape,
    # fractional-scale, primary-selection, ...) join this list as stages land.
    set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml")
    set(XDG_SHELL_HEADER "${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_SOURCE "${WAYLAND_GEN_DIR}/xdg-shell-protocol.c")
    add_custom_command(
        OUTPUT "${XDG_SHELL_HEADER}"
        COMMAND "${WAYLAND_SCANNER_BIN}" client-header "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
        DEPENDS "${XDG_SHELL_XML}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${XDG_SHELL_SOURCE}"
        COMMAND "${WAYLAND_SCANNER_BIN}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_SOURCE}"
        DEPENDS "${XDG_SHELL_XML}"
        VERBATIM
    )

    list(APPEND EVENTLOOP_SOURCES
        "${XDG_SHELL_SOURCE}"
        "${XDG_SHELL_HEADER}"
    )
endif()

add_library(eventloop OBJECT ${EVENTLOOP_SOURCES})
mb_enable_arc_on_mm(EVENTLOOP_SOURCES)

target_include_directories(eventloop PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/terminal
    ${CMAKE_SOURCE_DIR}/src/script
)

target_link_libraries(eventloop PUBLIC
    dawn::webgpu_dawn
    spdlog::spdlog
)

if(APPLE)
    target_link_libraries(eventloop PUBLIC
        "-framework AppKit"
        "-framework Carbon"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Metal"
        "-framework QuartzCore"
    )
else()
    pkg_check_modules(XCB REQUIRED xcb xcb-util xcb-sync xcb-xkb xcb-cursor)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon xkbcommon-x11)
    pkg_check_modules(X11 REQUIRED x11 x11-xcb)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

    target_include_directories(eventloop PRIVATE
        ${XCB_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        "${WAYLAND_GEN_DIR}"
    )
    target_link_libraries(eventloop PUBLIC
        ${XCB_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${X11_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
    )
endif()
