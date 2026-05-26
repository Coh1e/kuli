#include "kuli/bp/source.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "kuli/http/download.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/store/archive.hpp"

namespace kuli::bp {

namespace {
using kuli::diag::Diagnostic;

Diagnostic err(std::string msg, std::string code = "E0800") {
    return Diagnostic::error(std::move(msg), std::move(code));
}

std::string now_epoch() {
    auto t = std::chrono::system_clock::now();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

std::string trim(std::string s) {
    auto sp = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && sp(s.front())) s.erase(s.begin());
    while (!s.empty() && sp(s.back())) s.pop_back();
    return s;
}

// --- flat TOML (key = "value" / key = true) -------------------------------
std::map<std::string, std::string> parse_flat(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == '[') continue;
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            std::string inner = val.substr(1, val.size() - 2);
            std::string out;  // unescape \\ and \" (mirror toml_quote)
            for (std::size_t i = 0; i < inner.size(); ++i) {
                if (inner[i] == '\\' && i + 1 < inner.size()) {
                    out.push_back(inner[++i]);
                } else {
                    out.push_back(inner[i]);
                }
            }
            val = out;
        }
        kv[key] = val;
    }
    return kv;
}

std::string toml_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// --- github URL parsing ----------------------------------------------------
struct GhRef {
    std::string owner, repo;
};

std::optional<GhRef> parse_github(std::string_view url) {
    std::string u(url);
    const std::string pfx_scheme = "github:";
    const std::string pfx_https = "https://github.com/";
    std::string rest;
    if (u.rfind(pfx_scheme, 0) == 0) {
        rest = u.substr(pfx_scheme.size());
    } else if (u.rfind(pfx_https, 0) == 0) {
        rest = u.substr(pfx_https.size());
    } else if (u.find("://") == std::string::npos && u.find('/') != std::string::npos &&
               !std::filesystem::exists(u) && u.front() != '/' && u.front() != '\\' &&
               u.front() != '.' && !(u.size() >= 2 && u[1] == ':')) {
        rest = u;  // "owner/repo" shorthand (not an absolute/relative/drive path)
    } else {
        return std::nullopt;
    }
    if (rest.size() > 4 && rest.substr(rest.size() - 4) == ".git") rest.erase(rest.size() - 4);
    auto slash = rest.find('/');
    if (slash == std::string::npos) return std::nullopt;
    GhRef g{rest.substr(0, slash), rest.substr(slash + 1)};
    // trim any trailing path after owner/repo
    auto more = g.repo.find('/');
    if (more != std::string::npos) g.repo = g.repo.substr(0, more);
    if (g.owner.empty() || g.repo.empty()) return std::nullopt;
    return g;
}

const std::set<std::string>& official_owners() {
    static const std::set<std::string> s = {"kuli-lang"};  // bootstrap allowlist
    return s;
}
}  // namespace

bool SourceEntry::is_local() const {
    return !parse_github(url).has_value();
}

fs::path SourceRegistry::entry_path(std::string_view name) const {
    return config_dir_ / (std::string(name) + ".toml");
}

std::optional<SourceEntry> SourceRegistry::read_entry(const fs::path& p) const {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto kv = parse_flat(ss.str());
    SourceEntry e;
    e.name = p.stem().string();
    e.url = kv["url"];
    e.ref = kv["ref"];
    e.commit = kv["commit"];
    e.added_at = kv["added_at"];
    e.official = kv["official"] == "true";
    if (e.url.empty()) return std::nullopt;
    return e;
}

std::expected<void, Diagnostic> SourceRegistry::write_entry(const SourceEntry& e) const {
    std::error_code ec;
    fs::create_directories(config_dir_, ec);
    fs::path tmp = entry_path(e.name);
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(err("cannot write source registry entry: " + e.name));
        out << "url = " << toml_quote(e.url) << "\n";
        out << "ref = " << toml_quote(e.ref) << "\n";
        out << "commit = " << toml_quote(e.commit) << "\n";
        out << "added_at = " << toml_quote(e.added_at) << "\n";
        out << "official = " << (e.official ? "true" : "false") << "\n";
    }
    fs::rename(tmp, entry_path(e.name), ec);
    if (ec) return std::unexpected(err("cannot publish source registry entry: " + ec.message()));
    return {};
}

std::vector<SourceEntry> SourceRegistry::list() const {
    std::vector<SourceEntry> out;
    std::error_code ec;
    if (!fs::exists(config_dir_, ec)) return out;
    for (const auto& de : fs::directory_iterator(config_dir_, ec)) {
        if (de.path().extension() != ".toml") continue;
        if (auto e = read_entry(de.path())) out.push_back(*e);
    }
    std::sort(out.begin(), out.end(),
              [](const SourceEntry& a, const SourceEntry& b) { return a.name < b.name; });
    return out;
}

std::optional<SourceEntry> SourceRegistry::find(std::string_view name) const {
    return read_entry(entry_path(name));
}

fs::path SourceRegistry::source_root(const SourceEntry& e) const {
    if (e.is_local()) return fs::path(e.url);  // live-linked path
    return data_dir_ / e.name;                 // synced copy
}

