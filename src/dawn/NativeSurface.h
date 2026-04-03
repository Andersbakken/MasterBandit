#pragma once

#include <dawn/webgpu_cpp.h>

struct GLFWwindow;

wgpu::Surface createNativeSurface(GLFWwindow* window, wgpu::Instance instance);
