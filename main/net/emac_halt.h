// ESP32 Time Server (from ESP32-WebTherm)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The EMAC is stopped before every CPU reset and at the start of the firmware, before its variables are used (see
// emac_halt.cpp). A CPU reset (esp_restart(), the restart after a panic) does not stop the EMAC: its DMA keeps
// storing received frames into the receive buffers of the firmware that ran before, which can be the variables of
// the firmware that starts.

#pragma once

#include <cstdint>

enum class emac_start_state_t : uint8_t
{
    not_checked, // the check at the start did not run (ESP-IDF calls cache_hal_init() no more)
    idle,        // the EMAC was off, in reset or stopped
    stopped,     // its DMA was started (the firmware before did not stop it): stopped and reset
};

// What the check at the start found (before the Ethernet driver exists)
emac_start_state_t emac_halt_start_state();

// Stops the receive and transmit DMA and the MAC and resets the EMAC (only before the Ethernet driver is created:
// for a start whose check did not run). Returns true when the DMA was started.
bool emac_halt_now();
