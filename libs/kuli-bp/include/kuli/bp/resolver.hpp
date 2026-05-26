#pragma once
// GitHub release asset resolution. Absorbs ref/luban/src/source_resolver_github
// (asset scoring) + lua_pattern (matching). Turns a fetch derivation's
// (owner, repo, version, assetPattern) into a concrete download URL. Runs at
// apply time (network), not eval time, so it never affects the derivation hash.

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"

namespace kuli::bp {

struct Asset {
    std::string name;
    std::string url;
};

// Choose the best asset for `target` among those whose name matches `pattern`
// (a Lua pattern) and is not a checksum/signature sidecar. Ties broken by a
// host-triplet score (prefers the right os/arch). Pure + network-free (testable).
std::optional<Asset> select_asset(const std::vector<Asset>& assets,
                                  const std::string& pattern, const std::string& target);

// Resolve owner/repo/version/assetPattern -> download URL (GitHub API GET).
std::expected<std::string, kuli::diag::Diagnostic> resolve_github_asset(
    const std::string& owner, const std::string& repo, const std::string& version,
    const std::string& asset_pattern, const std::string& target);

// Resolve every fetch derivation's `url` in place within an ApplyDerivation doc.
std::expected<void, kuli::diag::Diagnostic> resolve_urls(nlohmann::json& ir);

}  // namespace kuli::bp
