#include "kuli/platform/win_env.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include "kuli/platform/env.hpp"
#include "kuli/platform/paths.hpp"

namespace fs = std::filesystem;

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kuli::platform {

namespace {

std::string expand_tilde(const std::string& s) {
    if (s.empty() || s[0] != '~') return s;
    std::string rest = (s.size() > 1 && (s[1] == '/' || s[1] == '\\')) ? s.substr(2) : s.substr(1);
    fs::path p = paths::home();
    if (!rest.empty()) p /= fs::path(rest);
    return p.string();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool path_contains(const std::string& path_value, const std::string& entry) {
    std::string target = lower(entry);
    std::string seg;
    for (char c : path_value) {
        if (c == ';') {
            if (lower(seg) == target) return true;
            seg.clear();
        } else {
            seg.push_back(c);
        }
    }
    return lower(seg) == target;
}

#if defined(_WIN32)
std::wstring to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string read_user_path() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0, size = 0;
    std::string out;
    if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size) {
        std::wstring buf(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()),
                             &size) == ERROR_SUCCESS) {
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            out = to_utf8(buf);
        }
    }
    RegCloseKey(key);
    return out;
}

bool write_user_path(const std::string& value) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring w = to_w(value);
    LONG rc = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                             reinterpret_cast<const BYTE*>(w.c_str()),
                             static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    // Broadcast so already-running shells can pick it up.
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000, &result);
    return true;
}
#endif  // _WIN32

}  // namespace

bool hkcu_path_prepend(const std::string& entry) {
    // Escape hatch for users who manage PATH themselves (and for tests): when
    // set, kuli never touches the user environment (R-NF-01 — least surprise).
    if (auto skip = get_env("KULI_SKIP_PATH"); skip && !skip->empty()) return false;
    std::string expanded = expand_tilde(entry);
#if defined(_WIN32)
    std::string current = read_user_path();
    if (path_contains(current, expanded)) return false;
    std::string updated = current.empty() ? expanded : (expanded + ";" + current);
    return write_user_path(updated);
#else
    (void)expanded;
    return false;  // shell-rc integration is a later milestone
#endif
}

}  // namespace kuli::platform
