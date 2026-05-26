#include "kuli/platform/paths.hpp"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace kuli::platform::paths {

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Raw environment read. Var names are ASCII; values keep native encoding
// (wide on Windows -> fs::path stores them losslessly, no UTF-8 round-trip).
std::optional<fs::path> raw_env_path(std::string_view var) {
#if defined(_WIN32)
    std::wstring wname(var.begin(), var.end());  // ASCII name
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    if (got == 0) return std::nullopt;
    buf.resize(got);
    if (buf.empty()) return std::nullopt;
    return fs::path(buf);
#else
    std::string name(var);
    const char* v = std::getenv(name.c_str());
    if (!v || !*v) return std::nullopt;
    return fs::path(v);
#endif
}

// Expand a leading "~" (literal "~" or "~/..."), mirroring Path.expanduser.
fs::path expanduser(const std::string& s) {
    if (s.empty() || s[0] != '~') return fs::path(s);
    if (s.size() == 1) return home();
    if (s[1] == '/' || s[1] == '\\') return home() / fs::path(s.substr(2));
    return fs::path(s);
}

// Resolve a canonical home: XDG var (with /kuli suffix) wins everywhere;
// otherwise the platform default.
fs::path resolve(std::string_view xdg_var, const fs::path& fallback) {
    if (auto p = from_env(xdg_var)) return *p / std::string(APP_NAME);
    return fallback;
}

#if defined(_WIN32)
fs::path localappdata() {
    if (auto v = raw_env_path("LOCALAPPDATA")) return *v;
    return home() / "AppData" / "Local";
}
#endif

}  // namespace

std::optional<fs::path> from_env(std::string_view var) {
    auto raw = raw_env_path(var);
    if (!raw) return std::nullopt;
    // Trim using the UTF-8 view for portability; rebuild from the trimmed form.
    std::string s = trim(raw->string());
    if (s.empty()) return std::nullopt;
    return expanduser(s);
}

fs::path current_exe() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return fs::path(buf);
#elif defined(__linux__)
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
#else
    return {};  // macOS exe-path resolution deferred (cross-platform low priority)
#endif
}

fs::path home() {
#if defined(_WIN32)
    if (auto p = raw_env_path("USERPROFILE")) return *p;
    auto drive = raw_env_path("HOMEDRIVE");
    auto path = raw_env_path("HOMEPATH");
    if (drive && path) return fs::path(drive->wstring() + path->wstring());
    return fs::path("C:\\");
#else
    if (auto p = raw_env_path("HOME")) return *p;
    return fs::current_path();
#endif
}

fs::path data_dir() {
#if defined(_WIN32)
    return resolve("XDG_DATA_HOME", localappdata() / std::string(APP_NAME));
#else
    return resolve("XDG_DATA_HOME", home() / ".local" / "share" / std::string(APP_NAME));
#endif
}

fs::path config_dir() {
#if defined(_WIN32)
    return resolve("XDG_CONFIG_HOME", localappdata() / std::string(APP_NAME) / "Config");
#else
    return resolve("XDG_CONFIG_HOME", home() / ".config" / std::string(APP_NAME));
#endif
}

fs::path state_dir() {
#if defined(_WIN32)
    return resolve("XDG_STATE_HOME", localappdata() / std::string(APP_NAME) / "State");
#else
    return resolve("XDG_STATE_HOME", home() / ".local" / "state" / std::string(APP_NAME));
#endif
}

fs::path cache_dir() {
#if defined(_WIN32)
    return resolve("XDG_CACHE_HOME", localappdata() / std::string(APP_NAME) / "Cache");
#else
    return resolve("XDG_CACHE_HOME", home() / ".cache" / std::string(APP_NAME));
#endif
}

fs::path store_dir() { return data_dir() / "store"; }
fs::path identity_dir() { return data_dir() / "identity"; }
fs::path profiles_dir() { return data_dir() / "profiles"; }
fs::path profile_dir(std::string_view name) { return profiles_dir() / std::string(name); }

// ~/.local/bin everywhere (co-located with uv / pipx / claude-code), even on
// Windows — POSIX-flavored tools hardcode it regardless of XDG_BIN_HOME.
fs::path xdg_bin_home() { return home() / ".local" / "bin"; }

fs::path bp_sources_dir() { return data_dir() / "bp_sources"; }
fs::path bp_sources_dir(std::string_view name) { return bp_sources_dir() / std::string(name); }
fs::path bp_sources_config_dir() { return config_dir() / "bp-sources"; }
fs::path backends_config_dir() { return config_dir() / "backends"; }
fs::path downloads_dir() { return cache_dir() / "downloads"; }
fs::path lock_path() { return config_dir() / "kuli.lock"; }

fs::path sessions_dir(const fs::path& cwd) { return cwd / ".kuli" / "sessions"; }
fs::path session_dir(const fs::path& cwd, std::string_view id) {
    return sessions_dir(cwd) / std::string(id);
}

void ensure_dirs() {
    std::error_code ec;
    for (const auto& p : {data_dir(), config_dir(), state_dir(), cache_dir(),
                          store_dir(), identity_dir(), profiles_dir(),
                          xdg_bin_home(), bp_sources_dir(), bp_sources_config_dir(),
                          backends_config_dir(), downloads_dir()}) {
        fs::create_directories(p, ec);
    }
}

}  // namespace kuli::platform::paths
