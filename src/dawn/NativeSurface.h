#ifndef NATIVESURFACE_H
#define NATIVESURFACE_H

#include <dawn/webgpu_cpp.h>

struct GLFWwindow;

wgpu::Surface createNativeSurface(GLFWwindow* window, wgpu::Instance instance);

#endif /* NATIVESURFACE_H */
