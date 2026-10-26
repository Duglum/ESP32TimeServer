// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Diagnostics of the time keeping without an oscilloscope (review 4.5):
//   - stability of the GNSS PPS against the free-running PTP clock: overlapping Allan deviation (ADEV) and time
//     deviation (TDEV) of the phase measured at every PPS edge, with the frequency commands of the servo taken out
//   - RMS of the servo innovations over the last minute and the last hour
//   - excess of the PPS interrupt time over the central value of its latency (histogram)
// The PPS task writes once per second; the web server and MQTT read. Nothing here is in the time path, and a
// reader never blocks the PPS task (it skips publishing a second instead).

#pragma once

#include <cstddef>
#include <cstdint>

// One phase sample per second: 4096 s of data, ADEV and TDEV up to tau = 1024 s
static constexpr size_t TIME_DIAG_PHASE_SAMPLES = 4096;
static constexpr size_t TIME_DIAG_TAUS = 11; // tau = 1, 2, 4, ..., 1024 s
static constexpr size_t TIME_DIAG_ISR_BINS = 6; // excess up to 0.25, 0.5, 1, 2, 5 us and above

enum class time_diag_source_t : uint8_t
{
    none = 0,
    loopback = 1, // PTP PPS loopback (capture, 12.5 ns)
    software = 2  // software readings of the PTP clock
};

void time_diag_init();

// PPS task, once per PPS edge with a PTP measurement.
// measured_ns: raw PTP - true time at the edge as the servo uses it (qErr corrected; for an outlier the prediction
//   of the filter, as the filter coasts on it)
// commanded_ns: phase the frequency commands added since the previous edge
// free_rate_ppb: estimated rate of the PTP clock without the commands (removed from the stored values, which keeps
//   them small; a constant rate does not change ADEV or TDEV)
// restart: the series starts again with this sample (start of the filter, clock stepped, new measurement source).
//   A missing second or another source restarts it as well.
void time_diag_add_phase(int64_t edge_sec, double measured_ns, double commanded_ns, double free_rate_ppb,
                         time_diag_source_t source, bool restart);

// PPS task: innovation of a normal filter update (not of outliers or starts)
void time_diag_add_innovation(int64_t edge_sec, double innovation_ns);

// PPS task: interrupt time of a PPS edge minus the central value of the interrupt latency, in ns
void time_diag_add_isr_excess(double excess_ns);

struct time_diag_stability_t
{
    uint32_t samples;          // seconds of continuous phase data used
    time_diag_source_t source; // of these samples
    uint8_t count;             // taus with a value (tau_s[0 .. count - 1])
    uint16_t tau_s[TIME_DIAG_TAUS];
    float adev[TIME_DIAG_TAUS];    // overlapping Allan deviation (dimensionless)
    float tdev_ns[TIME_DIAG_TAUS]; // time deviation in ns (0 = not enough data for this tau)
};

// Computes ADEV and TDEV from the current series (some 10 ms, call from a task with time, e.g. the web server).
// Returns false if there are fewer than 3 samples.
bool time_diag_stability(time_diag_stability_t *out);

struct time_diag_innovation_t
{
    float rms_minute_ns;     // RMS of the innovations in the last complete minute
    uint32_t minute_samples;
    float rms_hour_ns;       // over the complete minutes of the last hour
    uint32_t hour_samples;
    uint32_t age_s;          // seconds since the last complete minute (UINT32_MAX = none yet)
};

void time_diag_innovation(time_diag_innovation_t *out);

struct time_diag_isr_t
{
    uint32_t bins[TIME_DIAG_ISR_BINS];
    uint32_t total;
    uint32_t max_ns;
};

void time_diag_isr(time_diag_isr_t *out);
