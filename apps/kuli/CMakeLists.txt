# kuli — the CLI binary. Front-ends link the engine ABI (design §2.4); in
# Phase A it only needs diag + platform + the generated version header.
add_executable(kuli main.cpp)

target_link_libraries(kuli PRIVATE
    kuli::diag
    kuli::platform
    kuli::bp
    CLI11::CLI11
    kuli-gen
    kuli-build-flags)

# Static link so kuli.exe runs on a fresh OS with no system runtime DLLs
# (R-NF-02). MSVC achieves this via CMAKE_MSVC_RUNTIME_LIBRARY; clang/gcc
# need explicit flags.
if(NOT MSVC)
    target_link_options(kuli PRIVATE -static -static-libgcc -static-libstdc++)
endif()
