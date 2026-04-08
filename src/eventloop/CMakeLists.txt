set(EVENTLOOP_SOURCES
    # Platform-independent sources will go here as Input.cpp / Actions.cpp are migrated
)

if(APPLE)
    list(APPEND EVENTLOOP_SOURCES
        mac/EventLoop_nsapp.mm
        mac/Window_cocoa.mm
        bsd/EventLoop_kqueue.cpp
    )
else()
    list(APPEND EVENTLOOP_SOURCES
        linux/EventLoop_epoll.cpp
        linux/Window_xcb.cpp
    )
endif()

add_library(eventloop OBJECT ${EVENTLOOP_SOURCES})

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
    pkg_check_modules(XCB REQUIRED xcb xcb-util)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon xkbcommon-x11)
    pkg_check_modules(X11 REQUIRED x11 x11-xcb)

    target_include_directories(eventloop PRIVATE
        ${XCB_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
    )
    target_link_libraries(eventloop PUBLIC
        ${XCB_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${X11_LIBRARIES}
    )
endif()
