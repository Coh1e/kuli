#include "kuli/store/store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "kuli/http/download.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/store/archive.hpp"

namespace kuli::store {

namespace {

using kuli::diag::Diagnostic;
using kuli::diag::Kind;

constexpr const char* kMarker = ".kuli-store-marker.json";

// The sha256 recorded in a store path's marker, or nullopt if the marker is
// absent / unreadable / lacks the field (treated as "no valid marker").
std::optional<std::string> read_marker_sha(const fs::path& dir) {
    std::ifstream in(dir / kMarker, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("sha256") || !j["sha256"].is_string()) {
        return std::nullopt;
    }
    return j["sha256"].get<std::string>();
}

// Cache state for a store path given the content we intend to realize there.
// `Hit` = present with a matching marker; `Collision` = present with a marker
// recording *different* content (64-bit hash-prefix + name clash); `Absent` =
// nothing usable (missing, or a markerless/corrupt incomplete partial).
enum class CacheState { Absent, Hit, Collision };

CacheState check_cache(const fs::path& dir, const kuli::crypto::HashSpec& expected) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return CacheState::Absent;
    auto sha = read_marker_sha(dir);
    if (!sha) return CacheState::Absent;  // markerless / corrupt -> re-realize
    return *sha == expected.hex ? CacheState::Hit : CacheState::Collision;
}

Diagnostic collision_error(const fs::path& dir) {
    return Diagnostic::error(
               "store path collision at " + dir.string() +
                   " (different content shares this hash-prefix + name)",
               "E0233")
        .with_help("astronomically rare; remove the path and retry, or report it");
}

std::string now_iso() {
    auto t = std::chrono::system_clock::now();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

std::string url_ext(std::string_view url) {
    auto q = url.find('?');
    std::string_view path = url.substr(0, q == std::string_view::npos ? url.size() : q);
    auto slash = path.find_last_of('/');
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot == std::string_view::npos) return ".archive";
    return std::string(base.substr(dot));
}

}  // namespace

fs::path Store::store_path(std::string_view hash16, std::string_view name) const {
    return root / (std::string(hash16) + "-" + std::string(name));
}

bool Store::is_present(const fs::path& store_dir) const {
    std::error_code ec;
    return fs::exists(store_dir, ec) && fs::exists(store_dir / kMarker, ec);
}

std::expected<FetchResult, Diagnostic> Store::realize_from_archive(
    std::string_view hash16, std::string_view name, const fs::path& archive,
    const kuli::crypto::HashSpec& expected, std::string_view bin) const {
    fs::path dir = store_path(hash16, name);
    auto make_result = [&](bool present) {
        FetchResult r;
        r.store_dir = dir;
        r.bin_path = bin.empty() ? dir : dir / fs::path(bin);
        r.was_already_present = present;
        return r;
    };

    switch (check_cache(dir, expected)) {
        case CacheState::Hit:       return make_result(true);
        case CacheState::Collision: return std::unexpected(collision_error(dir));
        case CacheState::Absent:    break;
    }

    // Verify the archive's content hash (it is a derivation *input*, §8.2.1).
    if (!kuli::crypto::verify_file(archive, expected)) {
        auto actual = kuli::crypto::hash_file(archive, expected.algo);
        return std::unexpected(
            Diagnostic::error("archive sha256 mismatch for " + std::string(name), "E0230")
                .with_help("expected " + expected.hex + ", got " +
                           (actual ? actual->hex : std::string("<unreadable>"))));
    }

    std::error_code ec;
    fs::create_directories(root, ec);
    // H-3: a markerless/corrupt dir at the target is a known-incomplete partial
    // (e.g. an interrupted earlier realize) — remove it so the publish rename
    // doesn't fail forever on Windows (where rename onto an existing dir errors).
    if (fs::exists(dir, ec)) fs::remove_all(dir, ec);
    fs::path tmp = root / ("." + std::string(hash16) + "-" + std::string(name) + ".tmp");
    fs::remove_all(tmp, ec);

    auto extracted = kuli::archive::extract(archive, tmp);
    if (!extracted) {
        fs::remove_all(tmp, ec);
        return std::unexpected(extracted.error());
    }

    // Provenance marker, written before the atomic publish.
    nlohmann::json marker{
        {"hash", std::string(hash16)},
        {"name", std::string(name)},
        {"sha256", expected.hex},
        {"realized_at", now_iso()},
    };
    {
        std::ofstream m(tmp / kMarker, std::ios::binary | std::ios::trunc);
        if (!m) {
            fs::remove_all(tmp, ec);
            return std::unexpected(Diagnostic::error("cannot write store marker", "E0231"));
        }
        m << marker.dump(2);
    }

    // Atomic publish. If another process won the race with matching content,
    // discard our tmp and treat it as a hit.
    fs::rename(tmp, dir, ec);
    if (ec) {
        if (check_cache(dir, expected) == CacheState::Hit) {
            fs::remove_all(tmp, ec);
            return make_result(true);
        }
        fs::remove_all(tmp, ec);
        return std::unexpected(Diagnostic::error(
            "failed to publish store path " + dir.string() + ": " + ec.message(), "E0232"));
    }
    return make_result(false);
}

