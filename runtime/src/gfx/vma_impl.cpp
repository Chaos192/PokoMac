// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The one translation unit that holds the VulkanMemoryAllocator implementation (MIT, pinned in cmake/gfx_deps.cmake).
// The Vulkan entry points are loaded by volk and handed to VMA through VmaVulkanFunctions (see vk_device.cpp).
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
