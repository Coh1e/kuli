#pragma once
// Blueprint source registry — register/sync repos of blueprints and resolve a
// blueprint by name. Per-source metadata lives at
// ~/.config/kuli/bp-sources/<name>.toml; a github source is synced (codeload
// .zip via kuli-http + miniz) to ~/.local/share/kuli/bp_sources/<name>/, while
// a local path is live-linked (no copy). Flat-TOML, no TOML library (the files
// are flat key=value — keeps kuli dependency-light).

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kuli/diag/diagnostic.hpp"

namespace kuli::bp {

namespace fs = std::filesystem;

struct SourceEntry {
    std::string name;
    std::string url;       // "github:owner/repo" / "https://github.com/owner/repo" / local path
    std::string ref;       // branch/tag/sha; empty = repo default
    std::string commit;    // resolved sha, or "tarball:<ts>" / "local:<ts>"
    std::string added_at;  // epoch seconds
    bool official = false;

    bool is_local() const;  // a filesystem path / file: URL (live-linked)
};

class SourceRegistry {
   public:
    SourceRegistry(fs::path config_dir, fs::path data_dir)
        : config_dir_(std::move(config_dir)), data_dir_(std::move(data_dir)) {}

    std::vector<SourceEntry> list() const;
    std::optional<SourceEntry> find(std::string_view name) const;

    // Register a source from a github URL/shorthand or a local path, fetching
    // (github) or live-linking (local). `name` defaults to the repo/dir base.
    std::expected<SourceEntry, kuli::diag::Diagnostic> add(
        std::string_view url_or_path, std::optional<std::string> name, bool assume_yes);

    std::expected<void, kuli::diag::Diagnostic> remove(std::string_view name);
    std::expected<SourceEntry, kuli::diag::Diagnostic> update(std::string_view name);

    // The on-disk root of a source (synced dir for github; the live path for local).
    fs::path source_root(const SourceEntry& e) const;

    struct Resolved {
        fs::path blueprint_file;
        fs::path source_root;
        std::string source_name;
    };
    // Resolve "<name>" or "<source>/<name>" to a blueprint file in a registered
    // source (bare name searches all sources; ambiguity is an error).
    std::expected<Resolved, kuli::diag::Diagnostic> resolve(std::string_view spec) const;

   private:
    fs::path entry_path(std::string_view name) const;
    std::optional<SourceEntry> read_entry(const fs::path& p) const;
    std::expected<void, kuli::diag::Diagnostic> write_entry(const SourceEntry& e) const;

    fs::path config_dir_;
    fs::path data_dir_;
};

SourceRegistry default_registry();

// Resolve an apply/explain argument that may be either a direct .luau file path
// or a blueprint name (resolved against the default registry).
std::expected<fs::path, kuli::diag::Diagnostic> resolve_blueprint_spec(std::string_view spec);

}  // namespace kuli::bp
