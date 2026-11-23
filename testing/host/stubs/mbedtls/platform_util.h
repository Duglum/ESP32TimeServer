// ESP32 Time Server - host tests: mbedtls_platform_zeroize() of TF-PSA-Crypto for ntp_auth.cpp and ptp_auth.cpp (the
// tests with TF-PSA-Crypto find its own header first)
#pragma once
#include <stddef.h>

static inline void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len-- != 0)
        *p++ = 0;
}
