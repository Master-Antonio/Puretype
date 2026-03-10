#pragma once

#include <dwrite.h>
#include <d2d1.h>

namespace puretype
{
    namespace hooks
    {
        bool InstallDWriteHooks();

        void RemoveDWriteHooks();
    }
}