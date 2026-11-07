#pragma once

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace orbit::testing {

// A throwaway self-signed certificate for "localhost", generated at test time so
// that no private key is ever checked in.
struct TestCertificate {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    explicit TestCertificate(const std::filesystem::path& directory) {
        cert_path = directory / "cert.pem";
        key_path = directory / "key.pem";

        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* cert = X509_new();
        if (key == nullptr || cert == nullptr) {
            throw std::runtime_error("cannot generate test key");
        }
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
        X509_set_pubkey(cert, key);

        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, name);

        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* san =
            X509V3_EXT_nconf_nid(nullptr, &context, NID_subject_alt_name, "DNS:localhost");
        X509_add_ext(cert, san, -1);
        X509_EXTENSION_free(san);
        X509_sign(cert, key, EVP_sha256());

        std::FILE* cert_file = std::fopen(cert_path.c_str(), "w");
        std::FILE* key_file = std::fopen(key_path.c_str(), "w");
        PEM_write_X509(cert_file, cert);
        PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(cert_file);
        std::fclose(key_file);

        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

}  // namespace orbit::testing
