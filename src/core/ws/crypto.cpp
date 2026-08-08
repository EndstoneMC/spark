#include "core/ws/crypto.h"

#include <cstring>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace spark {

namespace {

// Helper: write an EVP_PKEY to PKCS#8 DER (unencrypted).
std::vector<std::uint8_t> pkeyToPkcs8(EVP_PKEY *key)
{
    std::vector<std::uint8_t> out;
    auto *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return out;
    }
    if (i2d_PKCS8PrivateKey_bio(bio, key, nullptr, nullptr, 0, nullptr, nullptr)) {
        char *buf = nullptr;
        long len = BIO_get_mem_data(bio, &buf);
        if (len > 0) {
            out.assign(reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len);
        }
    }
    BIO_free(bio);
    return out;
}

// Helper: write a public key to X.509 SubjectPublicKeyInfo DER.
std::vector<std::uint8_t> pkeyToX509(EVP_PKEY *key)
{
    std::vector<std::uint8_t> out;
    auto *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return out;
    }
    if (i2d_PUBKEY_bio(bio, key)) {
        char *buf = nullptr;
        long len = BIO_get_mem_data(bio, &buf);
        if (len > 0) {
            out.assign(reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len);
        }
    }
    BIO_free(bio);
    return out;
}

// Helper: load an EVP_PKEY from X.509 SubjectPublicKeyInfo DER.
EVP_PKEY *x509ToPkey(const std::uint8_t *data, std::size_t len)
{
    return d2i_PUBKEY(nullptr, &data, static_cast<long>(len));
}

// Helper: load an EVP_PKEY from PKCS#8 DER.
EVP_PKEY *pkcs8ToPkey(const std::uint8_t *data, std::size_t len)
{
    const unsigned char *p = data;
    return d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(len));
}

}  // namespace

Crypto::KeyPair Crypto::generateKeyPair()
{
    KeyPair kp;
    auto *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) {
        return kp;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return kp;
    }
    EVP_PKEY *key = nullptr;
    if (EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return kp;
    }
    EVP_PKEY_CTX_free(ctx);
    kp.public_key_x509 = pkeyToX509(key);
    kp.private_key_pkcs8 = pkeyToPkcs8(key);
    EVP_PKEY_free(key);
    return kp;
}

std::vector<std::uint8_t> Crypto::sign(const std::vector<std::uint8_t> &private_key_pkcs8, const std::uint8_t *message,
                                       std::size_t length)
{
    std::vector<std::uint8_t> signature;
    EVP_PKEY *key = pkcs8ToPkey(private_key_pkcs8.data(), private_key_pkcs8.size());
    if (!key) {
        return signature;
    }
    auto *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return signature;
    }
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) > 0) {
        std::size_t sig_len = 0;
        if (EVP_DigestSign(ctx, nullptr, &sig_len, message, length) > 0 && sig_len > 0) {
            signature.resize(sig_len);
            if (EVP_DigestSign(ctx, signature.data(), &sig_len, message, length) > 0) {
                signature.resize(sig_len);
            }
            else {
                signature.clear();
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return signature;
}

bool Crypto::verify(const std::vector<std::uint8_t> &public_key_x509, const std::uint8_t *message, std::size_t length,
                    const std::uint8_t *signature, std::size_t sig_length)
{
    EVP_PKEY *key = x509ToPkey(public_key_x509.data(), public_key_x509.size());
    if (!key) {
        return false;
    }
    auto *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        return false;
    }
    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) > 0) {
        ok = EVP_DigestVerify(ctx, signature, sig_length, message, length) > 0;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

std::vector<std::uint8_t> Crypto::decodePublicKey(const std::vector<std::uint8_t> &x509_bytes)
{
    // In this implementation, the X.509 DER blob IS the public key representation.
    // We return it as-is so callers can compare and store it.
    EVP_PKEY *key = x509ToPkey(x509_bytes.data(), x509_bytes.size());
    if (!key) {
        return {};
    }
    EVP_PKEY_free(key);
    return x509_bytes;
}

}  // namespace spark
