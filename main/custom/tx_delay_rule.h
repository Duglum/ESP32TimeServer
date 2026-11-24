// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The estimate of the transmit delay of an NTP answer in basic mode (hardware transmit stamp minus the clock reading
// before the transmit call), which main.cpp adds to the T3 of later answers of the same kind. Pure, no ESP-IDF: host
// tested (testing/host/tx_delay_rule_test.cpp).
//
// A disturbance (cache refills while the other core reads much from the flash, a flash write, preemption) only adds
// delay, so the undisturbed delay is the lower edge of the samples, and without disturbance the samples lie close
// together (measured on the test board, 3 runs of about 900 answers: -0.41 to +3.79 us around the mean, p99 at most
// +3.47 us). A late sample must not move the estimate, or the answers after the disturbance carry a T3 that is too late
// (the running mean of every sample, the rule up to 2026-11-24, went from 13.4 to 48.7 us during 60 s of new HTTPS
// connections and from 13.5 to 100.4 us during 40 s of Saves). The rule:
//  - the first TX_DELAY_START_SAMPLES samples: the estimate is their smallest (main.cpp compensates only after them);
//  - a sample within -TX_DELAY_BAND_DOWN_NS .. +TX_DELAY_BAND_UP_NS of the estimate moves it by 1/32 of the difference
//    (as the running mean did). The band is symmetric: disturbed samples spread across it do not move the estimate on
//    average (with +5 / -2 us a continuous load walked it up: by 17 us with 5 % quiet samples, in a simulation on
//    synthetic loads; with +-2 us by 2.8 us); quiet samples above +2 us (8 to 25 of about 890 per run) are not used;
//  - a sample above the band is not used; after TX_DELAY_REBASE_UP_SAMPLES of them in a row (none in the band, none
//    below it) the delay has lastingly grown: the estimate becomes the smallest of them;
//  - a sample below the band is not used; after TX_DELAY_REBASE_DOWN_SAMPLES of them (only a sample in the band ends
//    the run: a late sample says nothing about the lower edge) the delay has lastingly shrunk: the estimate becomes
//    the largest of them (a single low sample changes nothing).
// The constants were measured for IPv4 answers without authentication on the test board (see the README).
#pragma once

#include <cstdint>

constexpr uint32_t TX_DELAY_START_SAMPLES = 16;       // compensation starts after these (as before)
constexpr int32_t TX_DELAY_BAND_UP_NS = 2000;         // quiet: p90 0.15 to 0.21 us above the mean (measured)
constexpr int32_t TX_DELAY_BAND_DOWN_NS = 2000;       // quiet samples at most 0.41 us below the mean (measured)
constexpr uint16_t TX_DELAY_REBASE_UP_SAMPLES = 64;   // late samples in a row: at most 5 at 8 answers/s under load,
                                                      // a 60 s load at 1 answer per 2 s gives 30 (measured)
constexpr uint16_t TX_DELAY_REBASE_DOWN_SAMPLES = 4;  // quiet: none below -2 us in 2700 samples (measured)
constexpr int32_t TX_DELAY_EWMA_SHIFT = 5;            // weight 1/32 within the band (as before)

struct tx_delay_rule_t
{
    int32_t estimate_ns = 0;
    uint32_t samples = 0;    // samples seen (saturating)
    uint16_t above_run = 0;  // samples above the band in a row
    uint16_t below_run = 0;  // samples below the band in a row
    int32_t above_min_ns = 0;
    int32_t below_max_ns = 0;
};

enum class tx_delay_step_t : uint8_t
{
    start,        // one of the first samples (the smallest so far is the estimate)
    fed,          // within the band: the estimate moved by 1/32
    above,        // above the band: not used
    below,        // below the band: not used
    rebased_up,   // the delay grew lastingly: the estimate is the smallest of the late samples
    rebased_down, // the delay shrank lastingly: the estimate is the largest of the early samples
};

// One sample (ns, 0 .. 500000 as main.cpp checks it). Bounded work, no allocation.
tx_delay_step_t tx_delay_rule_update(tx_delay_rule_t &r, int32_t sample_ns);
