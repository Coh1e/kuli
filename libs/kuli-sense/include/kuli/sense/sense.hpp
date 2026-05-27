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

struct SocketInfo {
    std::string proto;        // "tcp"
    std::string local_addr;   // "127.0.0.1:8080"
    std::string remote_addr;  // "0.0.0.0:0"
    std::string state;        // "LISTEN" / "ESTABLISHED" / ...
    long pid = 0;             // owning pid (Windows; 0 if unknown, e.g. Linux v0)
};

// Best-effort TCP socket table (IPv4). Windows = iphlpapi; Linux = /proc/net/tcp;
// macOS = empty for now (sysctl pcblist deferred).
std::vector<SocketInfo> list_sockets();

}  // namespace kuli::sense
