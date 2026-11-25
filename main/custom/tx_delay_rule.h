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
//  - the first TX_DELAY_START_SAMPLES samples: the estimate is their TX_DELAY_START_RANK-th smallest, so one low
//    sample among them does not decide it. main.cpp compensates only after them, and takes no sample while the answer
//    is not sent synchronised or shortly after a PPS outlier. Under a load with no quiet answer among them the start
//    value is a late sample and too high (on the board 26 to 39 us after restarts under new HTTPS connections); the
//    rule for unconfirmed estimates below brings it down. A higher rank, or the smallest sample with two more within
//    the band above it, made such a start higher still (in a simulation on recorded load samples 15 and 30 instead of
//    11 us above the quiet delay) and is not more robust against one low sample;
//  - a sample within -TX_DELAY_BAND_DOWN_NS .. +TX_DELAY_BAND_UP_NS of the estimate moves it by 1/32 of the difference
//    (as the running mean did). The band is symmetric: disturbed samples spread across it do not move the estimate on
//    average (with +5 / -2 us a continuous load walked it up: by 17 us with 5 % quiet samples, in a simulation on
//    synthetic loads; with +-2 us by 2.8 us); quiet samples above +2 us (11 to 40 of 1131 per run) are not used;
//  - a sample above the band is not used; after TX_DELAY_REBASE_UP_SAMPLES of them in a row (none in the band, none
//    below it) the delay has lastingly grown: the estimate becomes the smallest of them. A load gives such runs too,
//    so the count is long: a lasting growth of the undisturbed delay has no known cause in normal running (the path
//    is the same code and hardware), a load is common;
//  - a sample below the band is not used; after TX_DELAY_REBASE_DOWN_SAMPLES of them (only a sample in the band ends
//    the run: a late sample says nothing about the lower edge) the delay has lastingly shrunk: the estimate becomes
//    the second smallest of them (one low glitch and late samples among them do not decide it; a single low sample
//    changes nothing);
//  - a new estimate (the start value, a re-based one) is unconfirmed until TX_DELAY_CONFIRM_SAMPLES samples fell
//    within its band. While it is unconfirmed, a sample in the band does not end a run below it, and
//    TX_DELAY_REBASE_DOWN_UNCONFIRMED samples below re-base it (to the larger of them): a start value taken under a
//    load is a late sample, the late samples near it fall into its band and would keep ending the run, so the quiet
//    answers during the load were stamped too late. Two low samples before the confirmation decide the estimate (none
//    more than 2 us below in 6786 quiet samples after synchronisation: measured).
// The constants were measured for IPv4 answers without authentication on the test board (see the README).
#pragma once

#include <cstdint>

constexpr uint32_t TX_DELAY_START_SAMPLES = 16;       // compensation starts after these (as before)
constexpr int32_t TX_DELAY_BAND_UP_NS = 2000;         // quiet: p90 0.15 to 0.21 us above the mean (measured)
constexpr int32_t TX_DELAY_BAND_DOWN_NS = 2000;       // quiet samples at most 0.41 us below the mean (measured)
constexpr uint16_t TX_DELAY_REBASE_UP_SAMPLES = 1024; // late samples in a row: up to 190 seen at 8 answers/s under a
                                                      // 300 s handshake load (see the README)
constexpr uint16_t TX_DELAY_REBASE_DOWN_SAMPLES = 4;  // quiet: none below -2 us in 6 runs of 1131 answers (measured)
constexpr uint32_t TX_DELAY_START_RANK = 2;           // the start value: the second smallest of the first samples
constexpr uint16_t TX_DELAY_CONFIRM_SAMPLES = 16;     // samples within the band that confirm a new estimate
constexpr uint16_t TX_DELAY_REBASE_DOWN_UNCONFIRMED = 2; // early samples that re-base an unconfirmed estimate
constexpr int32_t TX_DELAY_EWMA_SHIFT = 5;            // weight 1/32 within the band (as before)

struct tx_delay_rule_t
{
    int32_t estimate_ns = 0;
    uint32_t samples = 0;    // samples seen (saturating)
    uint16_t above_run = 0;  // samples above the band in a row
    uint16_t below_run = 0;  // samples below the band in a row
    int32_t above_min_ns = 0;
    int32_t below_min_ns = 0;     // the smallest of the run below the band
    int32_t below_second_ns = 0;  // the second smallest of it
    uint16_t confirmed_by = 0;    // samples within the band since the estimate was set (up to TX_DELAY_CONFIRM_SAMPLES)
    int32_t start_low_ns[TX_DELAY_START_RANK] = {}; // the smallest samples of the start phase, in ascending order
};

enum class tx_delay_step_t : uint8_t
{
    start,        // one of the first samples (after the last of them the estimate is set, see above)
    fed,          // within the band: the estimate moved by 1/32
    above,        // above the band: not used
    below,        // below the band: not used
    rebased_up,   // the delay grew lastingly: the estimate is the smallest of the late samples
    rebased_down, // the delay shrank lastingly: the estimate is the second smallest of the early samples
};

// One sample (ns, 0 .. 500000 as main.cpp checks it). Bounded work, no allocation.
tx_delay_step_t tx_delay_rule_update(tx_delay_rule_t &r, int32_t sample_ns);
