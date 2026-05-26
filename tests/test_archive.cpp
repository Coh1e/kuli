#include <doctest/doctest.h>

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "kuli/store/archive.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() /
                 ("kuli-archive-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void write_bytes(const fs::path& p, const std::string& b) {
    std::ofstream(p, std::ios::binary).write(b.data(), static_cast<std::streamsize>(b.size()));
}

// --- minimal in-test ustar tar writer + compressors (hermetic fixtures) ---
std::string tar_header(const std::string& name, std::size_t size, char type) {
    char h[512];
    std::memset(h, 0, sizeof(h));
    std::memcpy(h, name.c_str(), std::min<std::size_t>(name.size(), 100));
    std::snprintf(h + 100, 8, "%07o", 0644u);
    std::snprintf(h + 124, 12, "%011o", static_cast<unsigned>(size));
    std::snprintf(h + 136, 12, "%011o", 0u);
    std::memset(h + 148, ' ', 8);  // checksum field = spaces during calc
    h[156] = type;
    std::memcpy(h + 257, "ustar", 5);
    h[263] = '0';
    h[264] = '0';
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += static_cast<unsigned char>(h[i]);
    std::snprintf(h + 148, 7, "%06o", sum);
    h[155] = ' ';
    return std::string(h, 512);
}
struct TEnt {
    std::string name;
    std::string content;
    bool is_dir = false;
    char type = '0';
};
std::string make_tar(const std::vector<TEnt>& es) {
    std::string t;
    for (const auto& e : es) {
        std::string nm = e.name;
        char type = e.is_dir ? '5' : e.type;
        if (e.is_dir && (nm.empty() || nm.back() != '/')) nm += '/';
        bool has_data = !e.is_dir && type != '2';
        t += tar_header(nm, has_data ? e.content.size() : 0, type);
        if (has_data && !e.content.empty()) {
            t += e.content;
            t.append((512 - e.content.size() % 512) % 512, '\0');
        }
    }
    t.append(1024, '\0');  // two zero blocks = end
    return t;
}
std::string gzip_compress(const std::string& in) {
    z_stream s{};
    deflateInit2(&s, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    std::string out;
    std::vector<char> buf(1 << 16);
    int rc;
    do {
        s.next_out = reinterpret_cast<Bytef*>(buf.data());
        s.avail_out = static_cast<uInt>(buf.size());
        rc = deflate(&s, Z_FINISH);
        out.append(buf.data(), buf.size() - s.avail_out);
    } while (rc != Z_STREAM_END);
    deflateEnd(&s);
    return out;
}
std::string zstd_compress(const std::string& in) {
    std::size_t cap = ZSTD_compressBound(in.size());
    std::string out(cap, '\0');
    std::size_t n = ZSTD_compress(out.data(), cap, in.data(), in.size(), 3);
    out.resize(n);
    return out;
}
}  // namespace

TEST_CASE("archive zip round-trip with single-top-dir flatten") {
    fs::path dir = scratch();
    fs::path zip = dir / "a.zip";
    REQUIRE(archive::create_zip(zip, {
        {"tool-1.0/bin/demo.txt", "hello"},
        {"tool-1.0/README.md", "readme"},
    }).has_value());

    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    REQUIRE(r.has_value());
    // top dir "tool-1.0/" stripped.
    CHECK(fs::exists(out / "bin" / "demo.txt"));
    CHECK(read(out / "bin" / "demo.txt") == "hello");
    CHECK(fs::exists(out / "README.md"));
    CHECK_FALSE(fs::exists(out / "tool-1.0"));

    fs::remove_all(dir);
}

TEST_CASE("archive no flatten when multiple top-level entries") {
    fs::path dir = scratch();
    fs::path zip = dir / "b.zip";
    REQUIRE(archive::create_zip(zip, {
        {"a/x.txt", "1"},
        {"b/y.txt", "2"},
    }).has_value());
    fs::path out = dir / "out";
    REQUIRE(archive::extract(zip, out).has_value());
    CHECK(fs::exists(out / "a" / "x.txt"));
    CHECK(fs::exists(out / "b" / "y.txt"));
    fs::remove_all(dir);
}

