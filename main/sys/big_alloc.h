// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Large objects (settings sets of 16 kB, the structures of the web server built from them) in PSRAM when the board
// has some, else in internal RAM. The internal heap has about 200 kB in three pieces at the start: three settings
// sets for /api/state (59 kB in one block) did not fit any more once the e-mail settings were added (core dump
// 2026-11-08).
//
// new (std::nothrow) is no help for them: with exceptions off, the nothrow operator new of libstdc++ calls the
// throwing one, and its failure ends in abort() (__cxa_allocate_exception) instead of returning nullptr.

#pragma once

#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"
#include "sys/secret.h"

// A value-initialised T (zeroed for plain structures); nullptr when neither PSRAM nor internal RAM has room
template <typename T>
T *big_new()
{
    void *memory = heap_caps_malloc_prefer(sizeof(T), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    return memory == nullptr ? nullptr : new (memory) T{};
}

// Zeroed after the destructor, before it is freed: most of these objects hold settings sets with their secrets
// (sys/secret.h)
template <typename T>
void big_delete(T *object)
{
    if (object == nullptr)
        return;
    object->~T();
    mbedtls_platform_zeroize(object, sizeof(T));
    free(object);
}

// An object for the whole run time, for a reference at namespace scope (T &s_x = big_static<T>();): PSRAM joins the
// heap in the CORE init stage, before the C++ constructors run. Each call makes a new object.
template <typename T>
T &big_static()
{
    T *object = big_new<T>();
    if (object == nullptr)
        abort(); // at the start: there is no way on without it
    return *object;
}
