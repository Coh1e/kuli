#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "kuli/crypto/hash.hpp"
#include "kuli/http/download.hpp"
#include "kuli/platform/env.hpp"
#include "kuli/store/store.hpp"

namespace fs = std::filesystem;
using namespace kuli;

// Real network end-to-end (HTTPS download via static libcurl+OpenSSL+HTTP/2 ->
// verify -> extract -> store). Self-skips unless KULI_NET_TESTS is set, so the
// default suite and CI stay offline and this never touches the user's machine
// (temp dirs only; no HKCU, no real store/bin).
TEST_CASE("net_smoke: download + realize a real release [gated: KULI_NET_TESTS]") {
    if (!platform::get_env("KULI_NET_TESTS")) return;

    static std::mt19937_64 rng{std::random_device{}()};
    fs::path dir = fs::temp_directory_path() / ("kuli-net-" + std::to_string(rng()));
    fs::create_directories(dir);

    const std::string url =
        "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip";
    fs::path zip = dir / "ninja.zip";

    auto dl = http::fetch_to_file(url, zip);
    if (!dl) MESSAGE("download failed: ", dl.error().message);
    REQUIRE(dl.has_value());

    auto sha = crypto::hash_file(zip);
    REQUIRE(sha.has_value());

    store::Store st{dir / "store", dir / "dl"};
    auto r = st.realize_from_archive("ninja00000000001", "ninja", zip, *sha, "ninja.exe");
    REQUIRE(r.has_value());
    CHECK(fs::exists(r->bin_path));  // store/<hash>-ninja/ninja.exe

    fs::remove_all(dir);
}
