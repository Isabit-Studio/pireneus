// The Vulkan backend (every platform but UWP) : fills a RendererBackend, see renderer_backend.h.

#ifndef _VULKAN_RENDERER_H_
#define _VULKAN_RENDERER_H_

#include "renderer/renderer_backend.h"

const RendererBackend* vulkan_get_backend(void);

#endif // _VULKAN_RENDERER_H_
