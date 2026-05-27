#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "kuli/bp/generation.hpp"
#include "kuli/luau/frontend.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-gen-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}

luau::DerivationGraph eval_src(const std::string& src) {
    luau::EvalRequest req;
    req.inline_source = src;
    req.chunk_name = "t";
    req.system = luau::SystemInfo{"windows", "x64", "11"};
    return luau::evaluate(req).value().graph;
}

luau::DerivationGraph graph_one() {
    return eval_src(R"(return function(ctx)
        return ctx.lib.fetchGitHubRelease{ name="ninja", owner="ninja-build",
            repo="ninja", version="1.12.1", assetPattern="win",
            sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", bin="ninja.exe" }
    end)");
}
luau::DerivationGraph graph_two() {
    return eval_src(R"(return function(ctx)
        local n = ctx.lib.fetchGitHubRelease{ name="ninja", owner="ninja-build",
            repo="ninja", version="1.12.1", assetPattern="win",
            sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", bin="ninja.exe" }
        local c = ctx.lib.fetchGitHubRelease{ name="cmake", owner="Kitware",
            repo="CMake", version="3.30", assetPattern="win",
            sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", bin="bin/cmake.exe" }
        return ctx.lib.composite{ name="dev", components={ n, c } }
    end)");
}
}  // namespace

TEST_CASE("generation set_hash is stable for the same derivation set") {
    CHECK(bp::derivation_set_hash(graph_one()) == bp::derivation_set_hash(graph_one()));
    CHECK(bp::derivation_set_hash(graph_one()) != bp::derivation_set_hash(graph_two()));
}

TEST_CASE("generation set_hash distinguishes the root (M-4)") {
    luau::DerivationGraph g;
    luau::Derivation a;
    a.hash = std::string(64, 'a');
    a.name = "a";
    luau::Derivation b;
    b.hash = std::string(64, 'b');
    b.name = "b";
    g.nodes[a.hash] = a;
    g.nodes[b.hash] = b;

    g.root = a.hash;
    std::string with_root_a = bp::derivation_set_hash(g);
    g.root = b.hash;
    std::string with_root_b = bp::derivation_set_hash(g);
    CHECK(with_root_a != with_root_b);  // same node set, different root => distinct
}

TEST_CASE("generation commit appends and points current (C1/C2 model)") {
    fs::path root = scratch();
    bp::Profile p{root, "default"};
    CHECK(p.current_id() == 0);

    auto g1 = graph_one();
    bp::Generation gen1 = p.commit(g1);  // appends; current advances via set_current/activate
    CHECK(gen1.id == 1);
    CHECK(gen1.parent == 0);
    p.set_current(1);
    CHECK(p.current_id() == 1);
    auto loaded = p.load(1);  // bind to a named value — don't iterate a temporary's member
    REQUIRE(loaded.has_value());
    CHECK(loaded->set_hash == bp::derivation_set_hash(g1));
    // the ninja fetch contributed a shim entry
    bool has_ninja_shim = false;
    for (const auto& d : loaded->derivations)
        for (const auto& s : d.shims)
            if (s.alias == "ninja") has_ninja_shim = true;
    CHECK(has_ninja_shim);

    // C1: re-applying the same set is detected as no-op by the caller.
    CHECK(p.current()->set_hash == bp::derivation_set_hash(graph_one()));

    // C2: a different set commits a new generation, chained to the previous.
    bp::Generation gen2 = p.commit(graph_two());
    CHECK(gen2.id == 2);
    CHECK(gen2.parent == 1);
    p.set_current(2);
    CHECK(p.current_id() == 2);
    CHECK(p.list() == std::vector<int>{1, 2});

    fs::remove_all(root);
}

TEST_CASE("generation set_current re-points (rollback primitive)") {
    fs::path root = scratch();
    bp::Profile p{root, "default"};
    p.commit(graph_one());
    p.commit(graph_two());
    p.set_current(2);
    CHECK(p.current_id() == 2);
    CHECK(p.set_current(1));
    CHECK(p.current_id() == 1);
    CHECK_FALSE(p.set_current(99));  // missing generation
    CHECK(p.current_id() == 1);

    fs::remove_all(root);
}
