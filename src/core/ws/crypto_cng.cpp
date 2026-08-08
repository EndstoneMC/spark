// Windows CNG/BCrypt crypto backend. Wire-protocol key formats match the OpenSSL backend.

#include "core/ws/crypto.h"

#ifndef _WIN32
#error "This file is Windows-only; use crypto.cpp on Linux."
#endif

// clang-format off: bcrypt.h and ncrypt.h require windows.h types
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
// clang-format on

#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace spark {

namespace {

// ---------------------------------------------------------------------------
// Minimal DER encoder / decoder
// ---------------------------------------------------------------------------

namespace der {

// Encode a DER length.
void encodeLength(std::vector<std::uint8_t> &out, std::size_t len)
{
    if (len < 0x80) {
        out.push_back(static_cast<std::uint8_t>(len));
    }
    else if (len <= 0xff) {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(len));
    }
    else if (len <= 0xffff) {
        out.push_back(0x82);
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(len));
    }
    else {
        out.push_back(0x83);
        out.push_back(static_cast<std::uint8_t>(len >> 16));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(len));
    }
}

// Append tag + length + content.
void encodeTLV(std::vector<std::uint8_t> &out, std::uint8_t tag, const std::uint8_t *content, std::size_t len)
{
    out.push_back(tag);
    encodeLength(out, len);
    out.insert(out.end(), content, content + len);
}

// Encode a big-endian integer as DER INTEGER (tag 0x02).
// Prepends 0x00 when the high bit is set so the value stays positive.
void encodeInteger(std::vector<std::uint8_t> &out, const std::uint8_t *data, std::size_t len)
{
    // Strip leading zero bytes (DER requires minimal encoding).
    while (len > 1 && data[0] == 0) {
        ++data;
        --len;
    }
    std::vector<std::uint8_t> padded;
    if (len > 0 && (data[0] & 0x80)) {
        padded.push_back(0x00);
    }
    padded.insert(padded.end(), data, data + len);
    encodeTLV(out, 0x02, padded.data(), padded.size());
}

// Encode SEQUENCE (tag 0x30).
void encodeSequence(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &content)
{
    encodeTLV(out, 0x30, content.data(), content.size());
}

// Encode BIT STRING (tag 0x03) with 0 unused bits.
void encodeBitString(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &content)
{
    std::vector<std::uint8_t> padded;
    padded.push_back(0x00);  // 0 unused bits
    padded.insert(padded.end(), content.begin(), content.end());
    encodeTLV(out, 0x03, padded.data(), padded.size());
}

// Encode OCTET STRING (tag 0x04).
void encodeOctetString(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &content)
{
    encodeTLV(out, 0x04, content.data(), content.size());
}

// The RSA algorithm identifier: SEQUENCE { OID 1.2.840.113549.1.1.1, NULL }
// Pre-encoded as constant bytes.
constexpr std::uint8_t kRsaAlgId[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
                                      0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
constexpr std::size_t kRsaAlgIdLen = sizeof(kRsaAlgId);

// Parse a DER tag + length.  On success, *content points to the value
// bytes, *contentLen is their length, and the return value points past
// the end of the element.  Returns nullptr on error.
const std::uint8_t *parseElement(const std::uint8_t *data, std::size_t len, std::uint8_t expectedTag,
                                 const std::uint8_t **content, std::size_t *contentLen)
{
    if (len < 2 || data[0] != expectedTag) {
        return nullptr;
    }
    std::size_t i = 1;
    std::size_t hdrLen;
    if (data[i] < 0x80) {
        *contentLen = data[i];
        hdrLen = 2;
    }
    else {
        std::size_t n = data[i] & 0x7f;
        ++i;
        if (n == 0 || n > 4 || len < i + n) {
            return nullptr;
        }
        *contentLen = 0;
        for (std::size_t j = 0; j < n; ++j) {
            *contentLen = (*contentLen << 8) | data[i + j];
        }
        hdrLen = 1 + 1 + n;
    }
    if (len < hdrLen + *contentLen) {
        return nullptr;
    }
    *content = data + hdrLen;
    return data + hdrLen + *contentLen;
}

// Parse a DER INTEGER, returning the big-endian value bytes.
// Strips a single leading 0x00 sign byte if present.
bool parseInteger(const std::uint8_t *data, std::size_t len, std::vector<std::uint8_t> &value,
                  const std::uint8_t **next)
{
    const std::uint8_t *content;
    std::size_t contentLen;
    const std::uint8_t *p = parseElement(data, len, 0x02, &content, &contentLen);
    if (!p) {
        return false;
    }
    // Strip a single leading 0x00 sign byte.
    if (contentLen > 1 && content[0] == 0x00) {
        ++content;
        --contentLen;
    }
    value.assign(content, content + contentLen);
    if (next) {
        *next = p;
    }
    return true;
}

// Skip any DER element (any tag).
const std::uint8_t *skipElement(const std::uint8_t *data, std::size_t len)
{
    if (len < 2) {
        return nullptr;
    }
    std::size_t i = 1;
    std::size_t hdrLen;
    std::size_t contentLen;
    if (data[i] < 0x80) {
        contentLen = data[i];
        hdrLen = 2;
    }
    else {
        std::size_t n = data[i] & 0x7f;
        ++i;
        if (n == 0 || n > 4 || len < i + n) {
            return nullptr;
        }
        contentLen = 0;
        for (std::size_t j = 0; j < n; ++j) {
            contentLen = (contentLen << 8) | data[i + j];
        }
        hdrLen = 1 + 1 + n;
    }
    if (len < hdrLen + contentLen) {
        return nullptr;
    }
    return data + hdrLen + contentLen;
}

}  // namespace der