std::expected<FetchResult, Diagnostic> Store::realize_fetch(
    std::string_view hash16, std::string_view name, std::string_view url,
    const kuli::crypto::HashSpec& expected, std::string_view bin) const {
    fs::path dir = store_path(hash16, name);
    switch (check_cache(dir, expected)) {
        case CacheState::Hit: {
            FetchResult r;
            r.store_dir = dir;
            r.bin_path = bin.empty() ? dir : dir / fs::path(bin);
            r.was_already_present = true;
            return r;
        }
        case CacheState::Collision: return std::unexpected(collision_error(dir));
        case CacheState::Absent:    break;
    }

    std::error_code ec;
    fs::create_directories(downloads, ec);
    fs::path archive = downloads / (std::string(hash16) + url_ext(url));

    kuli::http::FetchOptions opts;
    opts.expected = expected;  // verify on download
    auto dl = kuli::http::fetch_to_file(url, archive, opts);
    if (!dl) return std::unexpected(dl.error());

    return realize_from_archive(hash16, name, archive, expected, bin);
}

std::expected<FetchResult, Diagnostic> Store::realize_inline(
    std::string_view store_dir_name, std::string_view content_id,
    const std::vector<std::pair<std::string, std::string>>& files) const {
    fs::path dir = root / std::string(store_dir_name);
    kuli::crypto::HashSpec expected{kuli::crypto::Algorithm::Sha256, std::string(content_id)};
    auto make_result = [&](bool present) {
        FetchResult r;
        r.store_dir = dir;
        r.bin_path = dir;
        r.was_already_present = present;
        return r;
    };

    switch (check_cache(dir, expected)) {
        case CacheState::Hit:       return make_result(true);
        case CacheState::Collision: return std::unexpected(collision_error(dir));
        case CacheState::Absent:    break;
    }

    std::error_code ec;
    fs::create_directories(root, ec);
    if (fs::exists(dir, ec)) fs::remove_all(dir, ec);  // H-3: clear an incomplete partial
    fs::path tmp = root / ("." + std::string(store_dir_name) + ".tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    for (const auto& [rel, content] : files) {
        // Containment: the resolved path must stay within tmp (no "..", absolute).
        fs::path target = (tmp / fs::path(rel)).lexically_normal();
        fs::path rel_to = target.lexically_relative(tmp);
        if (rel_to.empty() || *rel_to.begin() == "..") {
            fs::remove_all(tmp, ec);
            return std::unexpected(
                Diagnostic::error("scripture file escapes its store path: " + rel, "E0234"));
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream o(target, std::ios::binary | std::ios::trunc);
        if (!o || (o.write(content.data(), static_cast<std::streamsize>(content.size())), !o.good())) {
            fs::remove_all(tmp, ec);
            return std::unexpected(Diagnostic::error("cannot write scripture file: " + rel, "E0235"));
        }
    }

    nlohmann::json marker{
        {"hash", std::string(store_dir_name)},
        {"sha256", std::string(content_id)},  // the scripture derivation hash
        {"realized_at", now_iso()},
        {"kind", "scripture"},
    };
    {
        std::ofstream m(tmp / kMarker, std::ios::binary | std::ios::trunc);
        if (!m) {
            fs::remove_all(tmp, ec);
            return std::unexpected(Diagnostic::error("cannot write store marker", "E0231"));
        }
        m << marker.dump(2);
    }

    fs::rename(tmp, dir, ec);
    if (ec) {
        if (check_cache(dir, expected) == CacheState::Hit) {
            fs::remove_all(tmp, ec);
            return make_result(true);
        }
        fs::remove_all(tmp, ec);
        return std::unexpected(Diagnostic::error(
            "failed to publish store path " + dir.string() + ": " + ec.message(), "E0232"));
    }
    return make_result(false);
}

Store default_store() {
    return Store{kuli::platform::paths::store_dir(), kuli::platform::paths::downloads_dir()};
}

}  // namespace kuli::store
