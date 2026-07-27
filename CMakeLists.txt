cmake_minimum_required(VERSION 3.25)

# Use packagename_ROOT for FindPackage.
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

# Disable default MSVC setting CRT type so we can set it ourselves.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Disable default MSVC warning level so we can set it ourselves.
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()

# Allow specifying MSVC debug configurations.
#if(POLICY CMP0141)
#    cmake_policy(SET CMP0141 NEW)
#endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

include(FeatureSummary)
include(CMakeDependentOption)

# We don't support in tree builds, so help people make the right choice.
if (CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Building in-source is not supported! Create a build dir and remove ${CMAKE_SOURCE_DIR}/CMakeCache.txt")
endif()

# Top level project, doesn't really affect anything.
project(genzh LANGUAGES C CXX)

# GeneralsX @build BenderAI 10/02/2026
# Linux port: Make CompatLib headers globally accessible so Core libraries (WWLib, WWMath, etc.)
# can access platform compatibility types when building for non-Windows platforms
if(UNIX)
    include_directories(${CMAKE_SOURCE_DIR}/Generals/Code/CompatLib/Include)
endif()

# Compiler cache (ccache) – must be included before compilers.cmake so the
# launcher is registered before CMake finalizes the compiler selection.
include(cmake/ccache.cmake)

# This file handles extra settings wanted/needed for different compilers.
include(cmake/compilers.cmake)

# Debug symbol stripping for Release builds (MinGW)
include(cmake/debug_strip.cmake)

include(FetchContent)

# MinGW-w64 specific configuration
if(MINGW)
    include(cmake/mingw.cmake)
    include(cmake/reactos-atl.cmake)
    include(cmake/widl.cmake)
endif()

# Find/Add build dependencies and stubs shared by all projects
# GeneralsX @bugfix Claude 27/07/2026 Fetch the Miles/Bink stubs on 64-bit Windows too.
#
# The gate used to also demand CMAKE_SIZEOF_VOID_P EQUAL 4, but every consumer of these two targets
# is gated on plain WIN32 with no word-size test: Core/Libraries/Source/WWVegas/CMakeLists.txt links
# milesstub into core_wwcommon, Core/GameEngineDevice links binkstub + milesstub, and both game
# Main/CMakeLists.txt list them under if(WIN32). At x64 the targets therefore did not exist while
# still being named, so mss.h was missing from the include path (Core/.../WWAudio/AudibleSound.h:42,
# 32 translation units) and CMake passed "milesstub"/"binkstub" through to the linker as bare
# library names.
#
# Nothing in either stub is 32-bit-only: both are C sources compiled from scratch (miles-sdk-stub
# miles.c + cleanup.c, bink-sdk-stub bink.c), the handles in mss.h are all pointer typedefs, and
# __stdcall simply degrades to the default convention at x64. The stdcall-decorated miles.def and
# bink_mingw.def are not referenced by the MSVC build - bink-sdk-stub's own CMakeLists already
# guards its .def on CMAKE_SIZEOF_VOID_P EQUAL 4.
#
# Guarded on Windows, so macOS / iOS / Linux never reach this.
if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    include(cmake/miles.cmake)
    include(cmake/bink.cmake)
endif()

# Always include DX8 headers (needed for both Windows and Linux builds)
# On Windows: min-dx8-sdk headers
# On Linux: DXVK headers (for D3D8→Vulkan translation)
include(cmake/dx8.cmake)

# SDL3 windowing/input for Linux builds (Phase 1)
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    include(cmake/sdl3.cmake)
endif()

# Define a dummy stlport target when not on VC6.
if (IS_VS6_BUILD)
    include(cmake/stlport.cmake)
else()
    add_library(stlport INTERFACE)
endif()

include(cmake/config.cmake)
include(cmake/gamespy.cmake)
include(cmake/lzhl.cmake)

# GeneralsX @bugfix fbraz 10/03/2026 Include openal.cmake here (before Core subdirectory)
# so OpenAL::OpenAL target is available when Core/GameEngineDevice/CMakeLists.txt processes.
# openal.cmake is self-guarded with if(SAGE_USE_OPENAL), so this is always safe to include.
include(cmake/openal.cmake)

# GeneralsX @feature BenderAI 21/04/2026 libcurl for in-game update checker (SAGE_UPDATE_CHECK builds)
# curl.cmake is self-guarded with if(SAGE_UPDATE_CHECK), so always safe to include.
include(cmake/curl.cmake)

# GeneralsX @feature fbraz 03/05/2026 Phase 4: Deterministic math library integration
# gamemath.cmake integrates fdlibm-based GameMath for cross-platform replay validation.
# Upstream reference: Okladnoj, PR #2670
# https://github.com/TheSuperHackers/GeneralsGameCode/pull/2670
# Self-guarded with if(SAGE_USE_DETERMINISTIC_MATH), so always safe to include.
include(cmake/gamemath.cmake)

if (IS_VS6_BUILD)
    # The original max sdk does not compile against a modern compiler.
    # If there is a desire to make this work, then a fixed max sdk needs to be created.
    add_subdirectory(Dependencies/MaxSDK)
endif()
add_subdirectory(Dependencies/Utility)

# To be removed when abandoning VC6
if (IS_VS6_BUILD)
    include_directories(Dependencies/Utility)
    add_compile_options(/FIUtility/CppMacros.h)
endif()

add_subdirectory(resources)

add_subdirectory(Core)

# GeneralsX @bugfix GitHubCopilot 14/04/2026 CompatLib must be configured on UNIX even when
# only Generals (not ZH) is built. It sets up d3d8lib INTERFACE includes for DXVK headers
# (windows_base.h) that core_wwcommon depends on transitively. When ZH is built,
# GeneralsMD/Code/CMakeLists.txt already includes CompatLib via add_subdirectory.
if(UNIX AND NOT RTS_BUILD_ZEROHOUR)
    add_subdirectory(GeneralsMD/Code/CompatLib)
endif()

# SagePatch (casual QoL extras: screenshot, cursor lock, brightness, etc.)
# Must be added before the game targets so sage_patch target exists when we
# declare the dependency below.
# GeneralsX @bugfix Claude 27/07/2026 Also require SDL3, instead of letting SagePatch FATAL_ERROR.
#
# SagePatch's own first statement is `if(NOT SAGE_USE_SDL3) message(FATAL_ERROR ...)`, and it hooks
# events through DYLD __interpose on macOS and LD_PRELOAD+dlsym on Linux - neither of which exists
# on Windows, where the engine uses the native Win32 device layer rather than SDL3. Since
# SAGE_USE_SDL3 defaults OFF (cmake/config-build.cmake:14) and no Windows preset turns it on, a
# Windows configure hit that FATAL_ERROR and stopped before reaching any real target.
#
# Guarding here rather than relaxing SagePatch's own check: the check is correct, SagePatch really
# does need SDL3. What was wrong is offering it on a configuration that cannot satisfy that.
if(RTS_BUILD_OPTION_SAGE_PATCH AND SAGE_USE_SDL3)
    add_subdirectory(Patches/SagePatch)
elseif(RTS_BUILD_OPTION_SAGE_PATCH)
    message(STATUS "SagePatch skipped: needs SAGE_USE_SDL3=ON (not available on this configuration)")
endif()

# Add main build targets
if(RTS_BUILD_ZEROHOUR)
    add_subdirectory(GeneralsMD)
    if(TARGET sage_patch)
        add_dependencies(z_generals sage_patch)
    endif()
endif()

if(RTS_BUILD_GENERALS)
    add_subdirectory(Generals)
    if(TARGET sage_patch)
        add_dependencies(g_generals sage_patch)
    endif()
endif()

feature_summary(WHAT ENABLED_FEATURES DESCRIPTION "Enabled features:")
feature_summary(WHAT DISABLED_FEATURES DESCRIPTION "Disabled features:")
