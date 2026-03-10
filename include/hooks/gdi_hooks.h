#pragma once

#include <Windows.h>

namespace puretype
{
    namespace hooks
    {
        bool InstallGDIHooks();

        void RemoveGDIHooks();
    }
}