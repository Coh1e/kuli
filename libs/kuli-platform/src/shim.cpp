#include "kuli/platform/shim.hpp"

#include <fstream>

namespace kuli::platform {

bool write_shim(const fs::path& bin_dir, const std::string& alias, const fs::path& target) {
    std::error_code ec;
    fs::create_directories(bin_dir, ec);
#if defined(_WIN32)
    fs::path shim = bin_dir / (alias + ".cmd");
    std::ofstream o(shim, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << "@echo off\r\n\"" << target.string() << "\" %*\r\n";
    return o.good();
#else
    fs::path shim = bin_dir / alias;
    {
        std::ofstream o(shim, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << "#!/bin/sh\nexec \"" << target.string() << "\" \"$@\"\n";
        if (!o.good()) return false;
    }
    fs::permissions(shim,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return !ec;
#endif
}

bool write_basename_shim(const fs::path& bin_dir, const std::string& basename,
                         const fs::path& kuli_exe) {
    std::error_code ec;
    fs::create_directories(bin_dir, ec);
#if defined(_WIN32)
    fs::path shim = bin_dir / (basename + ".cmd");
    std::ofstream o(shim, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << "@echo off\r\n\"" << kuli_exe.string() << "\" --basename " << basename << " -- %*\r\n";
    return o.good();
#else
    fs::path shim = bin_dir / basename;
    {
        std::ofstream o(shim, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << "#!/bin/sh\nexec \"" << kuli_exe.string() << "\" --basename " << basename
          << " -- \"$@\"\n";
        if (!o.good()) return false;
    }
    fs::permissions(shim,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return !ec;
#endif
}

bool remove_shim(const fs::path& bin_dir, const std::string& alias) {
    std::error_code ec;
#if defined(_WIN32)
    fs::path shim = bin_dir / (alias + ".cmd");
#else
    fs::path shim = bin_dir / alias;
#endif
    return fs::remove(shim, ec);
}

}  // namespace kuli::platform
