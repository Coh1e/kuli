#include "kuli/luau/hashing.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>

#include "kuli/crypto/hash.hpp"

namespace kuli::luau {

namespace {

using nlohmann::json;  // default json = std::map-backed -> keys always sorted

// L-2 guard: hashed closures must contain only strings / objects / arrays.
// A number (esp. a float) would make the canonical bytes depend on nlohmann's
// version-unstable number formatting, silently forking derivation hashes
// (R-NF-04). Numbers never appear today; this catches a future stray field in
// Debug builds.
void assert_no_numbers([[maybe_unused]] const json& j) {
#ifndef NDEBUG
    assert(!j.is_number() && "hashed closure must not contain numbers (R-NF-04 determinism)");
    if (j.is_object()) {
        for (const auto& [_, v] : j.items()) assert_no_numbers(v);
    } else if (j.is_array()) {
        for (const auto& v : j) assert_no_numbers(v);
    }
#endif
}

// Canonical bytes: sorted keys (nlohmann default), compact, ECMA-404 escapes,
// raw UTF-8 (ensure_ascii=false). Deterministic regardless of insertion order.
std::string canonical(const json& j) { return j.dump(); }

std::string sha256_of(const json& j) {
    assert_no_numbers(j);
    return kuli::crypto::sha256_hex(canonical(j));
}

}  // namespace

std::string hash_fetch(const FetchSpec& f, const std::string& name,
                       const std::string& system_target) {
    json closure{
        {"builder_cmd", "fetch"},
        {"name", name},
        {"system_target", system_target},
        {"source",
         {{"owner", f.owner},
          {"repo", f.repo},
          {"version", f.version},
          {"assetPattern", f.asset_pattern}}},
        {"source_sha256", f.sha256},
        // post_install changes the realized store contents, so it is part of
        // the identity. bin/shim_dir are profile concerns and excluded.
        {"post_install", f.post_install},
        {"env", json::object()},
    };
    return sha256_of(closure);
}

std::string hash_composite(const std::string& name,
                           const std::vector<std::string>& components,
                           const std::vector<std::string>& requires_) {
    json closure{
        {"builder_cmd", "composite"},
        {"name", name},
        {"components", components},  // order preserved (symlinkJoin order)
        {"requires", requires_},
    };
    return sha256_of(closure);
}

std::string hash_withfiles(const std::string& name, const std::string& base_hash,
                           const std::vector<FileEntry>& files) {
    json arr = json::array();
    // Sort by path for determinism (map iteration order is not guaranteed).
    std::vector<FileEntry> sorted = files;
    std::sort(sorted.begin(), sorted.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    for (const auto& f : sorted) {
        arr.push_back({{"path", f.path},
                       {"mode", f.mode},
                       {"content_sha256", kuli::crypto::sha256_hex(f.content)}});
    }
    json closure{
        {"builder_cmd", "withFiles"},
        {"name", name},
        {"base", base_hash},
        {"files", arr},
    };
    return sha256_of(closure);
}

std::string store_path_for(const std::string& hash, const std::string& name) {
    return hash.substr(0, 16) + "-" + name;
}

}  // namespace kuli::luau
