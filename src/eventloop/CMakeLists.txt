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
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED xcb xcb-util xcb-sync xcb-xkb xcb-cursor)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon xkbcommon-x11)
    pkg_check_modules(X11 REQUIRED x11 x11-xcb)

    # Wayland backend lives alongside the XCB one and is always built on
    # Linux. Selection happens at runtime (see PlatformDawn.cpp).
    # wayland-protocols >= 1.32 is needed for cursor-shape-v1 +
    # fractional-scale-v1; required upfront so users hit the version error
    # at configure rather than later in the port. wayland-cursor will be
    # added when the cursor code lands, if we don't end up using
    # wp_cursor_shape_v1 exclusively.
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED "wayland-protocols >= 1.32")

    target_include_directories(eventloop PRIVATE
        ${XCB_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
    )
    target_link_libraries(eventloop PUBLIC
        ${XCB_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${X11_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
    )
endif()
