#pragma once
// kuli-store — content-addressed store. Absorbs ref/luban/src/store.cpp +
// store_fetch.cpp, but keyed by the derivation's input-closure hash (§8.2.1)
// rather than the binary's hash: the store path is supplied by the caller and
// the asset sha256 is an *input* verified at realize time.
//
// Layout: <root>/<hash16>-<name>/  with a .kuli-store-marker.json (crash-safe:
// an interrupted realize leaves no marker, so the next attempt re-extracts).

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kuli/crypto/hash.hpp"
#include "kuli/diag/diagnostic.hpp"

namespace kuli::store {

namespace fs = std::filesystem;

struct FetchResult {
    fs::path store_dir;             // <root>/<hash16>-<name>
    fs::path bin_path;              // store_dir / bin  (== store_dir if bin empty)
    bool was_already_present = false;
};

// A store rooted at a directory, with a download cache. `default_store()`
// reads the real XDG locations; tests construct one over a temp dir.
struct Store {
    fs::path root;       // the store directory
    fs::path downloads;  // download cache

    [[nodiscard]] fs::path store_path(std::string_view hash16, std::string_view name) const;
    [[nodiscard]] bool is_present(const fs::path& store_dir) const;

    // Verify + extract an already-local archive into the store atomically.
    // No network. Idempotent: a present, marked store path short-circuits.
    std::expected<FetchResult, kuli::diag::Diagnostic> realize_from_archive(
        std::string_view hash16, std::string_view name, const fs::path& archive,
        const kuli::crypto::HashSpec& expected, std::string_view bin) const;

    // Download `url` (to the cache), verify, then realize_from_archive.
    std::expected<FetchResult, kuli::diag::Diagnostic> realize_fetch(
        std::string_view hash16, std::string_view name, std::string_view url,
        const kuli::crypto::HashSpec& expected, std::string_view bin) const;

    // Realize inline file content into a content-addressed store path (scripture
    // adapters/resources/manifest, §9.1). Unlike realize_from_archive the bytes
    // are supplied directly; `content_id` (the scripture's derivation hash) is
    // recorded in the marker for idempotency + collision detection.
    // `store_dir_name` is the derivation's full storePath ("<hash16>-<name>-scripture").
    // `files` maps relative paths (rejected if they escape the path) to content.
    std::expected<FetchResult, kuli::diag::Diagnostic> realize_inline(
        std::string_view store_dir_name, std::string_view content_id,
        const std::vector<std::pair<std::string, std::string>>& files) const;
};

Store default_store();

}  // namespace kuli::store
