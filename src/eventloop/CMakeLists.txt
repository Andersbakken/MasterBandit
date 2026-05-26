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

    # Per-protocol codegen helper. wayland-scanner ships two passes:
    #   client-header -> <name>-client-protocol.h (forward decls + listener structs)
    #   private-code  -> <name>-protocol.c        (wire-format dispatch tables)
    # Each protocol XML produces one of each.
    function(mb_wayland_scan basename xml_path)
        set(_h "${WAYLAND_GEN_DIR}/${basename}-client-protocol.h")
        set(_c "${WAYLAND_GEN_DIR}/${basename}-protocol.c")
        add_custom_command(
            OUTPUT "${_h}"
            COMMAND "${WAYLAND_SCANNER_BIN}" client-header "${xml_path}" "${_h}"
            DEPENDS "${xml_path}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${_c}"
            COMMAND "${WAYLAND_SCANNER_BIN}" private-code "${xml_path}" "${_c}"
            DEPENDS "${xml_path}"
            VERBATIM
        )
        list(APPEND EVENTLOOP_SOURCES "${_h}" "${_c}")
        set(EVENTLOOP_SOURCES "${EVENTLOOP_SOURCES}" PARENT_SCOPE)
    endfunction()

    mb_wayland_scan(xdg-shell
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml")

    # cursor-shape-v1: lets the compositor pick the right themed cursor from
    # a CSS-style enum (Stage 3). Its private-code references
    # zwp_tablet_tool_v2_interface (cursor-shape has a get_tablet_tool_v2
    # variant we never call), so we must also codegen tablet-v2 to satisfy
    # the linker even though no tablet code path exists.
    mb_wayland_scan(cursor-shape-v1
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/staging/cursor-shape/cursor-shape-v1.xml")
    mb_wayland_scan(tablet-v2
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/tablet/tablet-v2.xml")
    mb_wayland_scan(primary-selection-unstable-v1
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/unstable/primary-selection/primary-selection-unstable-v1.xml")
    mb_wayland_scan(viewporter
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/viewporter/viewporter.xml")
    mb_wayland_scan(fractional-scale-v1
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/staging/fractional-scale/fractional-scale-v1.xml")
    mb_wayland_scan(xdg-activation-v1
        "${WAYLAND_PROTOCOLS_PKGDATADIR}/staging/xdg-activation/xdg-activation-v1.xml")
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
