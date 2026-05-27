# kuli-bp — blueprint orchestration: eval (kuli-luau) -> ApplyDerivation IR ->
# engine. Profile/generation/lock/source-management land in later phases.
add_library(kuli-bp STATIC
    src/adapter.cpp
    src/apply.cpp
    src/resolver.cpp
    src/generation.cpp
    src/gen_cmd.cpp
    src/hosts.cpp
    src/meshell.cpp
    src/scripture.cpp
    src/source.cpp)
add_library(kuli::bp ALIAS kuli-bp)

target_include_directories(kuli-bp PUBLIC include)
target_compile_features(kuli-bp PUBLIC cxx_std_23)
target_link_libraries(kuli-bp
    PUBLIC kuli::luau nlohmann_json::nlohmann_json
    PRIVATE kuli::engine kuli::ir kuli::diag kuli::platform kuli::http
            kuli::store kuli::crypto kuli-build-flags)
