// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_tls.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
}

namespace
{
    const char *TAG = "web_tls";

    constexpr char NVS_NAMESPACE[] = "webtls";
    constexpr char NVS_KEY_KEY[] = "key";   // private key, DER
    constexpr char NVS_KEY_CERT[] = "crt";  // certificate, DER
    constexpr char NVS_KEY_NAME[] = "name"; // FQDN the certificate was issued for
    constexpr size_t MAX_DER = 1024;

    // Fixed validity: the time may not be known yet when the certificate is created
    constexpr char NOT_BEFORE[] = "20260101000000";
    constexpr char NOT_AFTER[] = "20991231235959";

    bool read_blob(nvs_handle_t handle, const char *key, uint8_t *&data, size_t &length)
    {
        length = 0;
        if (nvs_get_blob(handle, key, nullptr, &length) != ESP_OK || length == 0 || length > MAX_DER)
            return false;
        data = static_cast<uint8_t *>(malloc(length));
        if (data == nullptr)
            return false;
        if (nvs_get_blob(handle, key, data, &length) != ESP_OK)
        {
            free(data);
            data = nullptr;
            return false;
        }
        return true;
    }

    // Generates a new P-256 key pair with the PSA RNG (hardware entropy) and returns it as a PK context
    bool generate_key(mbedtls_pk_context &pk)
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_key_id_t key_id = 0;
        if (psa_generate_key(&attributes, &key_id) != PSA_SUCCESS)
            return false;
        const bool ok = mbedtls_pk_copy_from_psa(key_id, &pk) == 0;
        psa_destroy_key(key_id);
        return ok;
    }

    // Issues the self-signed certificate. Returns the DER length (0 on error); the DER is at the end of buf.
    size_t issue_certificate(mbedtls_pk_context &pk, const char *fqdn, uint8_t *buf, size_t size, uint8_t *&der)
    {
        mbedtls_x509write_cert crt;
        mbedtls_x509write_crt_init(&crt);
        char subject[300];
        snprintf(subject, sizeof(subject), "CN=%s,O=ESP32 Time Server", fqdn);

        uint8_t serial[16];
        esp_fill_random(serial, sizeof(serial));
        serial[0] &= 0x7F; // positive
        serial[0] |= 0x40; // no leading zero byte

        mbedtls_x509_san_list san{};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(fqdn));
        san.node.san.unstructured_name.len = strlen(fqdn);
        san.next = nullptr;

        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        int ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_validity(&crt, NOT_BEFORE, NOT_AFTER);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
        if (ret == 0)
            ret = mbedtls_x509write_crt_der(&crt, buf, size);
        mbedtls_x509write_crt_free(&crt);
        if (ret <= 0)
        {
            ESP_LOGE(TAG, "Certificate could not be created (-0x%04x)", static_cast<unsigned int>(-ret));
            return 0;
        }
        der = buf + size - ret;
        return static_cast<size_t>(ret);
    }
} // namespace

bool web_tls_load_or_create(const char *fqdn, web_tls_identity_t &identity)
{
    identity = web_tls_identity_t{};
    psa_crypto_init();

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS namespace %s unavailable (%s)", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }

    char stored_name[256] = "";
    size_t name_length = sizeof(stored_name);
    const bool have_name = nvs_get_str(handle, NVS_KEY_NAME, stored_name, &name_length) == ESP_OK;
    const bool have_key = read_blob(handle, NVS_KEY_KEY, identity.key_der, identity.key_length);
    const bool have_cert = read_blob(handle, NVS_KEY_CERT, identity.certificate_der, identity.certificate_length);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool key_ok = have_key && mbedtls_pk_parse_key(&pk, identity.key_der, identity.key_length, nullptr, 0) == 0;
    bool ok = key_ok && have_cert && have_name && strcmp(stored_name, fqdn) == 0;

    if (!ok)
    {
        bool new_key = false;
        if (!key_ok)
        {
            mbedtls_pk_free(&pk);
            mbedtls_pk_init(&pk);
            ESP_LOGI(TAG, "Creating the TLS key of the web admin (ECDSA P-256)");
            if (!generate_key(pk))
            {
                ESP_LOGE(TAG, "TLS key generation failed");
                mbedtls_pk_free(&pk);
                nvs_close(handle);
                return false;
            }
            new_key = true;
        }
        ESP_LOGI(TAG, "Issuing the self-signed certificate for %s", fqdn);

        uint8_t *buffer = static_cast<uint8_t *>(malloc(2 * MAX_DER));
        uint8_t *der = nullptr;
        size_t cert_length = buffer == nullptr ? 0 : issue_certificate(pk, fqdn, buffer, MAX_DER, der);
        int key_length = buffer == nullptr ? 0 : mbedtls_pk_write_key_der(&pk, buffer + MAX_DER, MAX_DER);
        if (cert_length == 0 || key_length <= 0)
        {
            free(buffer);
            mbedtls_pk_free(&pk);
            nvs_close(handle);
            return false;
        }
        const uint8_t *key_der = buffer + 2 * MAX_DER - key_length;

        free(identity.certificate_der);
        free(identity.key_der);
        identity.certificate_der = static_cast<uint8_t *>(malloc(cert_length));
        identity.key_der = static_cast<uint8_t *>(malloc(key_length));
        if (identity.certificate_der == nullptr || identity.key_der == nullptr)
        {
            memset(buffer, 0, 2 * MAX_DER);
            free(buffer);
            mbedtls_pk_free(&pk);
            nvs_close(handle);
            return false;
        }
        memcpy(identity.certificate_der, der, cert_length);
        memcpy(identity.key_der, key_der, key_length);
        identity.certificate_length = cert_length;
        identity.key_length = static_cast<size_t>(key_length);
        memset(buffer, 0, 2 * MAX_DER);
        free(buffer);

        err = new_key ? nvs_set_blob(handle, NVS_KEY_KEY, identity.key_der, identity.key_length) : ESP_OK;
        if (err == ESP_OK)
            err = nvs_set_blob(handle, NVS_KEY_CERT, identity.certificate_der, identity.certificate_length);
        if (err == ESP_OK)
            err = nvs_set_str(handle, NVS_KEY_NAME, fqdn);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "TLS identity could not be stored in NVS (%s): a new one is created at the next start", esp_err_to_name(err));
    }
    mbedtls_pk_free(&pk);
    nvs_close(handle);

    uint8_t hash[32];
    size_t hash_length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, identity.certificate_der, identity.certificate_length, hash, sizeof(hash), &hash_length) != PSA_SUCCESS)
        return false;
    char *p = identity.fingerprint;
    for (size_t i = 0; i < sizeof(hash); ++i)
        p += snprintf(p, 4, i == 0 ? "%02X" : ":%02X", hash[i]);
    return true;
}
