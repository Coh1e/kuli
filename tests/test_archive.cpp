#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "kuli/store/archive.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() /
                 ("kuli-archive-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("archive zip round-trip with single-top-dir flatten") {
    fs::path dir = scratch();
    fs::path zip = dir / "a.zip";
    REQUIRE(archive::create_zip(zip, {
        {"tool-1.0/bin/demo.txt", "hello"},
        {"tool-1.0/README.md", "readme"},
    }).has_value());

    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    REQUIRE(r.has_value());
    // top dir "tool-1.0/" stripped.
    CHECK(fs::exists(out / "bin" / "demo.txt"));
    CHECK(read(out / "bin" / "demo.txt") == "hello");
    CHECK(fs::exists(out / "README.md"));
    CHECK_FALSE(fs::exists(out / "tool-1.0"));

    fs::remove_all(dir);
}

TEST_CASE("archive no flatten when multiple top-level entries") {
    fs::path dir = scratch();
    fs::path zip = dir / "b.zip";
    REQUIRE(archive::create_zip(zip, {
        {"a/x.txt", "1"},
        {"b/y.txt", "2"},
    }).has_value());
    fs::path out = dir / "out";
    REQUIRE(archive::extract(zip, out).has_value());
    CHECK(fs::exists(out / "a" / "x.txt"));
    CHECK(fs::exists(out / "b" / "y.txt"));
    fs::remove_all(dir);
}

TEST_CASE("archive selective extraction by path prefix") {
    fs::path dir = scratch();
    fs::path zip = dir / "c.zip";
    REQUIRE(archive::create_zip(zip, {
        {"pkg/scripts/a.lua", "a"},
        {"pkg/fonts/f.ttf", "f"},
        {"pkg/docs/d.md", "d"},
    }).has_value());
    fs::path out = dir / "out";
    archive::ExtractOptions opts;
    opts.paths = {"scripts/", "fonts/"};  // post-flatten prefixes
    REQUIRE(archive::extract(zip, out, opts).has_value());
    CHECK(fs::exists(out / "scripts" / "a.lua"));
    CHECK(fs::exists(out / "fonts" / "f.ttf"));
    CHECK_FALSE(fs::exists(out / "docs" / "d.md"));
    fs::remove_all(dir);
}

TEST_CASE("archive rejects path traversal") {
    fs::path dir = scratch();
    fs::path zip = dir / "evil.zip";
    REQUIRE(archive::create_zip(zip, {
        {"safe/ok.txt", "ok"},
        {"safe/../../escape.txt", "bad"},
    }).has_value());
    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    CHECK_FALSE(r.has_value());  // traversal entry aborts the extract
    fs::remove_all(dir);
}

TEST_CASE("archive accepts a filename containing '..' (L-1, no false reject)") {
    fs::path dir = scratch();
    fs::path zip = dir / "d.zip";
    REQUIRE(archive::create_zip(zip, {
        {"pkg/v1..2/notes.txt", "ok"},  // '..' inside segments, not a traversal
    }).has_value());
    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    REQUIRE(r.has_value());
    CHECK(fs::exists(out / "v1..2" / "notes.txt"));  // "pkg" top dir flattened
    fs::remove_all(dir);
}

TEST_CASE("archive unsupported format reports a diagnostic") {
    fs::path dir = scratch();
    fs::path tar = dir / "x.tar.gz";
    { std::ofstream(tar) << "junk"; }
    auto r = archive::extract(tar, dir / "out");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("unsupported") != std::string::npos);
    fs::remove_all(dir);
}