namespace {
// Fetch + extract a github repo zip (zipball follows a redirect to codeload)
// into `dest`. Returns the resolved commit sha (best-effort).
std::expected<std::string, Diagnostic> sync_github(const GhRef& gh, const std::string& ref,
                                                   const fs::path& dest) {
    std::string zipball = "https://api.github.com/repos/" + gh.owner + "/" + gh.repo + "/zipball" +
                          (ref.empty() ? "" : ("/" + ref));
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path() / (gh.repo + "-kuli-src.zip");
    fs::create_directories(tmp.parent_path(), ec);
    if (auto dl = kuli::http::fetch_to_file(zipball, tmp); !dl) return std::unexpected(dl.error());

    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    auto ex = kuli::archive::extract(tmp, dest);
    fs::remove(tmp, ec);
    if (!ex) return std::unexpected(ex.error());

    // Best-effort commit sha.
    std::string commits = "https://api.github.com/repos/" + gh.owner + "/" + gh.repo + "/commits/" +
                          (ref.empty() ? "HEAD" : ref);
    if (auto body = kuli::http::fetch_text(commits)) {
        auto j = nlohmann::json::parse(*body, nullptr, false);
        if (!j.is_discarded() && j.contains("sha") && j["sha"].is_string()) {
            return j["sha"].get<std::string>();
        }
    }
    return std::string("tarball:") + now_epoch();
}
}  // namespace

std::expected<SourceEntry, Diagnostic> SourceRegistry::add(std::string_view url_or_path,
                                                           std::optional<std::string> name,
                                                           bool /*assume_yes*/) {
    SourceEntry e;
    e.added_at = now_epoch();

    if (auto gh = parse_github(url_or_path)) {
        e.url = "github:" + gh->owner + "/" + gh->repo;
        e.name = name.value_or(gh->repo);
        e.official = official_owners().count(gh->owner) > 0;
        auto sha = sync_github(*gh, e.ref, source_root(e));
        if (!sha) return std::unexpected(sha.error());
        e.commit = *sha;
    } else {
        // Local path source: live-linked, not copied.
        fs::path p(url_or_path);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            return std::unexpected(err("source path does not exist: " + p.string()));
        }
        p = fs::absolute(p, ec);
        if (!fs::exists(p / "blueprints", ec)) {
            return std::unexpected(
                err("local source has no blueprints/ directory: " + p.string()));
        }
        e.url = p.string();
        e.name = name.value_or(p.filename().string());
        e.official = false;
        e.commit = "local:" + e.added_at;
    }

    if (auto w = write_entry(e); !w) return std::unexpected(w.error());
    return e;
}

std::expected<void, Diagnostic> SourceRegistry::remove(std::string_view name) {
    auto e = find(name);
    if (!e) return std::unexpected(err("no such source: " + std::string(name)));
    std::error_code ec;
    fs::remove(entry_path(name), ec);
    if (!e->is_local()) fs::remove_all(data_dir_ / std::string(name), ec);  // drop synced copy
    return {};
}

std::expected<SourceEntry, Diagnostic> SourceRegistry::update(std::string_view name) {
    auto e = find(name);
    if (!e) return std::unexpected(err("no such source: " + std::string(name)));
    if (e->is_local()) return *e;  // live-linked; nothing to fetch
    auto gh = parse_github(e->url);
    if (!gh) return std::unexpected(err("source is not a github url: " + e->url));
    auto sha = sync_github(*gh, e->ref, source_root(*e));
    if (!sha) return std::unexpected(sha.error());
    e->commit = *sha;
    if (auto w = write_entry(*e); !w) return std::unexpected(w.error());
    return *e;
}

std::expected<SourceRegistry::Resolved, Diagnostic> SourceRegistry::resolve(
    std::string_view spec) const {
    std::string s(spec);
    auto blueprint_in = [&](const SourceEntry& e,
                            const std::string& bp) -> std::optional<Resolved> {
        fs::path root = source_root(e);
        fs::path file = root / "blueprints" / (bp + ".luau");
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) return Resolved{file, root, e.name};
        return std::nullopt;
    };

    auto slash = s.find('/');
    if (slash != std::string::npos) {  // "<source>/<name>"
        std::string src = s.substr(0, slash);
        std::string bp = s.substr(slash + 1);
        auto e = find(src);
        if (!e) return std::unexpected(err("no such source: " + src));
        if (auto r = blueprint_in(*e, bp)) return *r;
        return std::unexpected(err("blueprint '" + bp + "' not found in source '" + src + "'"));
    }

    // bare name: search all sources.
    std::vector<Resolved> hits;
    for (const auto& e : list()) {
        if (auto r = blueprint_in(e, s)) hits.push_back(*r);
    }
    if (hits.empty()) {
        return std::unexpected(err("blueprint '" + s + "' not found in any registered source")
                                   .with_help("register one with `kuli bp src add <url|path>`"));
    }
    if (hits.size() > 1) {
        return std::unexpected(err("blueprint '" + s + "' is ambiguous across sources")
                                   .with_help("qualify it as <source>/" + s));
    }
    return hits.front();
}

SourceRegistry default_registry() {
    return SourceRegistry{kuli::platform::paths::bp_sources_config_dir(),
                          kuli::platform::paths::bp_sources_dir()};
}

std::expected<fs::path, Diagnostic> resolve_blueprint_spec(std::string_view spec) {
    fs::path as_path(spec);
    std::error_code ec;
    if (fs::is_regular_file(as_path, ec)) return as_path;  // direct file path
    auto r = default_registry().resolve(spec);
    if (!r) return std::unexpected(r.error());
    return r->blueprint_file;
}

}  // namespace kuli::bp
