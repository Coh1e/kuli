# kuli-platform — host detection + XDG paths. The ONLY place <windows.h> is
# allowed (R-NF-05 platform isolation). Public headers are platform-neutral.
add_library(kuli-platform STATIC
    src/host.cpp
    src/paths.cpp
    src/env.cpp
    src/win_env.cpp
    src/shim.cpp)
add_library(kuli::platform ALIAS kuli-platform)

target_include_directories(kuli-platform PUBLIC include)
target_compile_features(kuli-platform PUBLIC cxx_std_23)
target_link_libraries(kuli-platform PRIVATE kuli-build-flags)

if(WIN32)
    target_link_libraries(kuli-platform PRIVATE shell32 ole32 advapi32 user32)
endif()
