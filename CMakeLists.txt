cmake_minimum_required(VERSION 3.14)
project(PureType VERSION 0.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- Dipendenze Esterne ---
FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG master
)
FetchContent_MakeAvailable(minhook)

FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
        GIT_TAG VER-2-13-2
)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype)

# --- Target Libreria (DLL) ---
add_library(PureType SHARED
        src/dllmain.cpp
        src/config.cpp
        src/rasterizer/ft_rasterizer.cpp
        src/filters/subpixel_filter.cpp
        src/filters/tone_mapper.cpp
        src/output/blender.cpp
        src/hooks/dwrite_hooks.cpp
        src/hooks/gdi_hooks.cpp
)

target_include_directories(PureType PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(PureType PRIVATE
        minhook
        freetype
        d2d1.lib
        dwrite.lib
        gdi32.lib
        user32.lib
        msimg32.lib
        windowscodecs.lib
)

target_compile_definitions(PureType PRIVATE
        PURETYPE_EXPORTS
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_compile_options(PureType PRIVATE /W4 /WX-)

# ── Main Executable (System Tray Application with Icon) ──────────────────────
add_executable(puretype_exe WIN32
        tools/injector.cpp
        src/puretype.rc
)
set_target_properties(puretype_exe PROPERTIES OUTPUT_NAME "puretype")

target_compile_definitions(puretype_exe PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(puretype_exe PRIVATE
        user32.lib
        shell32.lib
        advapi32.lib
)

# --- Operazioni Post-Build ---
add_custom_command(TARGET PureType POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/puretype.ini
        $<TARGET_FILE_DIR:PureType>/puretype.ini
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/src/puretype.ico
        $<TARGET_FILE_DIR:PureType>/puretype.ico
        COMMENT "Copying puretype.ini and puretype.ico to output directory"
)