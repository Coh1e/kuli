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

// `kuli scripture ls` — list scriptures installed by the current generation and
// the basenames they provide. Returns 0.
int scripture_list();

}  // namespace kuli::bp
