// ESP32 Time Server - host tests
// Test access to the internals of the tested sources (see gnss_uart_hooks.cpp) and a small check helper.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// UART reader: parser state cleared, gnss_uart_running() as given (commands need it), traffic counters reset
void host_gnss_uart_reset(bool running);
// Bytes as read from the driver, with the flush generation of the read
void host_gnss_uart_feed(const uint8_t *data, size_t length, uint32_t generation);

// ---- Checks: a failed one is printed and counted, the test goes on ----
inline int host_failures = 0;
inline int host_checks = 0;
#define CHECK(condition)                                                                    \
    do                                                                                      \
    {                                                                                       \
        ++host_checks;                                                                      \
        if (!(condition))                                                                   \
        {                                                                                   \
            ++host_failures;                                                                \
            std::printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);      \
        }                                                                                   \
    } while (0)
#define CHECK_EQ(actual, expected)                                                          \
    do                                                                                      \
    {                                                                                       \
        ++host_checks;                                                                      \
        const long long actual_value = static_cast<long long>(actual);                      \
        const long long expected_value = static_cast<long long>(expected);                  \
        if (actual_value != expected_value)                                                 \
        {                                                                                   \
            ++host_failures;                                                                \
            std::printf("%s:%d: check failed: %s == %lld, expected %lld\n", __FILE__, __LINE__, \
                        #actual, actual_value, expected_value);                             \
        }                                                                                   \
    } while (0)
