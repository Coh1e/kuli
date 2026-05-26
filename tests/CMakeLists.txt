# kuli unit tests — doctest, one TU per leaf module. Each module also gets a
# named ctest entry (via --test-case filter) so `ctest -R <module>` works
# regardless of whether doctest's discover module is on CMAKE_MODULE_PATH.
find_package(doctest CONFIG REQUIRED)

add_executable(kuli-tests
    test_main.cpp
    test_crypto_hash.cpp
    test_luau_sandbox.cpp
    test_derivation_hash.cpp
    test_blueprint_eval.cpp
    test_archive.cpp
    test_store.cpp
    test_ir_validate.cpp
    test_engine_dryrun.cpp
    test_engine_realize.cpp
    test_generation.cpp
    test_rollback.cpp
    test_resolver.cpp
    test_source.cpp
    test_net_smoke.cpp)

target_link_libraries(kuli-tests PRIVATE
    doctest::doctest
    kuli::crypto
    kuli::diag
    kuli::platform
    kuli::luau
    kuli::store
    kuli::http
    kuli::ir
    kuli::engine
    kuli::bp
    kuli-build-flags)

if(NOT MSVC AND NOT APPLE)
    target_link_options(kuli-tests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Whole suite + per-module filters.
add_test(NAME unit COMMAND kuli-tests)
add_test(NAME crypto_hash COMMAND kuli-tests --test-case=crypto_hash*)
add_test(NAME luau_sandbox COMMAND kuli-tests --test-case=luau_sandbox*)
add_test(NAME archive COMMAND kuli-tests --test-case=archive*)
add_test(NAME store COMMAND kuli-tests --test-case=store*)
add_test(NAME derivation_hash COMMAND kuli-tests --test-case=derivation_hash*)
add_test(NAME blueprint_eval COMMAND kuli-tests --test-case=blueprint_eval*)
add_test(NAME ir_validate COMMAND kuli-tests --test-case=ir_validate*)
add_test(NAME engine_dryrun COMMAND kuli-tests --test-case=engine_dryrun*)
add_test(NAME engine_realize COMMAND kuli-tests --test-case=engine_realize*)
add_test(NAME generation COMMAND kuli-tests --test-case=generation*)
add_test(NAME rollback COMMAND kuli-tests --test-case=rollback*)
add_test(NAME resolver COMMAND kuli-tests --test-case=resolver*)
add_test(NAME source COMMAND kuli-tests --test-case=source*)
add_test(NAME net_smoke COMMAND kuli-tests --test-case=net_smoke*)
