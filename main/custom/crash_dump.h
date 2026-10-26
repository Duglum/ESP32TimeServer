// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Core dump of the last crash (review E-07): ESP-IDF writes it to the "coredump" partition when the firmware panics.
// It is checked once at the start (logged, summary kept); the web admin page shows the summary, offers the dump for
// download and erases it. Reading the flash stops the cache for a moment, so it is only read on request.

#pragma once

#include <cstddef>
#include <cstdint>

struct crash_dump_info_t
{
    bool present;        // a complete core dump is stored
    char task[16];       // task that crashed
    uint32_t pc;         // program counter of the exception
    char reason[96];     // panic reason, e.g. "Guru Meditation Error ..." or the failed assertion
    uint32_t size;       // bytes in the partition (ELF core file with header and checksum)
};

// At the start (before the web admin): checks the partition, keeps the summary and logs a stored core dump
void crash_dump_init();

void crash_dump_get(crash_dump_info_t *info);

// Reads part of the stored core dump (web admin download). false = none stored or a flash error.
bool crash_dump_read(size_t offset, void *buffer, size_t length);

// Erases the stored core dump (the next crash writes a new one in any case)
bool crash_dump_erase();
