// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// TLS identity of the web admin page: an ECDSA P-256 key generated on the device (hardware RNG) and a self-signed
// certificate, both stored in NVS (namespace "webtls"). The key never leaves the device and is not part of the
// firmware. The factory reset keeps it. The certificate is issued again (same key) when the FQDN changes.

#pragma once

#include <cstddef>
#include <cstdint>

struct web_tls_identity_t
{
    uint8_t *certificate_der;
    size_t certificate_length;
    uint8_t *key_der;
    size_t key_length;
    char fingerprint[96]; // SHA-256 of the certificate, "AB:CD:..."
};

// Loads the identity from NVS or creates it. fqdn = name for the subject and the subject alternative name.
// The buffers stay allocated for the lifetime of the server.
bool web_tls_load_or_create(const char *fqdn, web_tls_identity_t &identity);
