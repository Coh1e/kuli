# kuli — top-level build.
#
# Invariants this file guards (docs/requirements.md §5):
#   R-NF-02  core is statically linked: /MT on MSVC (via x64-windows-static
#            triplet + CMAKE_MSVC_RUNTIME_LIBRARY) so a fresh OS unzip runs
#            kuli.exe with no system runtime DLLs. Linux statically links
#            libstdc++/libgcc; macOS links the system libs (no static libc).
#   R-NF-05  cross-platform day-1: this file MUST configure on Linux/macOS
#            too (no `if(NOT WIN32) FATAL_ERROR`). Win32 headers are confined
#            to libs/kuli-platform's win TUs; everything else is portable.
#   R-NF-03  Luau headers reach ONLY kuli-luau (added in Phase B); no other
#            target sees luau.h.
cmake_minimum_required(VERSION 3.25)
project(kuli
    VERSION 0.1.0
    DESCRIPTION "Fuse multi-source / multi-machine capability into a peer mesh"
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static C runtime on MSVC (R-NF-02). The x64-windows-static vcpkg triplet
# (set in CMakePresets) builds the deps /MT to match.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Single source of truth for the version: edit project(VERSION ...) above,
# rebuild — every TU reads the generated header.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_BINARY_DIR}/gen/kuli/version.hpp"
    @ONLY)

# Shared warning/flag bundle. Linked PRIVATE by every kuli target so our own
# code stays loud while vendored sources can opt out.
add_library(kuli-build-flags INTERFACE)
target_compile_options(kuli-build-flags INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
if(MSVC)
    target_compile_definitions(kuli-build-flags INTERFACE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Carries the configure_file'd version.hpp to whoever needs it.
add_library(kuli-gen INTERFACE)
target_include_directories(kuli-gen INTERFACE "${CMAKE_BINARY_DIR}/gen")

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

enable_testing()

add_subdirectory(libs/kuli-diag)
add_subdirectory(libs/kuli-crypto)
add_subdirectory(libs/kuli-platform)
add_subdirectory(libs/kuli-http)
add_subdirectory(libs/kuli-store)
add_subdirectory(libs/kuli-ir)
add_subdirectory(libs/kuli-luau)
add_subdirectory(libs/kuli-engine)
add_subdirectory(libs/kuli-bp)
add_subdirectory(apps/kuli)
add_subdirectory(tests)
