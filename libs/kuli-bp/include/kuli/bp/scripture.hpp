#pragma once
// Scripture basename routing (§9.2). A scripture installs basename shims that
// re-enter kuli as `kuli --basename <name> -- <args>`. This module looks the
// basename up in the current generation, loads the scripture's adapter from its
// store path, runs it in the Luau sandbox, and renders the result.

#include <filesystem>
#include <string>
#include <vector>

namespace kuli::bp {

namespace fs = std::filesystem;

// True if the current generation installs a scripture providing `basename`
// (so the CLI knows to route `kuli <basename> ...` / `--basename <basename>`).
bool is_installed_basename(const std::string& basename);

// Run an installed scripture basename: load `<store>/<storePath>/<adapter>.luau`
// and call function(ctx) with ctx.argv = argv. Renders the adapter's result.
// Returns the process exit code (diagnostics go to stderr).
int run_basename(const std::string& basename, const std::vector<std::string>& argv,
                 const fs::path& cwd);

// `kuli scripture ls` — list installed scriptures (registry + current
// generation) and the basenames they provide. Returns 0.
int scripture_list();

// The scriptures bundled with kuli (e.g. "find", "grep").
std::vector<std::string> builtin_scripture_names();

// `kuli scripture install <name>` for a built-in: realize it into the store,
// write its basename shims, and record it in the scripture registry (additive —
// installing one does not remove another). Returns the exit code.
int install_builtin_scripture(const std::string& name);

// `kuli scripture uninstall <name>`: remove its basename shims + registry entry.
int uninstall_scripture(const std::string& name);

}  // namespace kuli::bp
