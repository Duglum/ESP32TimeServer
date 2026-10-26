// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Diagnostics of the time keeping, see time_diag.h

#include "custom/time_diag.h"

#include <atomic>
#include <cmath>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{
    static_assert((TIME_DIAG_PHASE_SAMPLES & (TIME_DIAG_PHASE_SAMPLES - 1)) == 0, "power of two");
    // A reader leaves this many of the oldest slots out: the PPS task (one sample per second) cannot reach the
    // samples it reads within the computation (some 10 ms)
    constexpr uint32_t PHASE_READ_MARGIN = 8;
    constexpr uint32_t HOUR_MINUTES = 60;
    constexpr uint32_t ISR_BIN_LIMIT_NS[TIME_DIAG_ISR_BINS - 1] = {250, 500, 1000, 2000, 5000};

    // ---- Phase series (ring of TIME_DIAG_PHASE_SAMPLES floats, 16 kB) ----
    // Values in ns relative to the first sample of the series, without the frequency commands and without the rate
    // estimated at the start of the series. The stored floats stay below some 100 us, exact to 0.01 ns.
    float s_phase[TIME_DIAG_PHASE_SAMPLES];
    std::atomic<uint32_t> s_phase_head{0};   // samples written so far (the next one goes to head % size)
    std::atomic<uint32_t> s_series_start{0}; // head index of the first sample of the current series
    std::atomic<uint8_t> s_series_source{0};
    // writer state (PPS task only)
    bool s_have_series = false;
    time_diag_source_t s_source = time_diag_source_t::none;
    int64_t s_last_sec = 0;
    double s_x0_ns = 0.0;        // measurement of the first sample
    double s_commanded_ns = 0.0; // phase the commands added since the first sample
    double s_rate_ppb = 0.0;     // rate taken out (estimate at the start of the series)
    uint32_t s_seconds = 0;      // seconds since the first sample

    // ---- Innovation RMS ----
    struct minute_t
    {
        int64_t minute; // edge second / 60
        double sum_sq_ns2;
        uint32_t samples;
    };
    minute_t s_minutes[HOUR_MINUTES] = {};
    uint32_t s_minute_next = 0; // ring index for the next complete minute
    int64_t s_current_minute = INT64_MIN;
    double s_current_sum_sq = 0.0;
    uint32_t s_current_samples = 0;
    bool s_publish_pending = false;
    time_diag_innovation_t s_pending{}; // computed, not yet published (mutex busy)
    // published values (s_mutex), with the esp_timer time of the minute's end
    SemaphoreHandle_t s_mutex = nullptr;
    time_diag_innovation_t s_published{0.0f, 0, 0.0f, 0, UINT32_MAX};
    int64_t s_published_us = 0;
    int64_t s_pending_us = 0;

    // ---- PPS interrupt excess ----
    std::atomic<uint32_t> s_isr_bins[TIME_DIAG_ISR_BINS] = {};
    std::atomic<uint32_t> s_isr_total{0};
    std::atomic<uint32_t> s_isr_max_ns{0};

    inline double phase_at(uint32_t first, uint32_t i)
    {
        return static_cast<double>(s_phase[(first + i) & (TIME_DIAG_PHASE_SAMPLES - 1)]);
    }

    // Second difference x(i + 2m) - 2 x(i + m) + x(i), in ns
    inline double second_difference(uint32_t first, uint32_t i, uint32_t m)
    {
        return phase_at(first, i + 2 * m) - 2.0 * phase_at(first, i + m) + phase_at(first, i);
    }

    // Publishes the innovation statistics if the mutex is free; a reader holding it only delays this by a second
    void try_publish_innovation()
    {
        if (!s_publish_pending || s_mutex == nullptr || xSemaphoreTake(s_mutex, 0) != pdTRUE)
            return;
        s_published = s_pending;
        s_published_us = s_pending_us;
        xSemaphoreGive(s_mutex);
        s_publish_pending = false;
    }
}

void time_diag_init()
{
    if (s_mutex == nullptr)
        s_mutex = xSemaphoreCreateMutex();
}

void time_diag_add_phase(int64_t edge_sec, double measured_ns, double commanded_ns, double free_rate_ppb,
                         time_diag_source_t source, bool restart)
{
    const uint32_t head = s_phase_head.load(std::memory_order_relaxed);
    double value_ns = 0.0;
    if (restart || !s_have_series || source != s_source || edge_sec != s_last_sec + 1)
    {
        s_have_series = true;
        s_source = source;
        s_x0_ns = measured_ns;
        s_commanded_ns = 0.0;
        s_rate_ppb = free_rate_ppb;
        s_seconds = 0;
        s_series_source.store(static_cast<uint8_t>(source), std::memory_order_relaxed);
        s_series_start.store(head, std::memory_order_release);
    }
    else
    {
        // Phase of the free-running clock: the measurement without the phase the commands added
        s_commanded_ns += commanded_ns;
        ++s_seconds;
        value_ns = measured_ns - s_x0_ns - s_commanded_ns - s_rate_ppb * static_cast<double>(s_seconds);
    }
    s_last_sec = edge_sec;
    s_phase[head & (TIME_DIAG_PHASE_SAMPLES - 1)] = static_cast<float>(value_ns);
    s_phase_head.store(head + 1, std::memory_order_release);
}

