#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "kuli/bp/generation.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/platform/shim.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-rb-" + std::to_string(rng()));
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
luau::DerivationGraph ninja_graph() {
    return eval_src(R"(return function(ctx)
        return ctx.lib.fetchGitHubRelease{ name="ninja", owner="ninja-build",
            repo="ninja", version="1", assetPattern="win",
            sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", bin="ninja.exe" }
    end)");
}
luau::DerivationGraph fd_graph() {
    return eval_src(R"(return function(ctx)
        return ctx.lib.fetchGitHubRelease{ name="fd", owner="sharkdp",
            repo="fd", version="1", assetPattern="win",
            sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", bin="fd.exe" }
    end)");
}
#if defined(_WIN32)
const char* kExt = ".cmd";
#else
const char* kExt = "";
#endif
bool shim_exists(const fs::path& bin, const std::string& alias) {
    return fs::exists(bin / (alias + kExt));
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

// C3: rollback re-points PATH shims at the prior generation's store paths and
// removes shims the prior generation didn't have.
TEST_CASE("rollback re-projects shims onto the target generation (C3)") {
    fs::path root = scratch();
    fs::path bin = root / "bin";
    fs::path store = root / "store";

    bp::Profile p{root / "profiles", "default"};

    // Generation 1: ninja.
    p.commit(ninja_graph());
    REQUIRE(p.activate(1, bin, store));
    CHECK(shim_exists(bin, "ninja"));
    CHECK_FALSE(shim_exists(bin, "fd"));

    // Generation 2: fd (replaces ninja).
    p.commit(fd_graph());
    REQUIRE(p.activate(2, bin, store));
    CHECK(shim_exists(bin, "fd"));
    CHECK_FALSE(shim_exists(bin, "ninja"));  // ninja shim removed on switch

    // Rollback to 1: ninja shim returns, fd shim removed, and the shim points
    // at generation 1's store path.
    REQUIRE(p.activate(1, bin, store));
    CHECK(p.current_id() == 1);
    CHECK(shim_exists(bin, "ninja"));
    CHECK_FALSE(shim_exists(bin, "fd"));
    std::string ninja_shim = read(bin / (std::string("ninja") + kExt));
    auto g1 = p.load(1);
    REQUIRE(g1.has_value());
    CHECK(ninja_shim.find(g1->derivations.at(0).store_path) != std::string::npos);

    fs::remove_all(root);
}

// P-I-4: a composite installs each child's shim (the composite node itself
// needs no merged store dir in kuli's shim model).
TEST_CASE("rollback: composite installs both children's shims") {
    fs::path root = scratch();
    fs::path bin = root / "bin";
    fs::path store = root / "store";
    auto g = eval_src(
        std::string("return function(ctx)\n"
                    "  local rg = ctx.lib.fetchGitHubRelease{ name='ripgrep', owner='o',"
                    " repo='ripgrep', version='1', assetPattern='x', sha256='") +
        std::string(64, 'a') +
        "', bin='rg.exe' }\n"
        "  local fd = ctx.lib.fetchGitHubRelease{ name='fd', owner='o', repo='fd', version='1',"
        " assetPattern='x', sha256='" + std::string(64, 'b') +
        "', bin='fd.exe' }\n"
        "  return ctx.lib.composite{ name='tools', components={ rg, fd } }\n"
        "end");

    bp::Profile p{root / "profiles", "default"};
    p.commit(g);
    REQUIRE(p.activate(1, bin, store));
    CHECK(shim_exists(bin, "rg"));   // child shim (bin stem of rg.exe)
    CHECK(shim_exists(bin, "fd"));   // child shim
    CHECK_FALSE(shim_exists(bin, "tools"));  // composite node has no shim
    fs::remove_all(root);
}

// P-I-2: withFiles deploys a config file into the profile on activate, removes
// it on switch-away, restores a pre-existing file's backup, and re-deploys on
// rollback.
TEST_CASE("rollback: withFiles deploys a config file and reverts it") {
    fs::path root = scratch();
    fs::path bin = root / "bin";
    fs::path store = root / "store";
    fs::path cfg = root / "cfg.txt";
    std::string cfgp = cfg.generic_string();  // forward slashes for the Luau literal

    // A pre-existing user file at the target — must be backed up + restored.
    { std::ofstream(cfg, std::ios::binary) << "ORIGINAL"; }

    auto g_withfiles = eval_src(
        std::string("return function(ctx)\n"
                    "  local base = ctx.lib.fetchGitHubRelease{ name='t', owner='o', repo='r',"
                    " version='1', assetPattern='p', sha256='") +
        std::string(64, 'a') +
        "' }\n"
        "  return ctx.lib.withFiles(base, { ['" + cfgp +
        "'] = { mode='replace', content='HELLO' } })\n"
        "end");

    bp::Profile p{root / "profiles", "default"};
    p.commit(g_withfiles);                 // gen 1: deploys cfg
    REQUIRE(p.activate(1, bin, store));
    CHECK(fs::exists(cfg));
    CHECK(read(cfg) == "HELLO");

    p.commit(ninja_graph());               // gen 2: no files
    REQUIRE(p.activate(2, bin, store));
    CHECK(read(cfg) == "ORIGINAL");        // switch-away restored the backup

    REQUIRE(p.activate(1, bin, store));    // rollback to gen 1
    CHECK(read(cfg) == "HELLO");           // re-deployed

    fs::remove_all(root);
}

// M-2: a shim write that can't succeed must report false (so apply surfaces a
// partial activation instead of a false "switched to generation N").
TEST_CASE("rollback: write_shim reports failure on an unwritable bin dir (M-2)") {
    fs::path root = scratch();
    fs::path bin_is_a_file = root / "binfile";
    { std::ofstream(bin_is_a_file) << "x"; }  // the "bin dir" is actually a file
    CHECK_FALSE(kuli::platform::write_shim(bin_is_a_file, "tool",
                                           fs::path("C:/store/tool/tool.exe")));
    fs::remove_all(root);
}
