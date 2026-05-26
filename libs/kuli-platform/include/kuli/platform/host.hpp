#pragma once
// kuli-platform/host — tiny host-detection helper used to match a derivation's
// `system_target` and to report `kuli version`. Absorbs ref/luban/src/platform.
// Richer host facts (HostFacts IR) are a separate, later subsystem.

#include <string>
#include <string_view>

namespace kuli::platform {

// "windows" / "linux" / "macos" / "unknown". Matches ctx.system.os in blueprints.
[[nodiscard]] std::string_view host_os();

// "x64" / "arm64" / "x86" / "unknown".
[[nodiscard]] std::string_view host_arch();

// "<os>-<arch>", e.g. "windows-x64". The key into a derivation's per-target
// download table (§8.1 system_target).
[[nodiscard]] std::string host_triplet();

}  // namespace kuli::platform
