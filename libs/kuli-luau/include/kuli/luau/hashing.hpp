#pragma once
// Derivation hashing (§8.2.1). The input closure is serialized to canonical
// JSON (lexicographic keys via nlohmann's default sorted object, ECMA-404
// escapes, integers without decimals — NFC normalization deferred; inputs are
// ASCII identifiers/hashes in practice) then sha256'd. Determinism is the
// whole point (R-NF-04): same inputs -> same hash -> same store path, so the
// hash is computed C++-side, never in Luau.
//
// Deliberately excluded from every closure: kuli core version, eval-time
// clock, source.lockedAt.

#include <string>
#include <vector>

#include "kuli/luau/derivation.hpp"

namespace kuli::luau {

// 64-hex sha256 of the fetch input closure.
std::string hash_fetch(const FetchSpec& f, const std::string& name,
                       const std::string& system_target);

// composite: ordered component + requires hashes + name (§8.2.6).
std::string hash_composite(const std::string& name,
                           const std::vector<std::string>& components,
                           const std::vector<std::string>& requires_);

// withFiles: base hash + per-file {path, mode, content-sha256} (§8.2.6).
std::string hash_withfiles(const std::string& name, const std::string& base_hash,
                           const std::vector<FileEntry>& files);

// "<hash[:16]>-<name>".
std::string store_path_for(const std::string& hash, const std::string& name);

}  // namespace kuli::luau
