# kuli-sense — native host/process facts (ps / host). Win32 Toolhelp, Linux
# /proc, macOS sysctl behind #ifdef; no third-party dependencies.
add_library(kuli-sense STATIC src/sense.cpp)
add_library(kuli::sense ALIAS kuli-sense)

target_include_directories(kuli-sense PUBLIC include)
target_compile_features(kuli-sense PUBLIC cxx_std_23)
target_link_libraries(kuli-sense PRIVATE kuli::platform kuli-build-flags)
