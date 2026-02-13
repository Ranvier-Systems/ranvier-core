// Ranvier Core - DtlsContext Unit Tests
//
// Tests for memory-based certificate loading (load_certs_from_memory).
// These tests run without the Seastar event loop — they exercise the
// CPU-only OpenSSL helpers that replaced the blocking file I/O calls.

#include "dtls_context.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <string>

namespace ranvier {
namespace {

//------------------------------------------------------------------------------
// Test Helpers — Generate self-signed PEM certs in memory
//------------------------------------------------------------------------------

// Generate an RSA key pair and return as EVP_PKEY*.
// Caller owns the returned pointer.
static EVP_PKEY* generate_rsa_key(int bits = 2048) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) return nullptr;
    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

// Create a self-signed X509 certificate.
// Caller owns the returned pointer.
static X509* create_self_signed_cert(EVP_PKEY* pkey, const char* cn, long serial = 1) {
    X509* cert = X509_new();
    if (!cert) return nullptr;

    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600L);

    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert, name);  // Self-signed: issuer == subject

    if (X509_sign(cert, pkey, EVP_sha256()) <= 0) {
        X509_free(cert);
        return nullptr;
    }
    return cert;
}

// Serialize X509 cert to PEM string.
static std::string x509_to_pem(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, len);
    BIO_free(bio);
    return pem;
}

// Serialize EVP_PKEY to PEM string (unencrypted).
static std::string pkey_to_pem(EVP_PKEY* pkey) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, len);
    BIO_free(bio);
    return pem;
}

//------------------------------------------------------------------------------
// Test Fixture
//------------------------------------------------------------------------------

class DtlsLoadCertsFromMemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate CA key + self-signed CA cert
        _ca_key = generate_rsa_key();
        ASSERT_NE(_ca_key, nullptr);
        _ca_cert = create_self_signed_cert(_ca_key, "Test CA");
        ASSERT_NE(_ca_cert, nullptr);

        // Generate node key + self-signed node cert (for simplicity, self-signed
        // with the same key; production uses CA-signed certs)
        _node_key = generate_rsa_key();
        ASSERT_NE(_node_key, nullptr);
        _node_cert = create_self_signed_cert(_node_key, "test-node");
        ASSERT_NE(_node_cert, nullptr);

        _cert_pem = x509_to_pem(_node_cert);
        _key_pem = pkey_to_pem(_node_key);
        _ca_pem = x509_to_pem(_ca_cert);

        // Create a fresh SSL_CTX for each test
        _ctx = SSL_CTX_new(DTLS_method());
        ASSERT_NE(_ctx, nullptr);
    }

    void TearDown() override {
        if (_ctx) SSL_CTX_free(_ctx);
        if (_ca_cert) X509_free(_ca_cert);
        if (_ca_key) EVP_PKEY_free(_ca_key);
        if (_node_cert) X509_free(_node_cert);
        if (_node_key) EVP_PKEY_free(_node_key);
    }

    EVP_PKEY* _ca_key = nullptr;
    X509* _ca_cert = nullptr;
    EVP_PKEY* _node_key = nullptr;
    X509* _node_cert = nullptr;
    SSL_CTX* _ctx = nullptr;

    std::string _cert_pem;
    std::string _key_pem;
    std::string _ca_pem;
};

//------------------------------------------------------------------------------
// Success Cases
//------------------------------------------------------------------------------

TEST_F(DtlsLoadCertsFromMemoryTest, ValidCertsLoadSuccessfully) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, _ca_pem);
    EXPECT_FALSE(err.has_value()) << "Unexpected error: " << err.value_or("");
}

TEST_F(DtlsLoadCertsFromMemoryTest, KeyMatchesCertAfterLoad) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, _ca_pem);
    ASSERT_FALSE(err.has_value());

    // Verify the loaded key matches the loaded cert
    EXPECT_TRUE(SSL_CTX_check_private_key(_ctx));
}

