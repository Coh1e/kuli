#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "kuli/bp/apply.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/luau/frontend.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-engine-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}

luau::DerivationGraph eval_two_tools() {
    luau::EvalRequest req;
    req.inline_source = R"(
        return function(ctx)
            local rg = ctx.lib.fetchGitHubRelease{
                name="ripgrep", owner="o", repo="ripgrep", version="1",
                assetPattern="x",
                sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", bin="rg.exe" }
            return ctx.lib.composite{ name = "tools", components = { rg } }
        end)";
    req.chunk_name = "t";
    req.system = luau::SystemInfo{"windows", "x64", "11"};
    return luau::evaluate(req).value().graph;
}

std::size_t count_files(const fs::path& root) {
    std::size_t n = 0;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file()) ++n;
    }
    return n;
}
}  // namespace

TEST_CASE("engine_dryrun: validates, writes only the evidence session (C5)") {
    auto graph = eval_two_tools();
    nlohmann::json ir = bp::to_apply_ir(graph, /*dry_run=*/true);

    fs::path cwd = scratch();
    engine::Engine eng;
    engine::AdapterCall call;
    call.tool_name = "kuli-bp";
    call.cwd = cwd;
    call.ir_doc = ir;

    engine::RawResult r = eng.execute(call);

    CHECK(r.exit_code == 0);
    CHECK_FALSE(r.session_id.empty());

    fs::path sdir = cwd / ".kuli" / "sessions" / r.session_id;
    CHECK(fs::exists(sdir / "input.json"));
    CHECK(fs::exists(sdir / "ir.json"));
    CHECK(fs::exists(sdir / "plan.json"));
    CHECK(fs::exists(sdir / "summary.md"));

    // Nothing was written outside the session: every file under cwd lives in
    // the session directory.
    CHECK(count_files(cwd) == count_files(sdir));

    fs::remove_all(cwd);
}

TEST_CASE("engine_dryrun: malformed IR fails with exit 2") {
    nlohmann::json bad{{"schema", "kuli/ir/1.0"}, {"kind", "ApplyDerivation"},
                       {"node", {{"root", "x"}, {"derivations", nlohmann::json::array()}}}};
    fs::path cwd = scratch();
    engine::Engine eng;
    engine::AdapterCall call{"kuli-bp", {}, cwd, bad};
    engine::RawResult r = eng.execute(call);
    CHECK(r.exit_code == 2);  // root not among derivations
    fs::remove_all(cwd);
}
