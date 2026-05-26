# kuli-http — in-process libcurl GET. Absorbs luban download/libcurl_backend.
find_package(CURL CONFIG REQUIRED)

add_library(kuli-http STATIC
    src/download.cpp)
add_library(kuli::http ALIAS kuli-http)

target_include_directories(kuli-http PUBLIC include)
target_compile_features(kuli-http PUBLIC cxx_std_23)
target_link_libraries(kuli-http
    PUBLIC kuli::crypto kuli::diag
    PRIVATE CURL::libcurl kuli::platform kuli-build-flags)
