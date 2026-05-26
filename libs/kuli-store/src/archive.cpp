#include "kuli/store/archive.hpp"

#include <miniz.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string first_segment(const std::string& name) {
    auto slash = name.find('/');
    return slash == std::string::npos ? std::string{} : name.substr(0, slash);
}

// Reject ".." traversal segments, absolute paths, and drive-qualified paths.
bool is_unsafe(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel.front() == '/' || rel.front() == '\\') return true;
    if (rel.size() >= 2 && rel[1] == ':') return true;
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

// The single directory shared by every entry (for flatten), or "" if none.
std::string find_common_top(std::vector<std::string> names) {
    std::string top;
    bool have = false;
    for (auto& n : names) {
        std::replace(n.begin(), n.end(), '\\', '/');
        if (n.empty()) continue;
        std::string seg = first_segment(n);
        if (seg.empty()) return "";  // a top-level entry -> nothing to strip
        if (!have) { top = seg; have = true; }
        else if (top != seg) return "";
    }
    return have ? top : "";
}

// Place one archive entry into `dest`: flatten the common top dir, reject
// traversal/symlink/absolute, canonical-containment check, honor partial-extract
// path filters, then create the dir or write the file. Shared by zip + tar.
std::expected<void, Diagnostic> place_entry(const fs::path& dest, std::string name, bool is_dir,
                                            bool is_symlink, const std::string& content,
                                            const std::string& common_top,
                                            const ExtractOptions& opts) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::string rel = name;
    if (!common_top.empty()) {
        std::string prefix = common_top + "/";
        if (rel.rfind(prefix, 0) == 0) rel = rel.substr(prefix.size());
        else if (rel == common_top || rel == common_top + "/") return {};  // the dir entry itself
    }
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    if (rel.empty()) return {};
    if (is_unsafe(rel)) {
        return std::unexpected(err("unsafe path in archive: " + name, "E0214")
                                   .with_help("archive entries may not escape the destination"));
    }
    if (is_symlink) {
        return std::unexpected(err("symlink entry not allowed in archive: " + name, "E0222"));
    }
    if (!starts_with_any(rel, opts.paths)) return {};

    fs::path target = dest / fs::path(rel);
    fs::path nd = dest.lexically_normal();
    fs::path nt = target.lexically_normal();
    fs::path rc = nt.lexically_relative(nd);
    if (rc.empty() || *rc.begin() == "..") {
        return std::unexpected(err("archive entry escapes destination: " + name, "E0214"));
    }

    std::error_code ec;
    if (is_dir) {
        fs::create_directories(target, ec);
        return {};
    }
    if (!write_whole_file(target, content.data(), content.size())) {
        return std::unexpected(err("failed to write " + target.string(), "E0216"));
    }
    return {};
}

// ---- decompressors --------------------------------------------------------
// gzip = 10-byte header (+ optional FEXTRA/FNAME/FCOMMENT/FHCRC) + a raw DEFLATE
// stream + 8-byte trailer. We parse the header and raw-inflate the stream with
// miniz's tinfl (no zlib dependency; the store already sha256-verifies the whole
// archive, so the gzip CRC32 is redundant).
std::expected<std::string, Diagnostic> gunzip(const std::string& in) {
    auto u8 = [&](std::size_t i) { return static_cast<unsigned char>(in[i]); };
    if (in.size() < 18 || u8(0) != 0x1f || u8(1) != 0x8b || u8(2) != 0x08) {
        return std::unexpected(err("not a gzip stream", "E0240"));
    }
    unsigned char flg = u8(3);
    std::size_t p = 10;
    if (flg & 0x04) {  // FEXTRA
        if (p + 2 > in.size()) return std::unexpected(err("truncated gzip header", "E0241"));
        std::size_t xlen = u8(p) | (static_cast<std::size_t>(u8(p + 1)) << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < in.size() && in[p] != 0) ++p; ++p; }  // FNAME
    if (flg & 0x10) { while (p < in.size() && in[p] != 0) ++p; ++p; }  // FCOMMENT
    if (flg & 0x02) { p += 2; }                                        // FHCRC
    if (p + 8 > in.size()) return std::unexpected(err("truncated gzip stream", "E0241"));

    std::size_t deflate_len = in.size() - p - 8;
    std::size_t out_len = 0;
    void* o = tinfl_decompress_mem_to_heap(in.data() + p, deflate_len, &out_len, 0);
    if (!o) return std::unexpected(err("gzip decompression failed", "E0241"));
    std::string out(static_cast<const char*>(o), out_len);
    mz_free(o);
    return out;
}

