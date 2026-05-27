#pragma once
// Ed25519 signatures over OpenSSL EVP — node identity + signed CapabilityRecords
// (§6: "签名 + 单调版本化"). Keys are PEM strings; signatures are lowercase hex.

#include <optional>
#include <string>
#include <string_view>

namespace kuli::crypto {

struct KeyPair {
    std::string private_pem;
    std::string public_pem;
};

// Generate a fresh Ed25519 keypair (PEM-encoded). nullopt on OpenSSL failure.
std::optional<KeyPair> ed25519_generate();

// Derive the public PEM from a private PEM (e.g. after loading a stored key).
std::optional<std::string> ed25519_public_of(const std::string& private_pem);

// Sign `msg` with the private PEM; returns the signature as lowercase hex.
std::optional<std::string> ed25519_sign(const std::string& private_pem, std::string_view msg);

// Verify a lowercase-hex signature of `msg` against the public PEM.
bool ed25519_verify(const std::string& public_pem, std::string_view msg, const std::string& sig_hex);

}  // namespace kuli::crypto
