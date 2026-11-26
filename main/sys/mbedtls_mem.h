// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The memory functions of mbedTLS. With CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC ESP-IDF 6.1.1 compiles neither its own
// esp_mbedtls_mem_calloc() / esp_mbedtls_mem_free() (port/esp_mem.c) nor points mbedTLS at them (esp_config.h): until
// mbedtls_platform_set_calloc_free() is called, mbedTLS takes calloc() and free() of the C library
// (tf-psa-crypto platform.h, platform.c). So the firmware calls mbedtls_mem_install() in app_main() (its third
// statement, after bootloader_random_enable() and cjson_mem_init(); nothing before it there uses mbedTLS), before the
// web server, MQTT and the TLS identity use mbedTLS. ESP-IDF's secondary initialization calls psa_crypto_init()
// before app_main() (components/mbedtls/port/esp_psa_crypto_init.c): a block taken there (with calloc(), from the same
// heap) is freed through esp_mbedtls_mem_free() as well, and secret_free() handles every block of the heap.
#pragma once

// mbedTLS gets esp_mbedtls_mem_calloc() and esp_mbedtls_mem_free() of mbedtls_mem.cpp
void mbedtls_mem_install();

// The allocations of mbedTLS (and PSA Crypto) that failed since the start, in every task. mbedTLS does not always report
// a lack of memory as such: a failed allocation inside a key or certificate parse or a signature can end in an error of
// the format, of the public key or of an argument. A caller that must tell a lack of memory from a stored key or
// certificate that is not valid compares the count before and after the call; a failure of another task at the same
// time counts too (the caller then takes the safe side).
unsigned mbedtls_mem_failures();
