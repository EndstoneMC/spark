#ifndef ENDSTONE_SPARK_CRYPTO_H
#define ENDSTONE_SPARK_CRYPTO_H

#include <cstdint>
#include <string>
#include <vector>

namespace spark {

// RSA2048 with SHA256 signing, matching the upstream spark CryptoAlgorithm.RSA2048.
class Crypto {
public:
    struct KeyPair {
        std::vector<std::uint8_t> public_key_x509;  // X.509 SubjectPublicKeyInfo DER
        // Private key is stored as PKCS#8 DER. Opaque to callers.
        std::vector<std::uint8_t> private_key_pkcs8;
    };

    static constexpr int kVersion = 1;  // Protocol version

    // Generate a new RSA2048 key pair.
    static KeyPair generateKeyPair();

    // Sign a message with SHA256withRSA. Returns empty on failure.
    static std::vector<std::uint8_t> sign(const std::vector<std::uint8_t> &private_key_pkcs8,
                                          const std::uint8_t *message, std::size_t length);

    // Verify a signature. Returns false on failure or mismatch.
    static bool verify(const std::vector<std::uint8_t> &public_key_x509, const std::uint8_t *message,
                       std::size_t length, const std::uint8_t *signature, std::size_t sig_length);

    // Decode a raw X.509 SubjectPublicKeyInfo DER blob into a usable public key.
    // Returns empty vector on failure.
    static std::vector<std::uint8_t> decodePublicKey(const std::vector<std::uint8_t> &x509_bytes);
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_CRYPTO_H
