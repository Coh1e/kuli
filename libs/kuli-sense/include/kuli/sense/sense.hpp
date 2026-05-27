#pragma once
// kuli-sense — native, cross-platform host/process facts (design §3:
// "跨平台 ps/ip/lsof/host facts native API"). Dependency-free: Win32 Toolhelp /
// Linux /proc / macOS sysctl, confined to src/sense.cpp behind #ifdef (R-NF-05).
// Feeds the read IRs ProcessQuery + HostFacts (NetworkQuery / HandleQuery later).

#include <string>
#include <utility>
#include <vector>

namespace kuli::sense {

struct ProcessInfo {
    long pid = 0;
    long ppid = 0;
    std::string name;  // executable basename (comm)
};

// Best-effort process list; empty on failure or an unsupported platform.
std::vector<ProcessInfo> list_processes();

struct HostFacts {
    std::string os;    // "windows" / "linux" / "macos"
    std::string arch;  // "x64" / "arm64" / ...
    std::string hostname;
    unsigned cpu_count = 0;
};

HostFacts host_facts();

// The process environment as (name, value) pairs (unsorted; caller sorts).
std::vector<std::pair<std::string, std::string>> env_vars();

}  // namespace kuli::sense
