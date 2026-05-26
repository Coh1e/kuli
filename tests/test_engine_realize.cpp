#include <doctest/doctest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "kuli/crypto/hash.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/store/archive.hpp"
#include "kuli/store/store.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-realize-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
}  // namespace

// Exercises the executor's realize path hermetically by pre-populating the
// store so realize_fetch is a cache hit (no network). Shim/PATH projection is
// the activation layer's job (covered by test_rollback).
TEST_CASE("engine_realize: fetch derivation is realized into the store (C6)") {
    fs::path dir = scratch();
    fs::path zip = dir / "demo.zip";
    REQUIRE(archive::create_zip(zip, {
        {"demo-1.0/bin/demo.exe", "MZ-fake-exe"},
        {"demo-1.0/README", "hi"},
    }).has_value());
    auto sha = crypto::hash_file(zip);
    REQUIRE(sha.has_value());

    const std::string hash16 = "abcdef0123456789";
    const std::string full_hash = hash16 + std::string(48, '0');
    const std::string name = "demo";

    // Pre-realize into a temp store so the engine's realize_fetch short-circuits.
    store::Store st{dir / "store", dir / "dl"};
    REQUIRE(st.realize_from_archive(hash16, name, zip, *sha, "bin/demo.exe").has_value());

    // Build an ApplyDerivation IR for a single fetch derivation.
    nlohmann::json ir{
        {"schema", "kuli/ir/1.0"},
        {"kind", "ApplyDerivation"},
        {"options", {{"dry_run", false}}},
        {"node",
         {{"root", full_hash},
          {"derivations",
           nlohmann::json::array({nlohmann::json{
               {"hash", full_hash},
               {"name", name},
               {"storePath", hash16 + "-" + name},
               {"builder", "fetch"},
               {"systemTarget", "windows-x64"},
               {"fetch",
                {{"owner", "o"}, {"repo", "demo"}, {"version", "1.0"},
                 {"assetPattern", "demo"}, {"sha256", sha->hex},
                 {"url", "file:///unused-cache-hit"}, {"bin", "bin/demo.exe"}}}}})}}},
        {"descent_trace", nlohmann::json::array()},
        {"sources", nlohmann::json::array()},
    };

    // Injected exec env: temp store only (no HKCU, no real store).
    engine::ExecEnv env;
    env.store_root = dir / "store";
    env.downloads = dir / "dl";

    engine::Engine eng(env);
    engine::AdapterCall call;
    call.tool_name = "kuli-bp";
    call.cwd = dir;
    call.ir_doc = ir;

    engine::RawResult r = eng.execute(call);

    CHECK(r.exit_code == 0);
    // Store path realized (pre-populated, so this is a verified cache hit).
    CHECK(fs::exists(env.store_root / (hash16 + "-" + name) / "bin" / "demo.exe"));

    fs::remove_all(dir);
}