// ---------------------------------------------------------------------------
// BCRYPT_RSAKEY_BLOB helpers
// ---------------------------------------------------------------------------

struct RsaKeyParams {
    std::vector<std::uint8_t> modulus;
    std::vector<std::uint8_t> public_exp;
    std::vector<std::uint8_t> private_exp;
    std::vector<std::uint8_t> prime1;
    std::vector<std::uint8_t> prime2;
    std::vector<std::uint8_t> exponent1;
    std::vector<std::uint8_t> exponent2;
    std::vector<std::uint8_t> coefficient;
};

// Build a BCRYPT_RSAKEY_BLOB (public or private) from parsed params.
std::vector<std::uint8_t> buildRsaKeyBlob(bool is_private, const RsaKeyParams &p)
{
    BCRYPT_RSAKEY_BLOB header = {};
    header.Magic = is_private ? BCRYPT_RSAFULLPRIVATE_MAGIC : BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = static_cast<ULONG>(p.modulus.size() * 8);
    header.cbPublicExp = static_cast<ULONG>(p.public_exp.size());
    header.cbModulus = static_cast<ULONG>(p.modulus.size());
    header.cbPrime1 = is_private ? static_cast<ULONG>(p.prime1.size()) : 0;
    header.cbPrime2 = is_private ? static_cast<ULONG>(p.prime2.size()) : 0;

    std::vector<std::uint8_t> blob;
    blob.insert(blob.end(), reinterpret_cast<const std::uint8_t *>(&header),
                reinterpret_cast<const std::uint8_t *>(&header) + sizeof(header));
    blob.insert(blob.end(), p.public_exp.begin(), p.public_exp.end());
    blob.insert(blob.end(), p.modulus.begin(), p.modulus.end());
    if (is_private) {
        blob.insert(blob.end(), p.prime1.begin(), p.prime1.end());
        blob.insert(blob.end(), p.prime2.begin(), p.prime2.end());
        blob.insert(blob.end(), p.exponent1.begin(), p.exponent1.end());
        blob.insert(blob.end(), p.exponent2.begin(), p.exponent2.end());
        blob.insert(blob.end(), p.coefficient.begin(), p.coefficient.end());
        blob.insert(blob.end(), p.private_exp.begin(), p.private_exp.end());
    }
    return blob;
}

