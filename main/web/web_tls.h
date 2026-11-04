// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// TLS identity of the web admin page, stored in NVS (namespace "webtls"):
// - self-signed (default): an ECDSA P-256 key generated on the device (hardware RNG) and a self-signed certificate. The
//   key never leaves the device and is not part of the firmware. The reset to defaults keeps it. The certificate is
//   issued again (same key) when the FQDN changes.
// - own: a certificate chain and its private key in PEM, given by the owner (page, API, configuration file). Checked
//   with mbedTLS before it is stored: the chain parses, the key is not encrypted, ECDSA P-256/P-384 or RSA 2048 to 4096
//   bit, and it belongs to the first certificate. The reset to defaults erases it and returns to self-signed.
// The self-signed identity is kept while the own one is in use, so switching back gives the same certificate.

#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t WEB_TLS_CERT_PEM_MAX = 8192; // characters of the certificate chain (PEM)
constexpr size_t WEB_TLS_KEY_PEM_MAX = 4096;  // characters of the private key (PEM)
constexpr size_t WEB_TLS_SAN_MAX = 8;          // subject alternative names reported

// The identity in use. The buffers belong to web_tls and stay valid until the next web_tls_set() or
// web_tls_clear_own(); a TLS session copies them when it starts.
struct web_tls_identity_t
{
    const uint8_t *certificate; // self-signed: DER; own: PEM chain with its terminating NUL
    size_t certificate_length;
    const uint8_t *key;         // self-signed: DER; own: PEM with its terminating NUL
    size_t key_length;
    bool own;
    char fingerprint[96]; // SHA-256 of the (first) certificate, "AB:CD:..."
};

// What an own certificate contains
struct web_tls_info_t
{
    char subject[160];
    char issuer[160];
    char san[WEB_TLS_SAN_MAX][64]; // DNS names and IP addresses
    size_t san_count;
    int64_t not_before; // Unix seconds (UTC)
    int64_t not_after;
    char key_type[24]; // "ECDSA P-256", "RSA 2048"
    size_t chain_length;
    char fingerprint[96];
};

// Loads the identities from NVS or creates the self-signed one; fills the one in use. fqdn = name for the subject and
// the subject alternative name of the self-signed certificate.
bool web_tls_load_or_create(const char *fqdn, web_tls_identity_t &identity);

// Checks a certificate chain and a private key (PEM). key_pem nullptr or "": the stored own key. err: for the page.
bool web_tls_check_own(const char *cert_pem, const char *key_pem, web_tls_info_t &info, char *err, size_t err_size);

// own: stores the chain and key (checked again; cert_pem or key_pem nullptr or "" keeps the stored one) and uses them.
// !own: uses the self-signed identity, the own data stays stored. Fills identity with the one in use now.
bool web_tls_set(bool own, const char *cert_pem, const char *key_pem, web_tls_identity_t &identity, char *err, size_t err_size);

// What the stored own certificate contains (false when there is none), from its check at the start or when it was
// stored; a key is always stored with it
bool web_tls_own_info(web_tls_info_t &info);

// The stored own data as PEM, for the configuration file (nullptr when there is none). Valid until the next change.
const char *web_tls_own_certificate_pem();
const char *web_tls_own_key_pem();

// Reset to defaults: erases the own certificate and key and returns to self-signed (takes effect at the next start)
void web_tls_clear_own();
