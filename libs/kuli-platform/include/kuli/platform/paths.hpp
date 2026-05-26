#pragma once
// kuli-platform/paths — XDG-first directory resolution (R-NF-01).
//
// Absorbs ref/luban/src/paths.{hpp,cpp}, but APP_NAME = "kuli" and the layout
// adds the profile/generation + evidence-session locations kuli needs that
// luban lacked. Persist locations match docs/concepts.md:
//
//   identity   ~/.local/share/kuli/identity/
//   store      ~/.local/share/kuli/store/<hash16>-<name>/
//   profiles   ~/.local/share/kuli/profiles/<name>/{current, generations/N.json}
//   bp sources (synced)  ~/.local/share/kuli/bp_sources/<name>/
//   bp sources (registry) ~/.config/kuli/bp-sources/<name>.toml
//   backend config        ~/.config/kuli/backends/<name>.toml
//   shims      ~/.local/bin/<basename>
//   sessions   <cwd>/.kuli/sessions/<id>/   (evidence; per-invocation cwd)
//
// Resolution honors XDG_*_HOME everywhere (power users override on Windows
// too); Windows falls back to %LOCALAPPDATA%, POSIX to the XDG defaults.

#include <filesystem>
#include <optional>
#include <string_view>

namespace kuli::platform::paths {

namespace fs = std::filesystem;

inline constexpr std::string_view APP_NAME = "kuli";

// Env var read as a path (whitespace-trimmed, ~ expanded). Empty -> nullopt.
std::optional<fs::path> from_env(std::string_view var);

fs::path home();

// Absolute path of the running kuli executable (for writing basename shims).
fs::path current_exe();

// The four canonical homes (XDG-first).
fs::path data_dir();
fs::path config_dir();
fs::path state_dir();
fs::path cache_dir();

// Derived locations.
fs::path store_dir();                         // <data>/store
fs::path identity_dir();                       // <data>/identity
fs::path profiles_dir();                       // <data>/profiles
fs::path profile_dir(std::string_view name);   // <data>/profiles/<name>
fs::path xdg_bin_home();                        // ~/.local/bin (shims)
fs::path bp_sources_dir();                      // <data>/bp_sources (synced copies)
fs::path bp_sources_dir(std::string_view name); // <data>/bp_sources/<name>
fs::path bp_sources_config_dir();               // <config>/bp-sources (registry)
fs::path backends_config_dir();                 // <config>/backends
fs::path downloads_dir();                       // <cache>/downloads
fs::path lock_path();                           // <config>/kuli.lock

// Evidence sessions live under the invocation's cwd, not a global home.
fs::path sessions_dir(const fs::path& cwd);     // <cwd>/.kuli/sessions
fs::path session_dir(const fs::path& cwd, std::string_view id);

// Create the global directories that must exist before first use.
void ensure_dirs();

}  // namespace kuli::platform::paths
