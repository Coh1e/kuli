#include "kuli/sense/sense.hpp"

#include <thread>

#include "kuli/platform/host.hpp"  // host_os / host_arch

#if defined(_WIN32)
#include <windows.h>
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

}  // namespace kuli::sense
