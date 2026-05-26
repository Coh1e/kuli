#include <doctest/doctest.h>

#include "kuli/luau/hashing.hpp"

using namespace kuli::luau;

namespace {
FetchSpec base_spec() {
    FetchSpec f;
    f.owner = "BurntSushi";
    f.repo = "ripgrep";
    f.version = "14.1.0";
    f.asset_pattern = "%-x86_64%-pc%-windows%-msvc";
    f.sha256 = "abc123";
    f.bin = "rg.exe";
    return f;
}
}  // namespace

TEST_CASE("derivation_hash fetch is deterministic and 64 hex") {
    auto h1 = hash_fetch(base_spec(), "ripgrep", "windows-x64");
    auto h2 = hash_fetch(base_spec(), "ripgrep", "windows-x64");
    CHECK(h1 == h2);
    CHECK(h1.size() == 64);
}

TEST_CASE("derivation_hash store path is hash[:16]-name") {
    auto h = hash_fetch(base_spec(), "ripgrep", "windows-x64");
    CHECK(store_path_for(h, "ripgrep") == h.substr(0, 16) + "-ripgrep");
}

TEST_CASE("derivation_hash: content-affecting fields change the hash") {
    auto baseh = hash_fetch(base_spec(), "ripgrep", "windows-x64");

    auto f_ver = base_spec(); f_ver.version = "14.0.0";
    CHECK(hash_fetch(f_ver, "ripgrep", "windows-x64") != baseh);

    auto f_sha = base_spec(); f_sha.sha256 = "def456";
    CHECK(hash_fetch(f_sha, "ripgrep", "windows-x64") != baseh);

    // different target = different derivation
    CHECK(hash_fetch(base_spec(), "ripgrep", "linux-x64") != baseh);

    // post_install changes realized contents -> changes the hash
    auto f_post = base_spec(); f_post.post_install = "setup.bat";
    CHECK(hash_fetch(f_post, "ripgrep", "windows-x64") != baseh);
}

TEST_CASE("derivation_hash: profile-only fields do NOT change the hash") {
    auto baseh = hash_fetch(base_spec(), "ripgrep", "windows-x64");
    // bin / shim_dir are profile concerns (which shims), not store content.
    auto f_bin = base_spec(); f_bin.bin = "bin/rg.exe"; f_bin.shim_dir = "bin";
    CHECK(hash_fetch(f_bin, "ripgrep", "windows-x64") == baseh);
}

TEST_CASE("derivation_hash composite: component order matters, set is stable") {
    auto a = hash_composite("bootstrap", {"h1", "h2"}, {});
    auto b = hash_composite("bootstrap", {"h2", "h1"}, {});
    auto a2 = hash_composite("bootstrap", {"h1", "h2"}, {});
    CHECK(a == a2);
    CHECK(a != b);  // symlinkJoin order is significant
}

TEST_CASE("derivation_hash withFiles: content matters, file order does not") {
    std::vector<FileEntry> f1{{"a.toml", "replace", "X"}, {"b.toml", "replace", "Y"}};
    std::vector<FileEntry> f2{{"b.toml", "replace", "Y"}, {"a.toml", "replace", "X"}};
    CHECK(hash_withfiles("cfg", "base", f1) == hash_withfiles("cfg", "base", f2));

    std::vector<FileEntry> f3{{"a.toml", "replace", "CHANGED"}, {"b.toml", "replace", "Y"}};
    CHECK(hash_withfiles("cfg", "base", f1) != hash_withfiles("cfg", "base", f3));

    // base identity matters
    CHECK(hash_withfiles("cfg", "base", f1) != hash_withfiles("cfg", "other", f1));
}
