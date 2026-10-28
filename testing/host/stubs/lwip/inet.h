// ESP32 Time Server - host tests: the IPv6 address type of lwIP (see host_stubs.h)
#pragma once
#include <stdint.h>

struct in6_addr
{
    union
    {
        uint32_t u32_addr[4];
        uint8_t u8_addr[16];
    } un;
};
#define s6_addr un.u8_addr
