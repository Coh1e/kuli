#include "kuli/bp/resolver.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <initializer_list>
#include <string>
#include <string_view>

#include "kuli/http/download.hpp"
#include "kuli/luau/pattern.hpp"

namespace kuli::bp {

namespace {
using kuli::diag::Diagnostic;
using nlohmann::json;

Diagnostic err(std::string msg) { return Diagnostic::error(std::move(msg), "E0400"); }

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool m = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { m = false; break; }
        }
        if (m) return true;
    }
    return false;
}

// Checksum / signature / debug-symbol sidecars never carry the tool itself.
bool is_sidecar(std::string_view name) {
    for (const char* s : {".sha256", ".sha512", ".sha1", ".md5", ".asc", ".sig",
                          ".pem", ".pdb", ".cat"}) {
        if (name.size() >= std::string_view(s).size() &&
            contains_ci(name.substr(name.size() - std::string_view(s).size()), s)) {
            return true;
        }
    }
    return contains_ci(name, "checksums");
}

// Anchored os-token detector (avoids `darwin` matching "win", etc.) — ported
// from luban's source_resolver_github.
bool has_os_token(std::string_view name, std::initializer_list<std::string_view> tokens) {
    auto sep = [](char c) { return c == '-' || c == '_' || c == '.' || c == '/'; };
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto t : tokens) {
        std::string needle(t);
        std::size_t pos = 0;
        while ((pos = lower.find(needle, pos)) != std::string::npos) {
            bool left = pos == 0 || sep(lower[pos - 1]);
            bool right = pos + needle.size() >= lower.size() || sep(lower[pos + needle.size()]);
            if (left && right) return true;
            ++pos;
        }
    }
    return false;
}

int score_windows(std::string_view n) {
    int s = 0;
    if (contains_ci(n, "x86_64") || contains_ci(n, "amd64") || contains_ci(n, "x64") ||
        contains_ci(n, "win64")) s += 5;
    if (contains_ci(n, "windows") || contains_ci(n, "msvc") || has_os_token(n, {"win"})) s += 5;
    if (contains_ci(n, ".zip")) s += 2;
    if (contains_ci(n, "arm64") || contains_ci(n, "aarch64")) s -= 10;
    if (contains_ci(n, "darwin") || contains_ci(n, "macos") || has_os_token(n, {"mac"})) s -= 10;
    if (contains_ci(n, "linux") && !contains_ci(n, "windows") && !contains_ci(n, "msvc")) s -= 10;
    return s;
}
int score_linux(std::string_view n) {
    int s = 0;
    if (contains_ci(n, "x86_64") || contains_ci(n, "amd64") || contains_ci(n, "x64")) s += 5;
    if (contains_ci(n, "linux")) s += 5;
    if (contains_ci(n, ".tar.gz") || contains_ci(n, ".tgz") || contains_ci(n, ".tar.zst")) s += 2;
    if (contains_ci(n, "windows") || contains_ci(n, "msvc") || has_os_token(n, {"win"})) s -= 10;
    if (contains_ci(n, "darwin") || contains_ci(n, "macos") || has_os_token(n, {"mac"})) s -= 10;
    if (contains_ci(n, "arm64") || contains_ci(n, "aarch64")) s -= 10;
    return s;
}
int score_macos(std::string_view n, bool arm) {
    int s = 0;
    if (contains_ci(n, "darwin") || contains_ci(n, "apple") || contains_ci(n, "macos") ||
        contains_ci(n, "osx") || has_os_token(n, {"mac"})) s += 5;
    if (arm) { if (contains_ci(n, "arm64") || contains_ci(n, "aarch64")) s += 5; }
    else { if (contains_ci(n, "x86_64") || contains_ci(n, "x64")) s += 5; }
    if (contains_ci(n, "windows") || contains_ci(n, "msvc") || has_os_token(n, {"win"})) s -= 10;
    if (contains_ci(n, "linux")) s -= 10;
    return s;
}
int score_for_target(std::string_view name, std::string_view target) {
    if (target == "windows-x64") return score_windows(name);
    if (target == "linux-x64") return score_linux(name);
    if (target == "macos-arm64") return score_macos(name, true);
    if (target == "macos-x64") return score_macos(name, false);
    return 0;
}

}  // namespace

std::optional<Asset> select_asset(const std::vector<Asset>& assets, const std::string& pattern,
                                  const std::string& target) {
    const Asset* best = nullptr;
    int best_score = INT_MIN;
    for (const auto& a : assets) {
        if (is_sidecar(a.name)) continue;
        if (!kuli::luau::pattern_matches(a.name, pattern)) continue;
        int s = score_for_target(a.name, target);
        if (!best || s > best_score) {
            best = &a;
            best_score = s;
        }
    }
    if (best) return *best;
    return std::nullopt;
}

std::expected<std::string, Diagnostic> resolve_github_asset(const std::string& owner,
                                                            const std::string& repo,
                                                            const std::string& version,
                                                            const std::string& asset_pattern,
                                                            const std::string& target) {
    std::string api = "https://api.github.com/repos/" + owner + "/" + repo + "/releases/" +
                      (version == "latest" ? "latest" : ("tags/" + version));
    auto body = kuli::http::fetch_text(api);
    if (!body) return std::unexpected(body.error());

    json j = json::parse(*body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("assets") || !j["assets"].is_array()) {
        return std::unexpected(err("github: no release assets for " + owner + "/" + repo + "@" +
                                   version));
    }
    std::vector<Asset> assets;
    for (const auto& a : j["assets"]) {
        if (!a.is_object()) continue;
        assets.push_back(Asset{a.value("name", ""), a.value("browser_download_url", "")});
    }

    auto chosen = select_asset(assets, asset_pattern, target);
    if (!chosen || chosen->url.empty()) {
        return std::unexpected(err("github: no asset matching '" + asset_pattern + "' for " +
                                   target + " in " + owner + "/" + repo + "@" + version));
    }
    return chosen->url;
}

std::expected<void, Diagnostic> resolve_urls(json& ir) {
    if (!ir.contains("node") || !ir["node"].contains("derivations")) return {};
    for (auto& d : ir["node"]["derivations"]) {
        if (d.value("builder", "") != "fetch") continue;
        auto& f = d["fetch"];
        auto url = resolve_github_asset(f.value("owner", ""), f.value("repo", ""),
                                        f.value("version", ""), f.value("assetPattern", ""),
                                        d.value("systemTarget", ""));
        if (!url) return std::unexpected(url.error());
        f["url"] = *url;
    }
    return {};
}

}  // namespace kuli::bp
