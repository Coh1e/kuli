#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "kuli/crypto/hash.hpp"
#include "kuli/store/archive.hpp"
#include "kuli/store/store.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-store-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
}  // namespace

TEST_CASE("store path shape is <hash16>-<name>") {
    store::Store s{"C:/root", "C:/dl"};
    CHECK(s.store_path("0123456789abcdef", "ripgrep").filename().string() ==
          "0123456789abcdef-ripgrep");
}

TEST_CASE("store realize_from_archive: verify + extract + marker + cache hit") {
    fs::path dir = scratch();
    fs::path zip = dir / "tool.zip";
    REQUIRE(archive::create_zip(zip, {
        {"tool-2.0/bin/demo.txt", "payload"},
        {"tool-2.0/LICENSE", "mit"},
    }).has_value());

    auto sha = crypto::hash_file(zip);
    REQUIRE(sha.has_value());

    store::Store s{dir / "store", dir / "dl"};
    auto r = s.realize_from_archive("deadbeefdeadbeef", "tool", zip, *sha, "bin/demo.txt");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->was_already_present);
    CHECK(fs::exists(r->store_dir));
    CHECK(fs::exists(r->store_dir / ".kuli-store-marker.json"));
    CHECK(fs::exists(r->bin_path));
    CHECK(r->bin_path.filename().string() == "demo.txt");

    // Second realize is a no-network cache hit.
    auto r2 = s.realize_from_archive("deadbeefdeadbeef", "tool", zip, *sha, "bin/demo.txt");
    REQUIRE(r2.has_value());
    CHECK(r2->was_already_present);

    fs::remove_all(dir);
}

TEST_CASE("store detects a collision: same path, different content (H-2)") {
    fs::path dir = scratch();
    fs::path zipA = dir / "a.zip";
    fs::path zipB = dir / "b.zip";
    REQUIRE(archive::create_zip(zipA, {{"t/x.txt", "AAAA"}}).has_value());
    REQUIRE(archive::create_zip(zipB, {{"t/x.txt", "BBBB"}}).has_value());
    auto shaA = crypto::hash_file(zipA);
    auto shaB = crypto::hash_file(zipB);
    REQUIRE(shaA.has_value());
    REQUIRE(shaB.has_value());

    store::Store s{dir / "store", dir / "dl"};
    // Realize A at a given (hash16,name).
    REQUIRE(s.realize_from_archive("aaaa111122223333", "tool", zipA, *shaA, "").has_value());
    // Realizing *different* content at the same path is a collision, not a hit.
    auto r = s.realize_from_archive("aaaa111122223333", "tool", zipB, *shaB, "");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("collision") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("store recovers from a markerless partial dir (H-3)") {
    fs::path dir = scratch();
    fs::path zip = dir / "t.zip";
    REQUIRE(archive::create_zip(zip, {{"t/x.txt", "data"}}).has_value());
    auto sha = crypto::hash_file(zip);
    REQUIRE(sha.has_value());

    store::Store s{dir / "store", dir / "dl"};
    fs::path target = s.store_path("beef000011112222", "tool");
    // Simulate an interrupted realize: a dir exists with NO marker.
    fs::create_directories(target / "leftover");
    { std::ofstream(target / "leftover" / "junk.txt") << "stale"; }
    CHECK_FALSE(s.is_present(target));

    auto r = s.realize_from_archive("beef000011112222", "tool", zip, *sha, "");
    REQUIRE(r.has_value());               // recovered, not wedged
    CHECK(s.is_present(target));          // published with a marker now
    CHECK(fs::exists(target / "x.txt"));  // fresh content
    CHECK_FALSE(fs::exists(target / "leftover"));  // stale partial removed
    fs::remove_all(dir);
}

TEST_CASE("store rejects sha256 mismatch") {
    fs::path dir = scratch();
    fs::path zip = dir / "tool.zip";
    REQUIRE(archive::create_zip(zip, {{"t/x.txt", "data"}}).has_value());

    crypto::HashSpec wrong{crypto::Algorithm::Sha256, std::string(64, 'a')};
    store::Store s{dir / "store", dir / "dl"};
    auto r = s.realize_from_archive("0000111122223333", "tool", zip, wrong, "");
    CHECK_FALSE(r.has_value());
    CHECK(r.error().message.find("mismatch") != std::string::npos);
    // Nothing published on failure.
    CHECK_FALSE(s.is_present(s.store_path("0000111122223333", "tool")));

    fs::remove_all(dir);
}
