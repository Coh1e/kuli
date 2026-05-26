#include <doctest/doctest.h>

#include <vector>

#include "kuli/bp/resolver.hpp"

using namespace kuli;

namespace {
std::vector<bp::Asset> assets(std::initializer_list<const char*> names) {
    std::vector<bp::Asset> v;
    for (const char* n : names) v.push_back(bp::Asset{n, std::string("https://dl/") + n});
    return v;
}
}  // namespace

TEST_CASE("resolver picks the real asset over a .sha256 sidecar (M-1)") {
    auto a = assets({"rg-x86_64-pc-windows-msvc.zip.sha256",
                     "rg-x86_64-pc-windows-msvc.zip"});
    auto r = bp::select_asset(a, "x86_64%-pc%-windows%-msvc%.zip$", "windows-x64");
    REQUIRE(r.has_value());
    CHECK(r->name == "rg-x86_64-pc-windows-msvc.zip");  // not the .sha256
}

TEST_CASE("resolver end-anchored pattern doesn't match a sidecar") {
    auto a = assets({"tool.zip.sig", "tool.zip"});
    auto r = bp::select_asset(a, "tool%.zip$", "windows-x64");
    REQUIRE(r.has_value());
    CHECK(r->name == "tool.zip");
}

TEST_CASE("resolver scoring tiebreak prefers the host-triplet asset") {
    // Pattern matches both; host-triplet score breaks the tie.
    auto a = assets({"tool-linux-x86_64.tar.gz", "tool-windows-x86_64.zip"});
    auto win = bp::select_asset(a, "tool%-", "windows-x64");
    REQUIRE(win.has_value());
    CHECK(win->name == "tool-windows-x86_64.zip");
    auto lin = bp::select_asset(a, "tool%-", "linux-x64");
    REQUIRE(lin.has_value());
    CHECK(lin->name == "tool-linux-x86_64.tar.gz");
}

TEST_CASE("resolver matches a literal dot pattern correctly") {
    // The old de-escaper dropped '.', collapsing "win.zip" to "winzip".
    auto a = assets({"app-win.zip"});
    auto r = bp::select_asset(a, "win%.zip", "windows-x64");
    REQUIRE(r.has_value());
    CHECK(r->name == "app-win.zip");
}

TEST_CASE("resolver returns nullopt when nothing matches") {
    auto a = assets({"tool-linux.tar.gz", "tool.zip.sha256"});
    CHECK_FALSE(bp::select_asset(a, "does%-not%-exist", "windows-x64").has_value());
}
