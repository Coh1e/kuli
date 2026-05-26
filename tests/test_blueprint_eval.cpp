#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "kuli/luau/frontend.hpp"

namespace fs = std::filesystem;
using namespace kuli::luau;

namespace {
EvalRequest inline_req(std::string src) {
    EvalRequest r;
    r.inline_source = std::move(src);
    r.chunk_name = "test";
    r.system = SystemInfo{"windows", "x64", "11"};
    return r;
}
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-bp-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
void write(const fs::path& p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
}  // namespace

TEST_CASE("blueprint_eval: fetch produces a content-addressed derivation") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            return ctx.lib.fetchGitHubRelease{
                name = "ripgrep", owner = "BurntSushi", repo = "ripgrep",
                version = "14.1.0", assetPattern = "msvc",
                sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                bin = "rg.exe",
            }
        end)"));
    REQUIRE(r.has_value());
    const auto* root = r->root();
    REQUIRE(root != nullptr);
    CHECK(root->builder == Builder::Fetch);
    CHECK(root->name == "ripgrep");
    CHECK(root->fetch.owner == "BurntSushi");
    CHECK(root->fetch.bin == "rg.exe");
    CHECK(root->store_path == root->hash.substr(0, 16) + "-ripgrep");
    CHECK(r->graph.nodes.size() == 1);
}

TEST_CASE("blueprint_eval: missing required field fails with a usage error") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            return ctx.lib.fetchGitHubRelease{ name = "x" }  -- no owner/repo/...
        end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("owner") != std::string::npos);
}

TEST_CASE("blueprint_eval: malformed sha256 fails eval (M-3)") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            return ctx.lib.fetchGitHubRelease{
                name="x", owner="o", repo="r", version="1",
                assetPattern="p", sha256="not-a-real-sha" }
        end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("sha256") != std::string::npos);
}

TEST_CASE("blueprint_eval: uppercase sha is normalized (no hash fork, M-3)") {
    auto mk = [](const char* sha) {
        EvalRequest q = inline_req(std::string(R"(
            return function(ctx)
                return ctx.lib.fetchGitHubRelease{
                    name="x", owner="o", repo="r", version="1",
                    assetPattern="p", sha256=")") + sha + "\" } end");
        return evaluate(q);
    };
    auto lo = mk(std::string(64, 'a').c_str());
    auto up = mk(std::string(64, 'A').c_str());
    REQUIRE(lo.has_value());
    REQUIRE(up.has_value());
    CHECK(lo->graph.root == up->graph.root);  // case-insensitive -> same hash
}

TEST_CASE("blueprint_eval: composite builds a tree with children") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            local rg = ctx.lib.fetchGitHubRelease{
                name="ripgrep", owner="o", repo="ripgrep", version="1",
                assetPattern="x", sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }
            local fd = ctx.lib.fetchGitHubRelease{
                name="fd", owner="o", repo="fd", version="1",
                assetPattern="x", sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" }
            return ctx.lib.composite{ name = "tools", components = { rg, fd } }
        end)"));
    REQUIRE(r.has_value());
    const auto* root = r->root();
    REQUIRE(root != nullptr);
    CHECK(root->builder == Builder::Composite);
    CHECK(root->components.size() == 2);
    CHECK(r->graph.nodes.size() == 3);  // composite + 2 fetches
}

