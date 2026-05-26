#pragma once
// kuli-http — in-process libcurl HTTP(S) client. Absorbs ref/luban/src/
// download.cpp + libcurl_backend.cpp. The full libuv RPC transport
// (kuli-transport) is a later milestone; the bp feature only needs GET.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "kuli/crypto/hash.hpp"
#include "kuli/diag/diagnostic.hpp"

namespace kuli::http {

namespace fs = std::filesystem;

// (bytes_now, bytes_total) — total is 0 when the server omits Content-Length.
using ProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

struct FetchOptions {
    // If set, the downloaded file's digest must match or the fetch fails and
    // the partial file is removed.
    std::optional<kuli::crypto::HashSpec> expected;
    ProgressFn progress;
};

// Download `url` to `dest` (HTTP/2, follow redirects, fail on >=400). The
// destination's parent directory must exist. On failure the partial file is
// removed and a Diagnostic returned.
std::expected<void, kuli::diag::Diagnostic> fetch_to_file(
    std::string_view url, const fs::path& dest, const FetchOptions& opts = {});

// Download `url` into memory (small responses: GitHub API JSON, etc.).
std::expected<std::string, kuli::diag::Diagnostic> fetch_text(std::string_view url);

// Rewrite github.com / *.githubusercontent.com through KULI_GITHUB_MIRROR_PREFIX
// (api.github.com is left alone — public mirrors 403 it). Returns the url
// unchanged when the env var is unset.
std::string apply_mirror(std::string url);

}  // namespace kuli::http
