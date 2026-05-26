# kuli-engine — the ABI boundary. Front-ends link only this. It owns the
# built-in IR executor and evidence sessions.
add_library(kuli-engine STATIC
    src/engine.cpp)
add_library(kuli::engine ALIAS kuli-engine)

target_include_directories(kuli-engine PUBLIC include)
target_compile_features(kuli-engine PUBLIC cxx_std_23)
target_link_libraries(kuli-engine
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE kuli::ir kuli::diag kuli::store kuli::platform kuli-build-flags)
