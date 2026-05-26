#pragma once
// User-scoped environment mutation (R-NF-01: HKCU only, never HKLM, never
// elevate). On Windows this writes HKCU\Environment and broadcasts
// WM_SETTINGCHANGE; on POSIX it is a no-op for now (shell-rc lands later).

#include <string>

namespace kuli::platform {

// Prepend `entry` (a leading ~ is expanded to the home dir) to the user PATH.
// Idempotent: returns false without writing if the entry is already present.
bool hkcu_path_prepend(const std::string& entry);

}  // namespace kuli::platform
