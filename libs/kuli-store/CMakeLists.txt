# kuli-store — content-addressed store + archive extraction.
find_package(unofficial-minizip CONFIG QUIET)
find_package(miniz CONFIG QUIET)
find_package(unofficial-miniz CONFIG QUIET)

add_library(kuli-store STATIC
    src/archive.cpp
    src/store.cpp)
add_library(kuli::store ALIAS kuli-store)

target_include_directories(kuli-store PUBLIC include)
target_compile_features(kuli-store PUBLIC cxx_std_23)
target_link_libraries(kuli-store
    PUBLIC kuli::crypto kuli::diag
    PRIVATE kuli::http kuli::platform nlohmann_json::nlohmann_json kuli-build-flags)

# miniz target name varies by vcpkg port revision; link whichever exists.
if(TARGET unofficial::miniz::miniz)
    target_link_libraries(kuli-store PRIVATE unofficial::miniz::miniz)
elseif(TARGET miniz::miniz)
    target_link_libraries(kuli-store PRIVATE miniz::miniz)
else()
    message(FATAL_ERROR "miniz target not found (expected unofficial::miniz::miniz or miniz::miniz)")
endif()
