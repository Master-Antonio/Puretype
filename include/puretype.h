#pragma once

#include <atomic>

#define PURETYPE_VERSION_MAJOR 0
#define PURETYPE_VERSION_MINOR 2
#define PURETYPE_VERSION_PATCH 0

#ifdef PURETYPE_EXPORTS
#define PURETYPE_API __declspec(dllexport)
#else
#define PURETYPE_API __declspec(dllimport)
#endif

namespace puretype
{
    class Config;
    class FTRasterizer;
    class SubpixelFilter;
    class Blender;

    // Global hook reference count for safe DLL shutdown.
    // Hooks increment on entry, decrement on exit.
    // DLL_PROCESS_DETACH spin-waits until this reaches zero before destroying the rasterizer.
    extern std::atomic<int> g_activeHookCount;
}
