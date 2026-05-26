#pragma once
// kuli-crypto — content hashing over OpenSSL EVP.
//
// design.md §12.1 names OpenSSL as the crypto stack (cross-platform R-NF-05);
// this absorbs ref/luban/src/hash.{hpp,cpp} but swaps BCrypt for EVP_MD_CTX so
// the same code compiles on Windows / Linux / macOS.
//
// Two roles:
//   * file hashing + verify  — store artifact integrity (R-F-08 / §8.2.7)
//   * in-memory byte hashing  — derivation input-closure hash (§8.2.1), where
//     determinism matters; see kuli-luau/canonical_json.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kuli::crypto {

namespace fs = std::filesystem;

enum class Algorithm { Sha256, Sha512, Md5, Sha1 };

struct HashSpec {
    Algorithm algo = Algorithm::Sha256;
    std::string hex;  // lowercase

    bool operator==(const HashSpec&) const = default;
};

std::string algo_name(Algorithm a);
std::optional<Algorithm> parse_algo(std::string_view name);

// Parse "sha256:<hex>" / "sha512:<hex>" / bare hex (defaults sha256).
// Returns nullopt on malformed input.
std::optional<HashSpec> parse(std::string_view raw);

// Serialize to "<algo>:<hex>".
std::string to_string(const HashSpec& spec);

// --- in-memory hashing -----------------------------------------------------
// Lowercase hex digest of `data`.
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] std::string md5_hex(std::span<const std::byte> data);
[[nodiscard]] std::string md5_hex(std::string_view data);

// --- file hashing ----------------------------------------------------------
// Streaming digest of a file. Missing / unreadable -> nullopt.
std::optional<HashSpec> hash_file(const fs::path& path, Algorithm algo = Algorithm::Sha256);

// True if the file's digest equals `expected` (case-insensitive hex).
bool verify_file(const fs::path& path, const HashSpec& expected);

}  // namespace kuli::crypto
