// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// See tx_delay_rule.h

#include "tx_delay_rule.h"

tx_delay_step_t tx_delay_rule_update(tx_delay_rule_t &r, int32_t sample_ns)
{
    if (r.samples < TX_DELAY_START_SAMPLES)
    {
        // keep the TX_DELAY_START_RANK smallest samples in ascending order (insertion, at most RANK moves)
        const uint32_t kept = r.samples < TX_DELAY_START_RANK ? r.samples : TX_DELAY_START_RANK;
        uint32_t at = kept;
        while (at > 0 && r.start_low_ns[at - 1] > sample_ns)
        {
            if (at < TX_DELAY_START_RANK)
                r.start_low_ns[at] = r.start_low_ns[at - 1];
            --at;
        }
        if (at < TX_DELAY_START_RANK)
            r.start_low_ns[at] = sample_ns;
        ++r.samples;
        // the RANK-th smallest so far (the largest while fewer were seen); after the last start sample the start value
        const uint32_t have = r.samples < TX_DELAY_START_RANK ? r.samples : TX_DELAY_START_RANK;
        r.estimate_ns = r.start_low_ns[have - 1];
        r.above_run = 0;
        r.below_run = 0;
        r.confirmed_by = 0;
        return tx_delay_step_t::start;
    }
    if (r.samples < UINT32_MAX)
        ++r.samples;
    const int64_t difference = static_cast<int64_t>(sample_ns) - r.estimate_ns;
    // a new estimate (the start value, a re-based one) is confirmed by TX_DELAY_CONFIRM_SAMPLES samples within the band
    const bool confirmed = r.confirmed_by >= TX_DELAY_CONFIRM_SAMPLES;
    if (difference > TX_DELAY_BAND_UP_NS)
    {
        // a late sample does not end a run of early ones (it says nothing about the lower edge)
        if (r.above_run == 0 || sample_ns < r.above_min_ns)
            r.above_min_ns = sample_ns;
        if (++r.above_run < TX_DELAY_REBASE_UP_SAMPLES)
            return tx_delay_step_t::above;
        r.estimate_ns = r.above_min_ns;
        r.above_run = 0;
        r.confirmed_by = 0;
        return tx_delay_step_t::rebased_up;
    }
    if (difference < -TX_DELAY_BAND_DOWN_NS)
    {
        r.above_run = 0;
        if (r.below_run == 0)
        {
            r.below_min_ns = sample_ns;
            r.below_second_ns = INT32_MAX;
        }
        else if (sample_ns < r.below_min_ns)
        {
            r.below_second_ns = r.below_min_ns;
            r.below_min_ns = sample_ns;
        }
        else if (sample_ns < r.below_second_ns)
            r.below_second_ns = sample_ns;
        if (++r.below_run < (confirmed ? TX_DELAY_REBASE_DOWN_SAMPLES : TX_DELAY_REBASE_DOWN_UNCONFIRMED))
            return tx_delay_step_t::below;
        r.estimate_ns = r.below_second_ns;
        r.below_run = 0;
        r.confirmed_by = 0;
        return tx_delay_step_t::rebased_down;
    }
    r.above_run = 0;
    // a sample within the band of an unconfirmed estimate does not end a run of early ones: after a start under a load
    // such samples are late ones near a start value that is too high
    if (confirmed)
        r.below_run = 0;
    else
        ++r.confirmed_by;
    r.estimate_ns = static_cast<int32_t>(r.estimate_ns + (difference >> TX_DELAY_EWMA_SHIFT));
    return tx_delay_step_t::fed;
}
