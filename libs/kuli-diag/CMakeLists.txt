# kuli-diag — rustc-style diagnostics + exit-code map. Leaf module.
add_library(kuli-diag STATIC
    src/diagnostic.cpp)
add_library(kuli::diag ALIAS kuli-diag)

target_include_directories(kuli-diag PUBLIC include)
target_compile_features(kuli-diag PUBLIC cxx_std_23)
target_link_libraries(kuli-diag PRIVATE kuli-build-flags)
