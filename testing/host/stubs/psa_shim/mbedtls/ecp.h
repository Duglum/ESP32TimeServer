// Host build of TF-PSA-Crypto: ESP-IDF patches its sources to include "mbedtls/ecp.h", which its port
// (components/mbedtls/port/include/mbedtls/ecp.h) maps to the private header. This does the same without the
// hardware declarations.
#pragma once
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecp.h"
