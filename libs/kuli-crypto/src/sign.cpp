#include "kuli/crypto/sign.hpp"

#include <memory>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>

namespace kuli::crypto {

namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

std::string bio_str(BIO* bio) {
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return std::string(data, data + (len > 0 ? len : 0));
}

std::string to_hex(const unsigned char* p, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(k[p[i] >> 4]);
        out.push_back(k[p[i] & 0xF]);
    }
    return out;
}

std::optional<std::vector<unsigned char>> from_hex(const std::string& s) {
    if (s.size() % 2 != 0) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

PkeyPtr load_private(const std::string& pem) {
    BioPtr b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!b) return {nullptr, EVP_PKEY_free};
    return PkeyPtr(PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
}

PkeyPtr load_public(const std::string& pem) {
    BioPtr b(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!b) return {nullptr, EVP_PKEY_free};
    return PkeyPtr(PEM_read_bio_PUBKEY(b.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
}

}  // namespace

std::optional<KeyPair> ed25519_generate() {
    CtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0) return std::nullopt;
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) return std::nullopt;
    PkeyPtr pkey(raw, EVP_PKEY_free);

    BioPtr priv(BIO_new(BIO_s_mem()), BIO_free);
    BioPtr pub(BIO_new(BIO_s_mem()), BIO_free);
    if (!priv || !pub) return std::nullopt;
    if (!PEM_write_bio_PrivateKey(priv.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr)) {
        return std::nullopt;
    }
    if (!PEM_write_bio_PUBKEY(pub.get(), pkey.get())) return std::nullopt;
    return KeyPair{bio_str(priv.get()), bio_str(pub.get())};
}

std::optional<std::string> ed25519_public_of(const std::string& private_pem) {
    PkeyPtr pkey = load_private(private_pem);
    if (!pkey) return std::nullopt;
    BioPtr pub(BIO_new(BIO_s_mem()), BIO_free);
    if (!pub || !PEM_write_bio_PUBKEY(pub.get(), pkey.get())) return std::nullopt;
    return bio_str(pub.get());
}

std::optional<std::string> ed25519_sign(const std::string& private_pem, std::string_view msg) {
    PkeyPtr pkey = load_private(private_pem);
    if (!pkey) return std::nullopt;
    MdCtxPtr md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md) return std::nullopt;
    if (EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, pkey.get()) <= 0) return std::nullopt;

    const auto* m = reinterpret_cast<const unsigned char*>(msg.data());
    std::size_t siglen = 0;
    if (EVP_DigestSign(md.get(), nullptr, &siglen, m, msg.size()) <= 0) return std::nullopt;
    std::vector<unsigned char> sig(siglen);
    if (EVP_DigestSign(md.get(), sig.data(), &siglen, m, msg.size()) <= 0) return std::nullopt;
    return to_hex(sig.data(), siglen);
}

bool ed25519_verify(const std::string& public_pem, std::string_view msg, const std::string& sig_hex) {
    auto sig = from_hex(sig_hex);
    if (!sig) return false;
    PkeyPtr pkey = load_public(public_pem);
    if (!pkey) return false;
    MdCtxPtr md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md) return false;
    if (EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, pkey.get()) <= 0) return false;
    const auto* m = reinterpret_cast<const unsigned char*>(msg.data());
    return EVP_DigestVerify(md.get(), sig->data(), sig->size(), m, msg.size()) == 1;
}

}  // namespace kuli::crypto
