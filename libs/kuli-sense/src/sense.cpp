#include "kuli/sense/sense.hpp"

#include <thread>

#include "kuli/platform/host.hpp"  // host_os / host_arch

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>   // after windows.h (WIN32_LEAN_AND_MEAN keeps winsock1 out)
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <unistd.h>
#include <dirent.h>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#elif defined(__APPLE__)
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif

#if !defined(_WIN32)
// Available to the main executable (kuli-sense is statically linked into it;
// the dylib-only restriction that would need _NSGetEnviron does not apply).
extern "C" char** environ;
#endif

namespace kuli::sense {

namespace {

#if defined(_WIN32)
// Narrow a wide Win32 string to UTF-8 (exe names / hostname).
std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* win_tcp_state(DWORD st) {
    switch (st) {
        case MIB_TCP_STATE_CLOSED:     return "CLOSED";
        case MIB_TCP_STATE_LISTEN:     return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT:   return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD:   return "SYN_RCVD";
        case MIB_TCP_STATE_ESTAB:      return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1:  return "FIN_WAIT1";
        case MIB_TCP_STATE_FIN_WAIT2:  return "FIN_WAIT2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING:    return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK:   return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT:  return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
        default:                       return "UNKNOWN";
    }
}

std::string win_addr(DWORD addr, DWORD port) {
    const auto* b = reinterpret_cast<const unsigned char*>(&addr);  // network order in memory
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
                  ntohs(static_cast<u_short>(port)));
    return buf;
}
#elif defined(__linux__)
const char* linux_tcp_state(const std::string& hex) {
    static const char* names[] = {"",          "ESTABLISHED", "SYN_SENT",  "SYN_RECV",
                                  "FIN_WAIT1", "FIN_WAIT2",   "TIME_WAIT", "CLOSE",
                                  "CLOSE_WAIT", "LAST_ACK",   "LISTEN",    "CLOSING"};
    long v = std::strtol(hex.c_str(), nullptr, 16);
    return (v >= 1 && v <= 11) ? names[v] : "UNKNOWN";
}

// "0100007F:1F90" -> "127.0.0.1:8080" (hex IP is little-endian octets; hex port).
std::string linux_addr(const std::string& tok) {
    auto colon = tok.find(':');
    if (colon == std::string::npos) return tok;
    unsigned long ip = std::strtoul(tok.substr(0, colon).c_str(), nullptr, 16);
    unsigned long port = std::strtoul(tok.substr(colon + 1).c_str(), nullptr, 16);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu:%lu", ip & 0xFF, (ip >> 8) & 0xFF,
                  (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port);
    return buf;
}
#endif

}  // namespace

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            ProcessInfo p;
            p.pid = static_cast<long>(e.th32ProcessID);
            p.ppid = static_cast<long>(e.th32ParentProcessID);
            p.name = narrow(e.szExeFile);
            out.push_back(std::move(p));
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
#elif defined(__linux__)
    DIR* d = opendir("/proc");
    if (!d) return out;
    for (struct dirent* de = readdir(d); de != nullptr; de = readdir(d)) {
        const char* n = de->d_name;
        bool numeric = *n != '\0';
        for (const char* c = n; *c; ++c) {
            if (!std::isdigit(static_cast<unsigned char>(*c))) { numeric = false; break; }
        }
        if (!numeric) continue;

        ProcessInfo p;
        p.pid = std::atol(n);
        {
            std::ifstream in(std::string("/proc/") + n + "/comm");
            std::getline(in, p.name);
        }
        {
            // /proc/<pid>/stat: "pid (comm) state ppid ..."; comm may contain
            // spaces/parens, so read the fields after the last ')'.
            std::ifstream in(std::string("/proc/") + n + "/stat");
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto rp = s.rfind(')');
            if (rp != std::string::npos) {
                std::istringstream ss(s.substr(rp + 1));
                std::string state;
                long ppid = 0;
                ss >> state >> ppid;
                p.ppid = ppid;
            }
        }
        out.push_back(std::move(p));
    }
    closedir(d);
#elif defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    std::size_t len = 0;
    if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0) return out;
    std::vector<char> buf(len);
    if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) != 0) return out;
    const auto* procs = reinterpret_cast<const struct kinfo_proc*>(buf.data());
    std::size_t count = len / sizeof(struct kinfo_proc);
    for (std::size_t i = 0; i < count; ++i) {
        ProcessInfo p;
        p.pid = procs[i].kp_proc.p_pid;
        p.ppid = procs[i].kp_eproc.e_ppid;
        p.name = procs[i].kp_proc.p_comm;
        out.push_back(std::move(p));
    }
#endif
    return out;
}

HostFacts host_facts() {
    HostFacts f;
    f.os = std::string(kuli::platform::host_os());
    f.arch = std::string(kuli::platform::host_arch());
    f.cpu_count = std::thread::hardware_concurrency();
#if defined(_WIN32)
    wchar_t name[256];
    DWORD sz = 256;
    if (GetComputerNameW(name, &sz)) f.hostname = narrow(name);
#else
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        f.hostname = host;
    }
#endif
    return f;
}

std::vector<std::pair<std::string, std::string>> env_vars() {
    std::vector<std::pair<std::string, std::string>> out;
#if defined(_WIN32)
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return out;
    for (LPWCH p = block; *p;) {
        std::wstring entry = p;
        p += entry.size() + 1;
        std::string e = narrow(entry.c_str());
        if (e.empty() || e[0] == '=') continue;  // skip "=C:=..." drive entries
        auto eq = e.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(e.substr(0, eq), e.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
#else
    if (environ) {
        for (char** ep = environ; *ep; ++ep) {
            std::string e = *ep;
            auto eq = e.find('=');
            if (eq == std::string::npos) continue;
            out.emplace_back(e.substr(0, eq), e.substr(eq + 1));
        }
    }
#endif
    return out;
}

std::vector<SocketInfo> list_sockets() {
    std::vector<SocketInfo> out;
#if defined(_WIN32)
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return out;
    std::vector<char> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) !=
        NO_ERROR) {
        return out;
    }
    const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const auto& row = t->table[i];
        SocketInfo s;
        s.proto = "tcp";
        s.local_addr = win_addr(row.dwLocalAddr, row.dwLocalPort);
        // a remote endpoint is only meaningful when connected (port is 0 when not)
        s.remote_addr = win_addr(row.dwRemoteAddr, row.dwState == MIB_TCP_STATE_LISTEN ? 0
                                                                                       : row.dwRemotePort);
        s.state = win_tcp_state(row.dwState);
        s.pid = static_cast<long>(row.dwOwningPid);
        out.push_back(std::move(s));
    }
#elif defined(__linux__)
    std::ifstream in("/proc/net/tcp");
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string sl, local, rem, st;
        ss >> sl >> local >> rem >> st;
        if (local.empty() || rem.empty()) continue;
        SocketInfo s;
        s.proto = "tcp";
        s.local_addr = linux_addr(local);
        s.remote_addr = linux_addr(rem);
        s.state = linux_tcp_state(st);
        s.pid = 0;  // /proc/net/tcp gives an inode, not a pid (mapping deferred)
        out.push_back(std::move(s));
    }
#endif
    // macOS: empty for now (sysctl pcblist deferred).
    return out;
}

}  // namespace kuli::sense