std::expected<std::string, Diagnostic> zstd_decompress(const std::string& in) {
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return std::unexpected(err("zstd init failed", "E0242"));
    ZSTD_initDStream(ds);
    std::string out;
    std::vector<char> buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob{buf.data(), buf.size(), 0};
        std::size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(rc)) {
            ZSTD_freeDStream(ds);
            return std::unexpected(err("zstd decompression failed", "E0243"));
        }
        out.append(buf.data(), ob.pos);
        if (rc == 0) break;  // frame complete
    }
    ZSTD_freeDStream(ds);
    return out;
}

// ---- tar (USTAR + GNU longname + PAX) -------------------------------------
std::string tar_field(const char* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(p, n);
}

long long parse_octal(const char* p, std::size_t max) {
    long long v = 0;
    std::size_t i = 0;
    while (i < max && p[i] == ' ') ++i;
    while (i < max && p[i] >= '0' && p[i] <= '7') { v = v * 8 + (p[i] - '0'); ++i; }
    return v;
}

// Parse PAX "len key=value\n" records, capturing path/size overrides.
void parse_pax(const std::string& rec, std::string& path, long long& size) {
    std::size_t i = 0;
    while (i < rec.size()) {
        std::size_t sp = rec.find(' ', i);
        if (sp == std::string::npos) break;
        int len = std::atoi(rec.substr(i, sp - i).c_str());
        if (len <= 0 || i + static_cast<std::size_t>(len) > rec.size()) break;
        std::string kv = rec.substr(sp + 1, i + len - (sp + 1));
        while (!kv.empty() && (kv.back() == '\n' || kv.back() == '\0')) kv.pop_back();
        auto eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "path") path = v;
            else if (k == "size") size = std::atoll(v.c_str());
        }
        i += static_cast<std::size_t>(len);
    }
}

struct TarEntry {
    std::string name;
    bool is_dir = false;
    bool is_symlink = false;
    std::string content;
};

std::expected<void, Diagnostic> extract_tar(const std::string& data, const fs::path& dest,
                                            const ExtractOptions& opts) {
    std::vector<TarEntry> entries;
    std::size_t pos = 0;
    std::string gnu_longname;
    std::string pax_path;
    long long pax_size = -1;

    while (pos + 512 <= data.size()) {
        const char* h = data.data() + pos;
        if (std::all_of(h, h + 512, [](char c) { return c == 0; })) break;  // end marker

        std::string name = tar_field(h, 100);
        if (std::strncmp(h + 257, "ustar", 5) == 0) {
            std::string prefix = tar_field(h + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        char type = h[156];
        long long size = parse_octal(h + 124, 12);
        std::size_t data_start = pos + 512;
        std::size_t blocks = static_cast<std::size_t>((size + 511) / 512);
        std::size_t next = data_start + blocks * 512;
        if (size < 0 || data_start + static_cast<std::size_t>(size) > data.size()) {
            return std::unexpected(err("corrupt tar entry", "E0244"));
        }

        if (type == 'L') {  // GNU long name -> name of the next entry
            gnu_longname = std::string(data.data() + data_start, static_cast<std::size_t>(size));
            while (!gnu_longname.empty() && gnu_longname.back() == '\0') gnu_longname.pop_back();
            pos = next;
            continue;
        }
        if (type == 'x' || type == 'g') {  // PAX extended header
            parse_pax(std::string(data.data() + data_start, static_cast<std::size_t>(size)),
                      pax_path, pax_size);
            pos = next;
            continue;
        }

        std::string ename = name;
        if (!gnu_longname.empty()) { ename = gnu_longname; gnu_longname.clear(); }
        if (!pax_path.empty()) { ename = pax_path; pax_path.clear(); }
        long long esize = (pax_size >= 0) ? pax_size : size;
        pax_size = -1;

        TarEntry e;
        e.name = ename;
        e.is_dir = (type == '5') || (!ename.empty() && ename.back() == '/');
        e.is_symlink = (type == '1' || type == '2');
        if (!e.is_dir && !e.is_symlink && esize > 0) {
            e.content.assign(data.data() + data_start, static_cast<std::size_t>(esize));
        }
        entries.push_back(std::move(e));
        pos = next;
    }

    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& e : entries) names.push_back(e.name);
    std::string top = opts.flatten_top_dir ? find_common_top(names) : "";

    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const auto& e : entries) {
        auto r = place_entry(dest, e.name, e.is_dir, e.is_symlink, e.content, top, opts);
        if (!r) return r;
    }
    return {};
}

