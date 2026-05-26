#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "kuli/bp/source.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-src-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
void write(const fs::path& p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
const char* kBp = "return function(ctx) return ctx.lib.composite{ name='x', components={} } end";
}  // namespace

TEST_CASE("source: add a local source, resolve by name, list, remove") {
    fs::path root = scratch();
    fs::path repo = root / "myrepo";
    write(repo / "blueprints" / "foo.luau", kBp);

    bp::SourceRegistry reg{root / "config", root / "data"};
    auto e = reg.add(repo.string(), std::nullopt, /*assume_yes=*/true);
    REQUIRE(e.has_value());
    CHECK(e->name == "myrepo");
    CHECK(e->is_local());

    // Registry round-trips through the on-disk .toml.
    CHECK(reg.list().size() == 1);
    REQUIRE(reg.find("myrepo").has_value());
    CHECK(reg.find("myrepo")->url == repo.string());

    // Resolve bare + qualified.
    auto r = reg.resolve("foo");
    REQUIRE(r.has_value());
    CHECK(r->source_name == "myrepo");
    CHECK(fs::exists(r->blueprint_file));
    CHECK(reg.resolve("myrepo/foo").has_value());
    CHECK_FALSE(reg.resolve("nope").has_value());

    REQUIRE(reg.remove("myrepo").has_value());
    CHECK(reg.list().empty());
    fs::remove_all(root);
}

TEST_CASE("source: bare name ambiguous across sources, qualified resolves") {
    fs::path root = scratch();
    write(root / "a" / "blueprints" / "dup.luau", kBp);
    write(root / "b" / "blueprints" / "dup.luau", kBp);

    bp::SourceRegistry reg{root / "config", root / "data"};
    REQUIRE(reg.add((root / "a").string(), std::string("srca"), true).has_value());
    REQUIRE(reg.add((root / "b").string(), std::string("srcb"), true).has_value());

    CHECK_FALSE(reg.resolve("dup").has_value());        // ambiguous
    CHECK(reg.resolve("srca/dup").has_value());          // qualified ok
    fs::remove_all(root);
}

TEST_CASE("source: a nonexistent local path is rejected (not mistaken for github)") {
    fs::path root = scratch();
    bp::SourceRegistry reg{root / "config", root / "data"};
    auto r = reg.add((root / "does-not-exist").string(), std::nullopt, true);
    CHECK_FALSE(r.has_value());  // must error, not attempt a network fetch
    fs::remove_all(root);
}

TEST_CASE("source: local source missing blueprints/ is rejected") {
    fs::path root = scratch();
    fs::create_directories(root / "emptyrepo");
    bp::SourceRegistry reg{root / "config", root / "data"};
    CHECK_FALSE(reg.add((root / "emptyrepo").string(), std::nullopt, true).has_value());
    fs::remove_all(root);
}
