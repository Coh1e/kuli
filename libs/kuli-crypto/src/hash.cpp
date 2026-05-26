#include "kuli/crypto/hash.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <vector>

namespace kuli::crypto {

namespace {

constexpr std::size_t kChunk = 1 << 16;  // 64 KiB

const EVP_MD* md_for(Algorithm a) {
    switch (a) {
        case Algorithm::Sha256: return EVP_sha256();
        case Algorithm::Sha512: return EVP_sha512();
        case Algorithm::Md5:    return EVP_md5();
        case Algorithm::Sha1:   return EVP_sha1();
    }
    return EVP_sha256();
}

std::string to_hex(const unsigned char* data, std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(n * 2, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[(data[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

// RAII over EVP_MD_CTX.
struct MdCtx {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    ~MdCtx() { if (ctx) EVP_MD_CTX_free(ctx); }
    explicit operator bool() const { return ctx != nullptr; }
};

std::optional<std::string> digest_bytes(Algorithm algo, const void* data, std::size_t len) {
    MdCtx c;
    if (!c) return std::nullopt;
    if (EVP_DigestInit_ex(c.ctx, md_for(algo), nullptr) != 1) return std::nullopt;
    if (len > 0 && EVP_DigestUpdate(c.ctx, data, len) != 1) return std::nullopt;
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(c.ctx, out.data(), &out_len) != 1) return std::nullopt;
    return to_hex(out.data(), out_len);
}

}  // namespace

std::string algo_name(Algorithm a) {
    switch (a) {
        case Algorithm::Sha256: return "sha256";
        case Algorithm::Sha512: return "sha512";
        case Algorithm::Md5:    return "md5";
        case Algorithm::Sha1:   return "sha1";
    }
    return "sha256";
}

std::optional<Algorithm> parse_algo(std::string_view name) {
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "sha256") return Algorithm::Sha256;
    if (s == "sha512") return Algorithm::Sha512;
    if (s == "md5")    return Algorithm::Md5;
    if (s == "sha1")   return Algorithm::Sha1;
    return std::nullopt;
}

std::optional<HashSpec> parse(std::string_view raw) {
    std::size_t b = 0;
    std::size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    if (b == e) return std::nullopt;
    std::string trimmed(raw.substr(b, e - b));

    auto colon = trimmed.find(':');
    Algorithm algo = Algorithm::Sha256;
    std::string hex;
    if (colon != std::string::npos) {
        auto a = parse_algo(std::string_view(trimmed).substr(0, colon));
        if (!a) return std::nullopt;
        algo = *a;
        hex = trimmed.substr(colon + 1);
    } else {
        hex = trimmed;
    }
    if (hex.empty()) return std::nullopt;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return HashSpec{algo, hex};
}

std::string to_string(const HashSpec& spec) {
    return algo_name(spec.algo) + ":" + spec.hex;
}

std::string sha256_hex(std::span<const std::byte> data) {
    auto r = digest_bytes(Algorithm::Sha256, data.data(), data.size());
    return r.value_or(std::string{});
}
std::string sha256_hex(std::string_view data) {
    auto r = digest_bytes(Algorithm::Sha256, data.data(), data.size());
    return r.value_or(std::string{});
}
std::string md5_hex(std::span<const std::byte> data) {
    auto r = digest_bytes(Algorithm::Md5, data.data(), data.size());
    return r.value_or(std::string{});
}
std::string md5_hex(std::string_view data) {
    auto r = digest_bytes(Algorithm::Md5, data.data(), data.size());
    return r.value_or(std::string{});
}

std::optional<HashSpec> hash_file(const fs::path& path, Algorithm algo) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    MdCtx c;
    if (!c) return std::nullopt;
    if (EVP_DigestInit_ex(c.ctx, md_for(algo), nullptr) != 1) return std::nullopt;

    std::vector<char> buf(kChunk);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0 &&
            EVP_DigestUpdate(c.ctx, buf.data(), static_cast<std::size_t>(n)) != 1) {
            return std::nullopt;
        }
    }
    if (in.bad()) return std::nullopt;

    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(c.ctx, out.data(), &out_len) != 1) return std::nullopt;
    return HashSpec{algo, to_hex(out.data(), out_len)};
}

bool verify_file(const fs::path& path, const HashSpec& expected) {
    auto actual = hash_file(path, expected.algo);
    if (!actual) return false;
    return actual->hex == expected.hex;  // both lowercase
}

}  // namespace kuli::crypto
