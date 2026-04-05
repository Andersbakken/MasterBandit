set(DAWN_SOURCES
    PlatformDawn.cpp
    EventLoop.cpp
    Input.cpp
    Actions.cpp
    Render.cpp
    Tabs.cpp
    TabBar.cpp
    Debug.cpp
    Renderer.cpp
    NativeSurface.cpp
    TexturePool.cpp
    ComputeStatePool.cpp
)
if(APPLE)
    list(APPEND DAWN_SOURCES MetalLayer.mm PlatformUtils_macOS.mm ../FontFallback_CoreText.mm ../FontResolver_CoreText.mm)
else()
    list(APPEND DAWN_SOURCES PlatformUtils_Linux.cpp ../FontFallback_FontConfig.cpp ../FontResolver_FontConfig.cpp)
endif()
add_library(platform_impl STATIC ${DAWN_SOURCES})
target_link_libraries(platform_impl PUBLIC
    dawn::webgpu_dawn
    glfw
    $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,uv>
    harfbuzz
    harfbuzz-gpu

    spdlog::spdlog
    SheenBidi::SheenBidi
    libunibreak::libunibreak
    websockets
    glaze::glaze
)
target_include_directories(platform_impl PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(platform_impl PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/terminal)

if(APPLE)
    target_link_libraries(platform_impl PUBLIC "-framework QuartzCore" "-framework Cocoa" "-framework CoreText" "-framework UserNotifications")
else()
    find_package(Fontconfig REQUIRED)
    target_link_libraries(platform_impl PUBLIC Fontconfig::Fontconfig)
endif()
