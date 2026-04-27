set(PLATFORM_SOURCES
    PlatformDawn.cpp
    Platform_EventLoop.cpp
    Platform_Actions.cpp
    Platform_Tabs.cpp
    Platform_Debug.cpp
    ActionRouter.cpp
    AnimationScheduler.cpp
    ConfigLoader.cpp
    InputController.cpp
    RenderEngine.cpp
    RenderThread.cpp
    Renderer.cpp
    TexturePool.cpp
    ComputeStatePool.cpp
)
if(APPLE)
    list(APPEND PLATFORM_SOURCES PlatformUtils_macOS.mm ../FontFallback_CoreText.mm ../FontResolver_CoreText.mm)
else()
    list(APPEND PLATFORM_SOURCES PlatformUtils_Linux.cpp DBusBridge.cpp ../FontFallback_FontConfig.cpp ../FontResolver_FontConfig.cpp)
endif()

add_library(platform OBJECT ${PLATFORM_SOURCES})
mb_enable_arc_on_mm(PLATFORM_SOURCES)

target_include_directories(platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/terminal
    ${CMAKE_SOURCE_DIR}/src/eventloop
    ${CMAKE_SOURCE_DIR}/src/script
)

target_include_directories(platform PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(platform PUBLIC
    dawn::webgpu_dawn
    harfbuzz
    harfbuzz-gpu
    spdlog::spdlog
    SheenBidi::SheenBidi
    libunibreak::libunibreak
    websockets
    glaze::glaze
)

if(APPLE)
    target_link_libraries(platform PUBLIC "-framework QuartzCore" "-framework Cocoa" "-framework CoreText" "-framework UserNotifications")
else()
    find_package(Fontconfig REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED dbus-1)
    target_include_directories(platform PUBLIC ${DBUS_INCLUDE_DIRS})
    target_link_libraries(platform PUBLIC Fontconfig::Fontconfig ${DBUS_LIBRARIES})
endif()