// Parse a BCRYPT_RSAKEY_BLOB into params.
bool parseRsaKeyBlob(const std::uint8_t *blob, std::size_t len, RsaKeyParams &params, bool &is_private)
{
    if (len < sizeof(BCRYPT_RSAKEY_BLOB)) {
        return false;
    }
    auto *hdr = reinterpret_cast<const BCRYPT_RSAKEY_BLOB *>(blob);
    is_private = (hdr->Magic == BCRYPT_RSAFULLPRIVATE_MAGIC);
    const std::uint8_t *p = blob + sizeof(BCRYPT_RSAKEY_BLOB);
    std::size_t remaining = len - sizeof(BCRYPT_RSAKEY_BLOB);

    auto extract = [&](std::size_t n, std::vector<std::uint8_t> &out) -> bool {
        if (remaining < n) {
            return false;
        }
        out.assign(p, p + n);
        p += n;
        remaining -= n;
        return true;
    };

    if (!extract(hdr->cbPublicExp, params.public_exp)) {
        return false;
    }
    if (!extract(hdr->cbModulus, params.modulus)) {
        return false;
    }
    if (is_private) {
        if (!extract(hdr->cbPrime1, params.prime1)) {
            return false;
        }
        if (!extract(hdr->cbPrime2, params.prime2)) {
            return false;
        }
        if (!extract(hdr->cbPrime1, params.exponent1)) {
            return false;
        }
        if (!extract(hdr->cbPrime2, params.exponent2)) {
            return false;
        }
        if (!extract(hdr->cbPrime1, params.coefficient)) {
            return false;
        }
        if (!extract(hdr->cbModulus, params.private_exp)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Key format conversion: BCRYPT blob <-> DER
// ---------------------------------------------------------------------------

// Convert BCRYPT_RSAPUBLIC_BLOB to X.509 SubjectPublicKeyInfo DER.
std::vector<std::uint8_t> publicBlobToX509(const std::uint8_t *blob, std::size_t len)
{
    RsaKeyParams params;
    bool is_private;
    if (!parseRsaKeyBlob(blob, len, params, is_private) || is_private) {
        return {};
    }

    // RSAPublicKey = SEQUENCE { INTEGER modulus, INTEGER publicExponent }
    std::vector<std::uint8_t> rsa_pub_key;
    der::encodeInteger(rsa_pub_key, params.modulus.data(), params.modulus.size());
    der::encodeInteger(rsa_pub_key, params.public_exp.data(), params.public_exp.size());
    std::vector<std::uint8_t> rsa_pub_seq;
    der::encodeSequence(rsa_pub_seq, rsa_pub_key);

    // SubjectPublicKeyInfo = SEQUENCE { AlgId, BIT STRING(RSAPublicKey) }
    std::vector<std::uint8_t> inner;
    inner.insert(inner.end(), der::kRsaAlgId, der::kRsaAlgId + der::kRsaAlgIdLen);
    der::encodeBitString(inner, rsa_pub_seq);

    std::vector<std::uint8_t> out;
    der::encodeSequence(out, inner);
    return out;
}

// Convert BCRYPT_RSAPRIVATE_BLOB to PKCS#8 PrivateKeyInfo DER.
std::vector<std::uint8_t> privateBlobToPkcs8(const std::uint8_t *blob, std::size_t len)
{
    RsaKeyParams params;
    bool is_private;
    if (!parseRsaKeyBlob(blob, len, params, is_private) || !is_private) {
        return {};
    }

    // RSAPrivateKey = SEQUENCE { version, n, e, d, p, q, dmp1, dmq1, iqmp }
    std::vector<std::uint8_t> rsa_priv_key;
    static const std::uint8_t kZero = 0;
    der::encodeInteger(rsa_priv_key, &kZero, 1);  // version = 0
    der::encodeInteger(rsa_priv_key, params.modulus.data(), params.modulus.size());
    der::encodeInteger(rsa_priv_key, params.public_exp.data(), params.public_exp.size());
    der::encodeInteger(rsa_priv_key, params.private_exp.data(), params.private_exp.size());
    der::encodeInteger(rsa_priv_key, params.prime1.data(), params.prime1.size());
    der::encodeInteger(rsa_priv_key, params.prime2.data(), params.prime2.size());
    der::encodeInteger(rsa_priv_key, params.exponent1.data(), params.exponent1.size());
    der::encodeInteger(rsa_priv_key, params.exponent2.data(), params.exponent2.size());
    der::encodeInteger(rsa_priv_key, params.coefficient.data(), params.coefficient.size());
    std::vector<std::uint8_t> rsa_priv_seq;
    der::encodeSequence(rsa_priv_seq, rsa_priv_key);

    // PrivateKeyInfo = SEQUENCE { version, AlgId, OCTET STRING(RSAPrivateKey) }
    std::vector<std::uint8_t> inner;
    der::encodeInteger(inner, &kZero, 1);  // version = 0
    inner.insert(inner.end(), der::kRsaAlgId, der::kRsaAlgId + der::kRsaAlgIdLen);
    der::encodeOctetString(inner, rsa_priv_seq);

    std::vector<std::uint8_t> out;
    der::encodeSequence(out, inner);
    return out;
}

// Parse X.509 SubjectPublicKeyInfo DER to BCRYPT_RSAPUBLIC_BLOB.
std::vector<std::uint8_t> x509ToPublicBlob(const std::uint8_t *data, std::size_t len)
{
    // SubjectPublicKeyInfo = SEQUENCE { AlgId, BIT STRING }
    const std::uint8_t *content;
    std::size_t contentLen;
    const std::uint8_t *p = der::parseElement(data, len, 0x30, &content, &contentLen);
    if (!p) {
        return {};
    }

    // Skip AlgId (SEQUENCE).
    const std::uint8_t *alg_content;
    std::size_t alg_len;
    const std::uint8_t *after_alg = der::parseElement(content, contentLen, 0x30, &alg_content, &alg_len);
    if (!after_alg) {
        return {};
    }

    // Parse BIT STRING.
    const std::uint8_t *bit_content;
    std::size_t bit_len;
    const std::uint8_t *after_bit =
        der::parseElement(after_alg, contentLen - (after_alg - content), 0x03, &bit_content, &bit_len);
    if (!after_bit || bit_len < 1) {
        return {};
    }

    // Skip the unused-bits byte.
    ++bit_content;
    --bit_len;

    // Parse RSAPublicKey = SEQUENCE { INTEGER modulus, INTEGER publicExponent }
    const std::uint8_t *rsa_content;
    std::size_t rsa_len;
    const std::uint8_t *after_rsa = der::parseElement(bit_content, bit_len, 0x30, &rsa_content, &rsa_len);
    if (!after_rsa) {
        return {};
    }

    RsaKeyParams params;
    const std::uint8_t *cur = rsa_content;
    std::size_t cur_len = rsa_len;
    if (!der::parseInteger(cur, cur_len, params.modulus, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.public_exp, &cur)) {
        return {};
    }

    return buildRsaKeyBlob(false, params);
}

// Parse PKCS#8 PrivateKeyInfo DER to BCRYPT_RSAPRIVATE_BLOB.
std::vector<std::uint8_t> pkcs8ToPrivateBlob(const std::uint8_t *data, std::size_t len)
{
    // PrivateKeyInfo = SEQUENCE { version, AlgId, OCTET STRING }
    const std::uint8_t *content;
    std::size_t contentLen;
    const std::uint8_t *p = der::parseElement(data, len, 0x30, &content, &contentLen);
    if (!p) {
        return {};
    }

    // Skip version (INTEGER).
    std::vector<std::uint8_t> version;
    const std::uint8_t *after_ver;
    if (!der::parseInteger(content, contentLen, version, &after_ver)) {
        return {};
    }

    // Skip AlgId (SEQUENCE).
    const std::uint8_t *alg_content;
    std::size_t alg_len;
    const std::uint8_t *after_alg =
        der::parseElement(after_ver, contentLen - (after_ver - content), 0x30, &alg_content, &alg_len);
    if (!after_alg) {
        return {};
    }

    // Parse OCTET STRING containing RSAPrivateKey.
    const std::uint8_t *oct_content;
    std::size_t oct_len;
    const std::uint8_t *after_oct =
        der::parseElement(after_alg, contentLen - (after_alg - content), 0x04, &oct_content, &oct_len);
    if (!after_oct) {
        return {};
    }

    // RSAPrivateKey = SEQUENCE { version, n, e, d, p, q, dmp1, dmq1, iqmp }
    const std::uint8_t *rsa_content;
    std::size_t rsa_len;
    const std::uint8_t *after_rsa = der::parseElement(oct_content, oct_len, 0x30, &rsa_content, &rsa_len);
    if (!after_rsa) {
        return {};
    }

    RsaKeyParams params;
    const std::uint8_t *cur = rsa_content;
    std::size_t cur_len = rsa_len;

    // Skip version.
    if (!der::parseInteger(cur, cur_len, version, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.modulus, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.public_exp, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.private_exp, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.prime1, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.prime2, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.exponent1, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.exponent2, &cur)) {
        return {};
    }
    cur_len = rsa_len - (cur - rsa_content);
    if (!der::parseInteger(cur, cur_len, params.coefficient, &cur)) {
        return {};
    }

    return buildRsaKeyBlob(true, params);
}

// ---------------------------------------------------------------------------
// SHA-256 helper
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> sha256(const std::uint8_t *data, std::size_t len)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }

    DWORD hash_len = 0;
    DWORD hash_obj_len = 0;
    ULONG cbData = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(DWORD), &cbData, 0);
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_obj_len), sizeof(DWORD), &cbData, 0);

    std::vector<std::uint8_t> hash_obj(hash_obj_len);
    std::vector<std::uint8_t> hash(hash_len);
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    if (BCryptCreateHash(alg, &hash_handle, hash_obj.data(), hash_obj_len, nullptr, 0, 0) == 0) {
        BCryptHashData(hash_handle, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
        BCryptFinishHash(hash_handle, hash.data(), hash_len, 0);
        BCryptDestroyHash(hash_handle);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash;
}

}  // namespace

