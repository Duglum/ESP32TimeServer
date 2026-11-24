// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Objects and arrays of the heap that give nullptr when there is no room for them.
//
// new (std::nothrow) does not: the firmware is built without C++ exceptions, and then the nothrow operator new of
// libstdc++ calls the throwing one, which on a failed malloc() calls __cxa_allocate_exception, ESP-IDF's stub of a build
// without exceptions, and that calls abort() (read in the image of 8217270 and measured on the board on 2026-11-24: a
// failed new (std::nothrow) restarted the device with a core dump, the null check after it never ran). So main/ has no
// new (std::nothrow): single objects come from big_new() of sys/big_alloc.h (shared with ESP32-WebTherm, not changed
// here), arrays and the objects that must stay in the memory operator new took for them come from this file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

#include "esp_heap_caps.h"
#include "sys/big_alloc.h"

// The array form of big_new() / big_delete(), as ESP32-ConsoleServer has them in its big_alloc.h (047eba0): remove these
// two here when big_alloc.h gets them.
// count value-initialised T in one block (T not an array type: an array of them is a member of a structure for
// big_new()); nullptr when neither PSRAM nor internal RAM has room. Given back with big_delete_array(), same count.
template <typename T>
T *big_new_array(size_t count)
{
    static_assert(!std::is_array_v<T>, "an array of arrays: a structure with big_new()");
    if (count == 0 || count > SIZE_MAX / sizeof(T))
        return nullptr;
    void *memory = heap_caps_malloc_prefer(count * sizeof(T), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    if (memory == nullptr)
        return nullptr;
    T *array = static_cast<T *>(memory);
    for (size_t i = 0; i < count; ++i)
        new (&array[i]) T{};
    return array;
}

// Zeroed after the destructors, before it is freed (as big_delete())
template <typename T>
void big_delete_array(T *array, size_t count)
{
    if (array == nullptr)
        return;
    for (size_t i = count; i > 0; --i)
        array[i - 1].~T();
    mbedtls_platform_zeroize(array, count * sizeof(T));
    free(array);
}

// A value-initialised T from malloc(), the memory operator new takes (internal RAM; with CONFIG_SPIRAM_USE_MALLOC a
// block above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes may be PSRAM): for objects that stay where operator new put
// them before, such as the tables of the MQTT task. nullptr when there is no room.
template <typename T>
T *heap_new()
{
    void *memory = malloc(sizeof(T));
    return memory == nullptr ? nullptr : new (memory) T{};
}

// count value-initialised T from malloc() in one block (see heap_new()); nullptr when there is no room or count is 0.
// Given back with heap_delete_array(), same count.
template <typename T>
T *heap_new_array(size_t count)
{
    static_assert(!std::is_array_v<T>, "an array of arrays: a structure with heap_new()");
    if (count == 0 || count > SIZE_MAX / sizeof(T))
        return nullptr;
    void *memory = malloc(count * sizeof(T));
    if (memory == nullptr)
        return nullptr;
    T *array = static_cast<T *>(memory);
    for (size_t i = 0; i < count; ++i)
        new (&array[i]) T{};
    return array;
}

// Not zeroed: for objects without secrets (use big_delete_array() for those)
template <typename T>
void heap_delete_array(T *array, size_t count)
{
    if (array == nullptr)
        return;
    for (size_t i = count; i > 0; --i)
        array[i - 1].~T();
    free(array);
}
