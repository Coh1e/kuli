# kuli-ir — kuli/ir/1.0 envelope + validator. nlohmann-json only; no platform,
# no Luau (it is below both).
add_library(kuli-ir STATIC
    src/validate.cpp)
add_library(kuli::ir ALIAS kuli-ir)

target_include_directories(kuli-ir PUBLIC include)
target_compile_features(kuli-ir PUBLIC cxx_std_23)
target_link_libraries(kuli-ir
    PUBLIC kuli::diag nlohmann_json::nlohmann_json
    PRIVATE kuli-build-flags)
