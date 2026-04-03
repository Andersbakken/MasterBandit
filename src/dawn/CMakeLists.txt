set(DAWN_SOURCES PlatformDawn.cpp Renderer.cpp NativeSurface.cpp)
if(APPLE)
    list(APPEND DAWN_SOURCES MetalLayer.mm)
endif()
add_library(platform_dawn STATIC ${DAWN_SOURCES})
target_link_libraries(platform_dawn PUBLIC
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
target_include_directories(platform_dawn PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(platform_dawn PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(APPLE)
    target_link_libraries(platform_dawn PUBLIC "-framework QuartzCore" "-framework Cocoa")
endif()
