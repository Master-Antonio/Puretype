#pragma once

#define PURETYPE_VERSION_MAJOR 0
#define PURETYPE_VERSION_MINOR 1
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
}