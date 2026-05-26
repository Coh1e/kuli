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

# Statically link the C++ runtime for portability. Full `-static` (incl. libc)
# is the Windows /MT story (handled by the x64-windows-static triplet); on Linux
# we keep libstdc++/libgcc static but libc dynamic (full-static + OpenSSL is
# fragile), and macOS has no static libc, so we add nothing there.
if(NOT MSVC AND NOT APPLE)
    target_link_options(kuli PRIVATE -static-libgcc -static-libstdc++)
endif()