// ---------------------------------------------------------------------------
// Crypto interface implementation
// ---------------------------------------------------------------------------

Crypto::KeyPair Crypto::generateKeyPair()
{
    KeyPair kp;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
        return kp;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptGenerateKeyPair(alg, &key, 2048, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return kp;
    }
    if (BCryptFinalizeKeyPair(key, 0) != 0) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
        return kp;
    }

    // Export public key blob.
    DWORD pub_blob_len = 0;
    BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &pub_blob_len, 0);
    std::vector<std::uint8_t> pub_blob(pub_blob_len);
    if (BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, pub_blob.data(), pub_blob_len, &pub_blob_len, 0) == 0) {
        kp.public_key_x509 = publicBlobToX509(pub_blob.data(), pub_blob_len);
    }

    // Export private key blob (includes private exponent).
    DWORD priv_blob_len = 0;
    BCryptExportKey(key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, nullptr, 0, &priv_blob_len, 0);
    std::vector<std::uint8_t> priv_blob(priv_blob_len);
    if (BCryptExportKey(key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, priv_blob.data(), priv_blob_len, &priv_blob_len, 0) ==
        0) {
        kp.private_key_pkcs8 = privateBlobToPkcs8(priv_blob.data(), priv_blob_len);
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return kp;
}