TEST_CASE("blueprint_eval: ctx.pkgs lazily resolves a sibling blueprint") {
    fs::path root = scratch();
    write(root / "blueprints" / "mingit.luau", R"(
        return function(ctx)
            return ctx.lib.fetchGitHubRelease{
                name="mingit", owner="git-for-windows", repo="git",
                version="2.45", assetPattern="MinGit",
                sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", bin="cmd/git.exe" }
        end)");
    write(root / "blueprints" / "bootstrap.luau", R"(
        return function(ctx)
            return ctx.lib.composite{ name = "bootstrap", components = { ctx.pkgs.mingit } }
        end)");

    EvalRequest req;
    req.blueprint_path = root / "blueprints" / "bootstrap.luau";
    req.system = SystemInfo{"windows", "x64", "11"};
    req.source = SourceCtx{"test-src", root, ""};

    auto r = evaluate(req);
    REQUIRE(r.has_value());
    REQUIRE(r->root() != nullptr);
    CHECK(r->root()->name == "bootstrap");
    CHECK(r->root()->components.size() == 1);
    CHECK(r->graph.nodes.size() == 2);
    // the resolved child is the mingit fetch
    const auto* child = r->graph.find(r->root()->components.at(0));
    REQUIRE(child != nullptr);
    CHECK(child->name == "mingit");

    fs::remove_all(root);
}

TEST_CASE("blueprint_eval: lib.merge shallow-merges, right side wins") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            local m = ctx.lib.merge({ a = "x", b = "y" }, { b = "z" })
            return ctx.lib.composite{ name = m.a .. m.b, components = {} }
        end)"));
    REQUIRE(r.has_value());
    REQUIRE(r->root() != nullptr);
    CHECK(r->root()->name == "xz");  // a from left, b overridden by right
}

TEST_CASE("blueprint_eval: lib.withFiles wraps a base derivation") {
    auto r = evaluate(inline_req(R"(
        return function(ctx)
            local base = ctx.lib.fetchGitHubRelease{
                name="t", owner="o", repo="r", version="1", assetPattern="p",
                sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }
            return ctx.lib.withFiles(base, { ["cfg.toml"] = { mode = "replace", content = "hello" } })
        end)"));
    REQUIRE(r.has_value());
    REQUIRE(r->root() != nullptr);
    CHECK(r->root()->builder == Builder::WithFiles);
    CHECK(r->root()->files.size() == 1);
    CHECK(r->graph.nodes.size() == 2);  // base fetch + withFiles node
}

TEST_CASE("blueprint_eval: readResource reads the source's resources dir") {
    fs::path root = scratch();
    write(root / "resources" / "note.txt", "hello-resource");
    write(root / "blueprints" / "bp.luau", R"(
        return function(ctx)
            return ctx.lib.composite{ name = ctx.lib.readResource("note.txt"), components = {} }
        end)");
    EvalRequest req;
    req.blueprint_path = root / "blueprints" / "bp.luau";
    req.system = SystemInfo{"windows", "x64", "11"};
    req.source = SourceCtx{"s", root, ""};
    auto r = evaluate(req);
    REQUIRE(r.has_value());
    CHECK(r->root()->name == "hello-resource");
    fs::remove_all(root);
}

TEST_CASE("blueprint_eval: readResource rejects traversal but allows '..' in a name (L-1)") {
    fs::path root = scratch();
    write(root / "resources" / "v1..2.txt", "ok");  // '..' inside a segment is fine

    auto eval_res = [&](const char* rel) {
        std::string src = std::string("return function(ctx) return ctx.lib.composite{ name = "
                                      "ctx.lib.readResource(\"") + rel + "\"), components = {} } end";
        write(root / "blueprints" / "bp.luau", src);
        EvalRequest req;
        req.blueprint_path = root / "blueprints" / "bp.luau";
        req.system = SystemInfo{"windows", "x64", "11"};
        req.source = SourceCtx{"s", root, ""};
        return evaluate(req);
    };

    auto ok = eval_res("v1..2.txt");
    REQUIRE(ok.has_value());
    CHECK(ok->root()->name == "ok");

    auto bad = eval_res("../secret");  // a real traversal segment
    CHECK_FALSE(bad.has_value());

    fs::remove_all(root);
}

TEST_CASE("blueprint_eval: lockedAt does not affect the derivation hash (R-NF-04)") {
    auto mk = [](const char* locked) {
        EvalRequest r;
        r.inline_source = R"(
            return function(ctx)
                return ctx.lib.fetchGitHubRelease{
                    name="x", owner="o", repo="r", version="1",
                    assetPattern="p", sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }
            end)";
        r.chunk_name = "t";
        r.system = SystemInfo{"windows", "x64", "11"};
        r.source = SourceCtx{"s", "C:/tmp", locked};
        return r;
    };
    auto a = evaluate(mk("2026-01-01"));
    auto b = evaluate(mk("2030-12-31"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->graph.root == b->graph.root);  // identical hash
}
