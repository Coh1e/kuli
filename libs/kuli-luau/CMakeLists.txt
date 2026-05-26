# kuli-luau — the sole Luau binding. Luau is vendored via FetchContent (not
# vcpkg, per design §3) and pinned to a release tag. Only this target sees the
# Luau headers (Luau.VM/Compiler/Ast linked PRIVATE); the public API is plain
# C++ (R-NF-03).
include(FetchContent)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.722
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(luau)

add_library(kuli-luau STATIC
    src/luau_frontend.cpp
    src/canonical_json.cpp)
add_library(kuli::luau ALIAS kuli-luau)

target_include_directories(kuli-luau PUBLIC include)
target_compile_features(kuli-luau PUBLIC cxx_std_23)
target_link_libraries(kuli-luau
    PUBLIC kuli::diag
    PRIVATE Luau.VM Luau.Compiler Luau.Ast kuli::crypto
            nlohmann_json::nlohmann_json kuli-build-flags)