// ---- zip (miniz) ----------------------------------------------------------
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

    std::vector<std::string> names;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st)) names.push_back(st.m_filename);
    }
    std::string top = opts.flatten_top_dir ? find_common_top(names) : "";

    std::error_code ec;
    fs::create_directories(dest, ec);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(err("corrupt zip entry in " + archive.string(), "E0213"));
        }
        bool is_dir = mz_zip_reader_is_file_a_directory(&zip, i);
        bool is_symlink = ((st.m_external_attr >> 16) & 0xF000u) == 0xA000u;
        std::string content;
        if (!is_dir && !is_symlink) {
            std::size_t out_size = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
            if (!p) {
                mz_zip_reader_end(&zip);
                return std::unexpected(err(std::string("failed to extract ") + st.m_filename, "E0215"));
            }
            content.assign(static_cast<const char*>(p), out_size);
            mz_free(p);
        }
        auto r = place_entry(dest, st.m_filename, is_dir, is_symlink, content, top, opts);
        if (!r) {
            mz_zip_reader_end(&zip);
            return r;
        }
    }
    mz_zip_reader_end(&zip);
    return {};
}

bool ends_with(const std::string& s, const char* suffix) {
    std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::expected<void, Diagnostic> extract_compressed_tar(const fs::path& archive, const fs::path& dest,
                                                       const ExtractOptions& opts, bool zstd) {
    std::string raw;
    if (!read_whole_file(archive, raw)) {
        return std::unexpected(err("cannot read archive: " + archive.string(), "E0211"));
    }
    auto decompressed = zstd ? zstd_decompress(raw) : gunzip(raw);
    if (!decompressed) return std::unexpected(decompressed.error());
    return extract_tar(*decompressed, dest, opts);
}

}  // namespace

std::expected<void, Diagnostic> extract(const fs::path& archive, const fs::path& dest,
                                        const ExtractOptions& opts) {
    std::string name = lower(archive.filename().string());
    if (ends_with(name, ".zip")) return extract_zip(archive, dest, opts);
    if (ends_with(name, ".tar.gz") || ends_with(name, ".tgz")) {
        return extract_compressed_tar(archive, dest, opts, /*zstd=*/false);
    }
    if (ends_with(name, ".tar.zst") || ends_with(name, ".tzst")) {
        return extract_compressed_tar(archive, dest, opts, /*zstd=*/true);
    }
    if (ends_with(name, ".tar")) {
        std::string raw;
        if (!read_whole_file(archive, raw)) {
            return std::unexpected(err("cannot read archive: " + archive.string(), "E0211"));
        }
        return extract_tar(raw, dest, opts);
    }
    return std::unexpected(
        err("unsupported archive format: " + archive.filename().string(), "E0217")
            .with_help("supported: .zip, .tar, .tar.gz/.tgz, .tar.zst/.tzst"));
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