TEST_CASE("archive selective extraction by path prefix") {
    fs::path dir = scratch();
    fs::path zip = dir / "c.zip";
    REQUIRE(archive::create_zip(zip, {
        {"pkg/scripts/a.lua", "a"},
        {"pkg/fonts/f.ttf", "f"},
        {"pkg/docs/d.md", "d"},
    }).has_value());
    fs::path out = dir / "out";
    archive::ExtractOptions opts;
    opts.paths = {"scripts/", "fonts/"};  // post-flatten prefixes
    REQUIRE(archive::extract(zip, out, opts).has_value());
    CHECK(fs::exists(out / "scripts" / "a.lua"));
    CHECK(fs::exists(out / "fonts" / "f.ttf"));
    CHECK_FALSE(fs::exists(out / "docs" / "d.md"));
    fs::remove_all(dir);
}

TEST_CASE("archive rejects path traversal") {
    fs::path dir = scratch();
    fs::path zip = dir / "evil.zip";
    REQUIRE(archive::create_zip(zip, {
        {"safe/ok.txt", "ok"},
        {"safe/../../escape.txt", "bad"},
    }).has_value());
    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    CHECK_FALSE(r.has_value());  // traversal entry aborts the extract
    fs::remove_all(dir);
}

TEST_CASE("archive accepts a filename containing '..' (L-1, no false reject)") {
    fs::path dir = scratch();
    fs::path zip = dir / "d.zip";
    REQUIRE(archive::create_zip(zip, {
        {"pkg/v1..2/notes.txt", "ok"},  // '..' inside segments, not a traversal
    }).has_value());
    fs::path out = dir / "out";
    auto r = archive::extract(zip, out);
    REQUIRE(r.has_value());
    CHECK(fs::exists(out / "v1..2" / "notes.txt"));  // "pkg" top dir flattened
    fs::remove_all(dir);
}

TEST_CASE("archive .tar round-trip with single-top-dir flatten") {
    fs::path dir = scratch();
    write_bytes(dir / "a.tar", make_tar({{"tool-1/bin/x.txt", "hi"}, {"tool-1/README", "r"}}));
    auto r = archive::extract(dir / "a.tar", dir / "out");
    REQUIRE(r.has_value());
    CHECK(read(dir / "out" / "bin" / "x.txt") == "hi");  // "tool-1/" stripped
    CHECK(fs::exists(dir / "out" / "README"));
    fs::remove_all(dir);
}

TEST_CASE("archive .tar.gz extracts (gzip + tar)") {
    fs::path dir = scratch();
    write_bytes(dir / "a.tar.gz", gzip_compress(make_tar({{"pkg/data.txt", "gzipped"}})));
    auto r = archive::extract(dir / "a.tar.gz", dir / "out");
    REQUIRE(r.has_value());
    CHECK(read(dir / "out" / "data.txt") == "gzipped");
    fs::remove_all(dir);
}

TEST_CASE("archive .tar.zst extracts (zstd + tar)") {
    fs::path dir = scratch();
    write_bytes(dir / "a.tar.zst", zstd_compress(make_tar({{"pkg/data.txt", "zstandard"}})));
    auto r = archive::extract(dir / "a.tar.zst", dir / "out");
    REQUIRE(r.has_value());
    CHECK(read(dir / "out" / "data.txt") == "zstandard");
    fs::remove_all(dir);
}

TEST_CASE("archive tar rejects a symlink entry") {
    fs::path dir = scratch();
    write_bytes(dir / "a.tar",
                make_tar({{"pkg/ok.txt", "x"}, {"pkg/evil", "", false, /*symlink*/ '2'}}));
    auto r = archive::extract(dir / "a.tar", dir / "out");
    CHECK_FALSE(r.has_value());
    fs::remove_all(dir);
}

TEST_CASE("archive unsupported format reports a diagnostic") {
    fs::path dir = scratch();
    fs::path bz = dir / "x.tar.bz2";
    { std::ofstream(bz) << "junk"; }
    auto r = archive::extract(bz, dir / "out");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("unsupported") != std::string::npos);
    fs::remove_all(dir);
}
