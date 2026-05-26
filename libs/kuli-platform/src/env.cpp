#include "kuli/platform/env.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace kuli::platform {

std::optional<std::string> get_env(std::string_view name) {
#if defined(_WIN32)
    std::wstring wname(name.begin(), name.end());  // ASCII names
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    if (got == 0) return std::nullopt;
    buf.resize(got);
    if (buf.empty()) return std::string{};
    int len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
#else
    std::string n(name);
    const char* v = std::getenv(n.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

}  // namespace kuli::platform