std::vector<std::uint8_t> Crypto::sign(const std::vector<std::uint8_t> &private_key_pkcs8, const std::uint8_t *message,
                                       std::size_t length)
{
    std::vector<std::uint8_t> signature;
    auto blob = pkcs8ToPrivateBlob(private_key_pkcs8.data(), private_key_pkcs8.size());
    if (blob.empty()) {
        return signature;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
        return signature;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, &key, blob.data(),
                            static_cast<ULONG>(blob.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return signature;
    }

    auto hash = sha256(message, length);
    if (hash.empty()) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
        return signature;
    }

    BCRYPT_PKCS1_PADDING_INFO pad_info = {};
    pad_info.pszAlgId = BCRYPT_SHA256_ALGORITHM;

    DWORD sig_len = 0;
    if (BCryptSignHash(key, &pad_info, hash.data(), static_cast<ULONG>(hash.size()), nullptr, 0, &sig_len,
                       BCRYPT_PAD_PKCS1) == 0 &&
        sig_len > 0) {
        signature.resize(sig_len);
        if (BCryptSignHash(key, &pad_info, hash.data(), static_cast<ULONG>(hash.size()), signature.data(), sig_len,
                           &sig_len, BCRYPT_PAD_PKCS1) == 0) {
            signature.resize(sig_len);
        }
        else {
            signature.clear();
        }
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return signature;
}

bool Crypto::verify(const std::vector<std::uint8_t> &public_key_x509, const std::uint8_t *message, std::size_t length,
                    const std::uint8_t *signature, std::size_t sig_length)
{
    auto blob = x509ToPublicBlob(public_key_x509.data(), public_key_x509.size());
    if (blob.empty()) {
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()),
                            0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    auto hash = sha256(message, length);
    bool ok = false;
    if (!hash.empty()) {
        BCRYPT_PKCS1_PADDING_INFO pad_info = {};
        pad_info.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        ok =
            BCryptVerifySignature(key, &pad_info, hash.data(), static_cast<ULONG>(hash.size()),
                                  const_cast<PUCHAR>(signature), static_cast<ULONG>(sig_length), BCRYPT_PAD_PKCS1) == 0;
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::vector<std::uint8_t> Crypto::decodePublicKey(const std::vector<std::uint8_t> &x509_bytes)
{
    // Validate by parsing; return the original bytes if valid.
    auto blob = x509ToPublicBlob(x509_bytes.data(), x509_bytes.size());
    if (blob.empty()) {
        return {};
    }
    return x509_bytes;
}

}  // namespace spark