TEST_F(DtlsLoadCertsFromMemoryTest, MultipleCAcertsInBundle) {
    // Create a second CA cert
    EVP_PKEY* ca2_key = generate_rsa_key();
    ASSERT_NE(ca2_key, nullptr);
    X509* ca2_cert = create_self_signed_cert(ca2_key, "Test CA 2", 2);
    ASSERT_NE(ca2_cert, nullptr);

    // Concatenate two CA certs into a bundle
    std::string ca_bundle = _ca_pem + x509_to_pem(ca2_cert);

    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, ca_bundle);
    EXPECT_FALSE(err.has_value()) << "Unexpected error: " << err.value_or("");

    X509_free(ca2_cert);
    EVP_PKEY_free(ca2_key);
}

TEST_F(DtlsLoadCertsFromMemoryTest, CertWithChainCertsInPEM) {
    // Create an "intermediate" cert and append it to the cert PEM
    EVP_PKEY* inter_key = generate_rsa_key();
    ASSERT_NE(inter_key, nullptr);
    X509* inter_cert = create_self_signed_cert(inter_key, "Intermediate CA", 3);
    ASSERT_NE(inter_cert, nullptr);

    std::string cert_chain = _cert_pem + x509_to_pem(inter_cert);

    auto err = DtlsContext::load_certs_from_memory(_ctx, cert_chain, _key_pem, _ca_pem);
    EXPECT_FALSE(err.has_value()) << "Unexpected error: " << err.value_or("");

    X509_free(inter_cert);
    EVP_PKEY_free(inter_key);
}

//------------------------------------------------------------------------------
// Failure Cases
//------------------------------------------------------------------------------

TEST_F(DtlsLoadCertsFromMemoryTest, EmptyCertPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, "", _key_pem, _ca_pem);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("certificate"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, EmptyKeyPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, "", _ca_pem);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("key"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, EmptyCAPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, "");
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("CA"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, GarbageCertPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, "not a certificate", _key_pem, _ca_pem);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("certificate"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, GarbageKeyPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, "not a key", _ca_pem);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("key"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, GarbageCAPEM) {
    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, "not a CA cert");
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("CA"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, MismatchedKeyAndCert) {
    // Generate a different key that doesn't match _node_cert
    EVP_PKEY* wrong_key = generate_rsa_key();
    ASSERT_NE(wrong_key, nullptr);
    std::string wrong_key_pem = pkey_to_pem(wrong_key);
    EVP_PKEY_free(wrong_key);

    auto err = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, wrong_key_pem, _ca_pem);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("does not match"), std::string::npos);
}

TEST_F(DtlsLoadCertsFromMemoryTest, NullSSLCtx) {
    // Passing nullptr should fail (not crash)
    auto err = DtlsContext::load_certs_from_memory(nullptr, _cert_pem, _key_pem, _ca_pem);
    ASSERT_TRUE(err.has_value());
}

//------------------------------------------------------------------------------
// Idempotency / Reload Simulation
//------------------------------------------------------------------------------

TEST_F(DtlsLoadCertsFromMemoryTest, ReloadWithNewCerts) {
    // First load
    auto err1 = DtlsContext::load_certs_from_memory(_ctx, _cert_pem, _key_pem, _ca_pem);
    ASSERT_FALSE(err1.has_value());

    // Generate new certs
    EVP_PKEY* new_key = generate_rsa_key();
    ASSERT_NE(new_key, nullptr);
    X509* new_cert = create_self_signed_cert(new_key, "new-node", 42);
    ASSERT_NE(new_cert, nullptr);

    std::string new_cert_pem = x509_to_pem(new_cert);
    std::string new_key_pem = pkey_to_pem(new_key);

    // Create a fresh SSL_CTX for the "reload" (mirrors check_and_reload_certs logic)
    SSL_CTX* new_ctx = SSL_CTX_new(DTLS_method());
    ASSERT_NE(new_ctx, nullptr);

    auto err2 = DtlsContext::load_certs_from_memory(new_ctx, new_cert_pem, new_key_pem, _ca_pem);
    EXPECT_FALSE(err2.has_value()) << "Unexpected error on reload: " << err2.value_or("");

    SSL_CTX_free(new_ctx);
    X509_free(new_cert);
    EVP_PKEY_free(new_key);
}

}  // namespace
}  // namespace ranvier
