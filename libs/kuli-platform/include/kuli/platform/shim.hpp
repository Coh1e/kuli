#pragma once
// PATH-launchable shims in ~/.local/bin. Shared by the engine executor (apply)
// and generation activation (rollback/switch). M0 form: a .cmd on Windows
// (PATHEXT picks it up), a chmod +x shell wrapper on POSIX. Real-exe / smart /
// junction variants (devkit §4.2-4.4) are a later refinement.

#include <filesystem>
#include <string>

namespace kuli::platform {

namespace fs = std::filesystem;

// Write a shim named <alias> that forwards all args to <target>.
// Returns false if the shim could not be written (so callers can surface a
// partial activation instead of reporting false success — M-2).
[[nodiscard]] bool write_shim(const fs::path& bin_dir, const std::string& alias,
                              const fs::path& target);

// Remove the shim for <alias>. Returns true if a file was deleted.
bool remove_shim(const fs::path& bin_dir, const std::string& alias);

}  // namespace kuli::platform
