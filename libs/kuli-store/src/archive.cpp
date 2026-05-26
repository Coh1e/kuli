#include "kuli/store/archive.hpp"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace kuli::archive {

namespace {

using kuli::diag::Diagnostic;
using kuli::diag::Kind;

Diagnostic err(std::string msg, std::string code = "E0210") {
    return Diagnostic::of(Kind::General, std::move(msg), std::move(code));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool read_whole_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_whole_file(const fs::path& p, const char* data, std::size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data, static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

// First path segment of an archive entry ("a/b/c" -> "a"); empty if none.
std::string first_segment(const std::string& name) {
    auto slash = name.find('/');
    return slash == std::string::npos ? std::string{} : name.substr(0, slash);
}

// Reject ".." traversal, absolute, or drive-qualified entry paths.
bool is_unsafe(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel.front() == '/' || rel.front() == '\\') return true;
    if (rel.size() >= 2 && rel[1] == ':') return true;  // C:\ ...
    std::string seg;
    std::istringstream ss(rel);
    while (std::getline(ss, seg, '/')) {
        if (seg == "..") return true;
    }
    return false;
}

bool starts_with_any(const std::string& s, const std::vector<std::string>& prefixes) {
    if (prefixes.empty()) return true;
    for (const auto& p : prefixes) {
        if (s.rfind(p, 0) == 0) return true;
    }
    return false;
}

std::expected<void, Diagnostic> extract_zip(const fs::path& archive, const fs::path& dest,
                                            const ExtractOptions& opts) {
    std::string buf;
    if (!read_whole_file(archive, buf)) {
        return std::unexpected(err("cannot read archive: " + archive.string(), "E0211"));
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, buf.data(), buf.size(), 0)) {
        return std::unexpected(err("not a valid zip archive: " + archive.string(), "E0212"));
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);

    // Detect a single shared top-level directory to flatten.
    std::string common_top;
    bool have_top = true;
    for (mz_uint i = 0; i < count && have_top; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename;
        std::string seg = first_segment(name);
        if (seg.empty()) {
            have_top = false;  // a top-level file means no single dir to strip
        } else if (common_top.empty()) {
            common_top = seg;
        } else if (common_top != seg) {
            have_top = false;
        }
    }
    bool flatten = opts.flatten_top_dir && have_top && !common_top.empty();

    std::error_code ec;
    fs::create_directories(dest, ec);

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(err("corrupt zip entry in " + archive.string(), "E0213"));
        }
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');

        std::string rel = name;
        if (flatten) {
            std::string prefix = common_top + "/";
            if (rel.rfind(prefix, 0) == 0) rel = rel.substr(prefix.size());
            else if (rel == common_top) continue;  // the dir entry itself
        }
        if (rel.empty()) continue;
        if (is_unsafe(rel)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(
                err("unsafe path in archive: " + name, "E0214")
                    .with_help("archive entries may not escape the destination"));
        }
        // Reject symlink entries: miniz reports Unix mode in the high 16 bits of
        // m_external_attr; S_IFLNK == 0xA000. We never follow/recreate links, so
        // an extracted symlink could later redirect a write outside the store.
        if (((st.m_external_attr >> 16) & 0xF000u) == 0xA000u) {
            mz_zip_reader_end(&zip);
            return std::unexpected(err("symlink entry not allowed in archive: " + name, "E0222"));
        }
        if (!starts_with_any(rel, opts.paths)) continue;

        fs::path target = dest / fs::path(rel);
        // Canonical containment proof: the normalized target must stay under
        // dest (belt-and-braces over the lexical is_unsafe check).
        {
            fs::path nd = dest.lexically_normal();
            fs::path nt = target.lexically_normal();
            fs::path r = nt.lexically_relative(nd);
            if (r.empty() || *r.begin() == "..") {
                mz_zip_reader_end(&zip);
                return std::unexpected(
                    err("archive entry escapes destination: " + name, "E0214"));
            }
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(target, ec);
            continue;
        }

        std::size_t out_size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
        if (!p) {
            mz_zip_reader_end(&zip);
            return std::unexpected(err("failed to extract " + name, "E0215"));
        }
        bool ok = write_whole_file(target, static_cast<const char*>(p), out_size);
        mz_free(p);
        if (!ok) {
            mz_zip_reader_end(&zip);
            return std::unexpected(err("failed to write " + target.string(), "E0216"));
        }
    }

    mz_zip_reader_end(&zip);
    return {};
}

}  // namespace

std::expected<void, Diagnostic> extract(const fs::path& archive, const fs::path& dest,
                                        const ExtractOptions& opts) {
    std::string name = lower(archive.filename().string());
    auto ends = [&](const char* suffix) {
        std::string s(suffix);
        return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0;
    };
    if (ends(".zip")) return extract_zip(archive, dest, opts);
    return std::unexpected(
        err("unsupported archive format: " + archive.filename().string(), "E0217")
            .with_help("v0.x extracts .zip; .tar.zst / .tar.gz are not yet implemented"));
}

std::expected<void, Diagnostic> create_zip(
    const fs::path& out,
    const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return std::unexpected(err("zip writer init failed", "E0218"));
    }
    for (const auto& [path, data] : entries) {
        if (!mz_zip_writer_add_mem(&zip, path.c_str(), data.data(), data.size(),
                                   MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return std::unexpected(err("zip add failed: " + path, "E0219"));
        }
    }
    void* p = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &p, &size)) {
        mz_zip_writer_end(&zip);
        return std::unexpected(err("zip finalize failed", "E0220"));
    }
    bool ok = write_whole_file(out, static_cast<const char*>(p), size);
    mz_free(p);
    mz_zip_writer_end(&zip);
    if (!ok) return std::unexpected(err("cannot write zip: " + out.string(), "E0221"));
    return {};
}

}  // namespace kuli::archive
