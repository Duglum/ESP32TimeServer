// ESP32 Time Server host tests (from ESP32-ConsoleServer): checks and the entry points of the test programs and fuzz targets.
//
// A test file defines run_tests() (unit tests, built without HOST_FUZZ) and fuzz_one(data, size) (built with
// HOST_FUZZ as a libFuzzer target). A failed CHECK in a fuzz target aborts, so libFuzzer saves the input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

inline int g_checks = 0;
inline int g_failures = 0;

#ifdef HOST_FUZZ
#define CHECK(cond)                                                                          \
    do                                                                                       \
    {                                                                                        \
        if (!(cond))                                                                         \
        {                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            abort();                                                                         \
        }                                                                                    \
    } while (0)
#else
#define CHECK(cond)                                                                          \
    do                                                                                       \
    {                                                                                        \
        ++g_checks;                                                                          \
        if (!(cond))                                                                         \
        {                                                                                    \
            ++g_failures;                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                                    \
    } while (0)
#endif

#define CHECK_STR(actual, expected)                                                          \
    do                                                                                       \
    {                                                                                        \
        const char *a_ = (actual);                                                           \
        const char *e_ = (expected);                                                         \
        const bool same_ = a_ != nullptr && strcmp(a_, e_) == 0;                             \
        if (!same_)                                                                          \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", e_); \
        CHECK(same_);                                                                        \
    } while (0)

void run_tests();
void fuzz_one(const uint8_t *data, size_t size);

#ifdef HOST_FUZZ
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_one(data, size);
    return 0;
}
#else
int main()
{
    run_tests();
    printf("%s: %d checks, %d failed\n", TEST_NAME, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
#endif