void time_diag_add_innovation(int64_t edge_sec, double innovation_ns)
{
    const int64_t minute = edge_sec >= 0 ? edge_sec / 60 : (edge_sec - 59) / 60;
    if (minute != s_current_minute)
    {
        if (s_current_samples > 0)
        {
            // The minute is complete: store it, and the statistics of it and of the last hour are published
            s_minutes[s_minute_next] = {s_current_minute, s_current_sum_sq, s_current_samples};
            s_minute_next = (s_minute_next + 1) % HOUR_MINUTES;
            double hour_sum = 0.0;
            uint32_t hour_samples = 0;
            for (const minute_t &m : s_minutes)
            {
                if (m.samples > 0 && m.minute > s_current_minute - static_cast<int64_t>(HOUR_MINUTES))
                {
                    hour_sum += m.sum_sq_ns2;
                    hour_samples += m.samples;
                }
            }
            s_pending.rms_minute_ns = static_cast<float>(sqrt(s_current_sum_sq / s_current_samples));
            s_pending.minute_samples = s_current_samples;
            s_pending.rms_hour_ns = static_cast<float>(sqrt(hour_sum / hour_samples));
            s_pending.hour_samples = hour_samples;
            s_pending_us = esp_timer_get_time();
            s_publish_pending = true;
        }
        s_current_minute = minute;
        s_current_sum_sq = 0.0;
        s_current_samples = 0;
    }
    s_current_sum_sq += innovation_ns * innovation_ns;
    ++s_current_samples;
    try_publish_innovation();
}

void time_diag_add_isr_excess(double excess_ns)
{
    const uint32_t ns = excess_ns <= 0.0 ? 0u : (excess_ns >= 4.0e9 ? UINT32_MAX : static_cast<uint32_t>(excess_ns));
    size_t bin = 0;
    while (bin < TIME_DIAG_ISR_BINS - 1 && ns > ISR_BIN_LIMIT_NS[bin])
        ++bin;
    s_isr_bins[bin].fetch_add(1, std::memory_order_relaxed);
    s_isr_total.fetch_add(1, std::memory_order_relaxed);
    if (ns > s_isr_max_ns.load(std::memory_order_relaxed)) // the PPS task is the only writer
        s_isr_max_ns.store(ns, std::memory_order_relaxed);
}

bool time_diag_stability(time_diag_stability_t *out)
{
    if (out == nullptr)
        return false;
    *out = time_diag_stability_t{};

    // The current series, without the oldest slots the writer could reach meanwhile; read again if a new series
    // started while the indexes were read
    uint32_t head = 0;
    uint32_t start = 0;
    for (int attempt = 0;; ++attempt)
    {
        start = s_series_start.load(std::memory_order_acquire);
        head = s_phase_head.load(std::memory_order_acquire);
        if (s_series_start.load(std::memory_order_acquire) == start || attempt == 2)
            break;
    }
    uint32_t n = head - start;
    if (n > TIME_DIAG_PHASE_SAMPLES - PHASE_READ_MARGIN)
        n = TIME_DIAG_PHASE_SAMPLES - PHASE_READ_MARGIN;
    if (n < 3)
        return false;
    const uint32_t first = head - n;
    out->samples = n;
    out->source = static_cast<time_diag_source_t>(s_series_source.load(std::memory_order_relaxed));

    uint8_t count = 0;
    for (uint32_t m = 1; count < TIME_DIAG_TAUS && n >= 2 * m + 1; m *= 2)
    {
        // Overlapping Allan variance: sum of the squared second differences / (2 m^2 (n - 2m)), tau0 = 1 s
        const uint32_t terms = n - 2 * m;
        double sum_sq = 0.0;
        for (uint32_t i = 0; i < terms; ++i)
        {
            const double d = second_difference(first, i, m);
            sum_sq += d * d;
        }
        const double md = static_cast<double>(m);
        out->tau_s[count] = static_cast<uint16_t>(m);
        out->adev[count] = static_cast<float>(sqrt(sum_sq / (2.0 * md * md * terms)) * 1e-9);

        // Time variance (from the modified Allan variance): sums of m consecutive second differences,
        // squared, / (6 m^2 (n - 3m + 1)); the window sum slides along the series
        if (n >= 3 * m + 1)
        {
            const uint32_t windows = n - 3 * m + 1;
            double window_sum = 0.0;
            for (uint32_t i = 0; i < m; ++i)
                window_sum += second_difference(first, i, m);
            double sum_window_sq = window_sum * window_sum;
            for (uint32_t j = 1; j < windows; ++j)
            {
                window_sum += second_difference(first, j + m - 1, m) - second_difference(first, j - 1, m);
                sum_window_sq += window_sum * window_sum;
            }
            out->tdev_ns[count] = static_cast<float>(sqrt(sum_window_sq / (6.0 * md * md * windows)));
        }
        ++count;
    }
    out->count = count;

    // The writer reached the samples read meanwhile (only possible if this task was stopped for seconds)
    return s_phase_head.load(std::memory_order_acquire) - head < PHASE_READ_MARGIN;
}

void time_diag_innovation(time_diag_innovation_t *out)
{
    if (out == nullptr)
        return;
    *out = time_diag_innovation_t{0.0f, 0, 0.0f, 0, UINT32_MAX};
    if (s_mutex == nullptr || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return;
    *out = s_published;
    const int64_t published_us = s_published_us;
    xSemaphoreGive(s_mutex);
    if (published_us != 0)
        out->age_s = static_cast<uint32_t>((esp_timer_get_time() - published_us) / 1000000LL);
}

void time_diag_isr(time_diag_isr_t *out)
{
    if (out == nullptr)
        return;
    for (size_t i = 0; i < TIME_DIAG_ISR_BINS; ++i)
        out->bins[i] = s_isr_bins[i].load(std::memory_order_relaxed);
    out->total = s_isr_total.load(std::memory_order_relaxed);
    out->max_ns = s_isr_max_ns.load(std::memory_order_relaxed);
}
