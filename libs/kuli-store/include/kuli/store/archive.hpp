#pragma once
// kuli-store/archive — archive extraction. Absorbs ref/luban/src/archive.cpp
// (miniz ZIP, single-top-dir flatten, path-traversal guard) and adds partial
// extraction (devkit §4.6). v0.x supports .zip; .tar.zst / .tar.gz are added
// when a blueprint needs them.

#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "kuli/diag/diagnostic.hpp"

namespace kuli::archive {

namespace fs = std::filesystem;

struct ExtractOptions {
    // If non-empty, extract only entries whose (post-flatten) path begins with
    // one of these prefixes (devkit §4.6 selective extraction).
    std::vector<std::string> paths;
    // Strip a single shared top-level directory (tool-1.0/bin/x -> bin/x).
    bool flatten_top_dir = true;
};

// Extract `archive` into `dest` (created if absent). Dispatches on extension.
std::expected<void, kuli::diag::Diagnostic> extract(const fs::path& archive,
                                                    const fs::path& dest,
                                                    const ExtractOptions& opts = {});

// Create a ZIP at `out` from {relative-path, bytes} entries. Used by tests and
// by future scripture packaging; keeps a single ZIP implementation.
std::expected<void, kuli::diag::Diagnostic> create_zip(
    const fs::path& out,
    const std::vector<std::pair<std::string, std::string>>& entries);

}  // namespace kuli::archive
