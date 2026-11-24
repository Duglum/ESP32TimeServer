// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// See tx_delay_rule.h

#include "tx_delay_rule.h"

tx_delay_step_t tx_delay_rule_update(tx_delay_rule_t &r, int32_t sample_ns)
{
    if (r.samples < TX_DELAY_START_SAMPLES)
    {
        if (r.samples == 0 || sample_ns < r.estimate_ns)
            r.estimate_ns = sample_ns;
        ++r.samples;
        r.above_run = 0;
        r.below_run = 0;
        return tx_delay_step_t::start;
    }
    if (r.samples < UINT32_MAX)
        ++r.samples;
    const int64_t difference = static_cast<int64_t>(sample_ns) - r.estimate_ns;
    if (difference > TX_DELAY_BAND_UP_NS)
    {
        // a late sample does not end a run of early ones (it says nothing about the lower edge)
        if (r.above_run == 0 || sample_ns < r.above_min_ns)
            r.above_min_ns = sample_ns;
        if (++r.above_run < TX_DELAY_REBASE_UP_SAMPLES)
            return tx_delay_step_t::above;
        r.estimate_ns = r.above_min_ns;
        r.above_run = 0;
        return tx_delay_step_t::rebased_up;
    }
    if (difference < -TX_DELAY_BAND_DOWN_NS)
    {
        r.above_run = 0;
        if (r.below_run == 0 || sample_ns > r.below_max_ns)
            r.below_max_ns = sample_ns;
        if (++r.below_run < TX_DELAY_REBASE_DOWN_SAMPLES)
            return tx_delay_step_t::below;
        r.estimate_ns = r.below_max_ns;
        r.below_run = 0;
        return tx_delay_step_t::rebased_down;
    }
    r.above_run = 0;
    r.below_run = 0;
    r.estimate_ns = static_cast<int32_t>(r.estimate_ns + (difference >> TX_DELAY_EWMA_SHIFT));
    return tx_delay_step_t::fed;
}
