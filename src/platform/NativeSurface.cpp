// Isolated compilation unit for native window surface creation.
// X11/Cocoa headers pollute the global namespace with macros (Bool, Success, etc.)
// so we keep them quarantined here.

#include "NativeSurface.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef __linux__
#  define GLFW_EXPOSE_NATIVE_X11
#endif
#ifdef __APPLE__
#  define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

// X11 defines Success as 0, which conflicts with Dawn enums
#ifdef Success
#  undef Success
#endif

#ifdef __APPLE__
extern "C" void* createMetalLayer(void* nsWindow);
#endif

wgpu::Surface createNativeSurface(GLFWwindow* window, wgpu::Instance instance)
{
#ifdef __linux__
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        Display* display = glfwGetX11Display();
        ::Window x11Window = glfwGetX11Window(window);

        wgpu::SurfaceSourceXlibWindow x11Source;
        x11Source.display = display;
        x11Source.window = static_cast<uint64_t>(x11Window);

        wgpu::SurfaceDescriptor surfDesc;
        surfDesc.nextInChain = &x11Source;
        return instance.CreateSurface(&surfDesc);
    }
#endif

#ifdef __APPLE__
    {
        void* metalLayer = createMetalLayer(glfwGetCocoaWindow(window));
        if (metalLayer) {
            wgpu::SurfaceSourceMetalLayer metalSource;
            metalSource.layer = metalLayer;

            wgpu::SurfaceDescriptor surfDesc;
            surfDesc.nextInChain = &metalSource;
            return instance.CreateSurface(&surfDesc);
        }
    }
#endif

    return nullptr;
}
