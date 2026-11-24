// ESP32 Time Server host tests: the estimate of the transmit delay of basic mode NTP answers (main/custom/tx_delay_rule.cpp)
#define TEST_NAME "tx_delay_rule_test"
#include "check.h"

#include "custom/tx_delay_rule.h"

#include <random>
#include <vector>

namespace
{
    // A quiet sample around base: -0.4 .. +3.8 us, mostly within +-0.3 us (the measured quiet spread)
    int32_t quiet(std::mt19937 &g, int32_t base)
    {
        const uint32_t x = g() % 1000;
        if (x < 10)
            return base + 3000 + static_cast<int32_t>(g() % 800); // the rare +3 .. +3.8 us
        return base - 400 + static_cast<int32_t>(g() % 700);
    }

    tx_delay_rule_t started(std::mt19937 &g, int32_t base)
    {
        tx_delay_rule_t r;
        for (uint32_t i = 0; i < TX_DELAY_START_SAMPLES; ++i)
            CHECK(tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::start);
        return r;
    }

    bool near(int32_t a, int32_t b, int32_t tolerance) { return a - b <= tolerance && b - a <= tolerance; }
} // namespace

void run_tests()
{
    std::mt19937 g(21);
    const int32_t base = 14000; // a quiet delay of the test board (about 13 to 21 us)

    // Quiet: the estimate stays at the quiet delay, every sample is fed (none out of the band)
    {
        tx_delay_rule_t r = started(g, base);
        CHECK(r.samples == TX_DELAY_START_SAMPLES);
        CHECK(near(r.estimate_ns, base - 400, 400)); // the smallest of the first samples
        int fed = 0;
        for (int i = 0; i < 5000; ++i)
            fed += tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::fed;
        CHECK(fed >= 4900); // the rare quiet samples above +2 us are not used
        CHECK(r.above_run < TX_DELAY_REBASE_UP_SAMPLES);
        CHECK(near(r.estimate_ns, base, 500));
    }
    // A burst of late samples (a handshake, a Save: up to 5 in a row measured at 8 answers per second): not used
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 200; ++i)
            tx_delay_rule_update(r, quiet(g, base));
        const int32_t before = r.estimate_ns;
        for (int burst = 0; burst < 50; ++burst)
        {
            for (int i = 0; i < 5; ++i)
                CHECK(tx_delay_rule_update(r, base + 20000 + static_cast<int32_t>(g() % 280000)) == tx_delay_step_t::above);
            for (int i = 0; i < 3; ++i)
                tx_delay_rule_update(r, quiet(g, base));
        }
        CHECK(near(r.estimate_ns, before, 300));
    }
    // A long run of late samples shorter than the re-base count, then quiet: unchanged, the quiet ones fed at once
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 200; ++i)
            tx_delay_rule_update(r, quiet(g, base));
        const int32_t before = r.estimate_ns;
        for (int i = 0; i < TX_DELAY_REBASE_UP_SAMPLES - 1; ++i)
            CHECK(tx_delay_rule_update(r, base + 30000 + static_cast<int32_t>(g() % 100000)) == tx_delay_step_t::above);
        CHECK(r.estimate_ns == before);
        CHECK(tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::fed);
        CHECK(r.above_run == 0);
        CHECK(near(r.estimate_ns, before, 300));
    }
    // A lasting step up (+40 us): re-based after TX_DELAY_REBASE_UP_SAMPLES samples on their smallest, then fed
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 200; ++i)
            tx_delay_rule_update(r, quiet(g, base));
        const int32_t up = base + 40000;
        int steps = 0;
        tx_delay_step_t s = tx_delay_step_t::above;
        while (s != tx_delay_step_t::rebased_up && steps < 1000)
        {
            s = tx_delay_rule_update(r, quiet(g, up));
            ++steps;
        }
        CHECK(steps == TX_DELAY_REBASE_UP_SAMPLES);
        CHECK(near(r.estimate_ns, up - 400, 400));
        for (int i = 0; i < 500; ++i)
            {
                const tx_delay_step_t st = tx_delay_rule_update(r, quiet(g, up));
                CHECK(st == tx_delay_step_t::fed || st == tx_delay_step_t::above); // above: the rare +3 us
            }
        CHECK(near(r.estimate_ns, up, 500));
    }
    // A lasting step up with disturbances among the samples: the lower edge, not a disturbed value
    {
        tx_delay_rule_t r = started(g, base);
        const int32_t up = base + 40000;
        int steps = 0;
        tx_delay_step_t s = tx_delay_step_t::above;
        while (s != tx_delay_step_t::rebased_up && steps < 1000)
        {
            s = tx_delay_rule_update(r, (steps % 3 == 0) ? up + 100000 : quiet(g, up));
            ++steps;
        }
        CHECK(steps == TX_DELAY_REBASE_UP_SAMPLES);
        CHECK(near(r.estimate_ns, up - 400, 400));
    }
    // A lasting step down (-10 us): re-based after TX_DELAY_REBASE_DOWN_SAMPLES samples on their largest
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 200; ++i)
            tx_delay_rule_update(r, quiet(g, base));
        const int32_t down = base - 10000;
        for (int i = 0; i < TX_DELAY_REBASE_DOWN_SAMPLES - 1; ++i)
            CHECK(tx_delay_rule_update(r, down - 400 + static_cast<int32_t>(g() % 700)) == tx_delay_step_t::below);
        CHECK(tx_delay_rule_update(r, down) == tx_delay_step_t::rebased_down);
        CHECK(near(r.estimate_ns, down, 400));
        for (int i = 0; i < 500; ++i)
            {
                const tx_delay_step_t st = tx_delay_rule_update(r, quiet(g, down));
                CHECK(st == tx_delay_step_t::fed || st == tx_delay_step_t::above); // above: the rare +3 us
            }
        CHECK(near(r.estimate_ns, down, 500));
    }
    // A long mixed load: quiet samples among late ones spread evenly over 0 .. 50 us above (the case of a band that
    // moves with the estimate); the estimate must not walk up with the load
    for (const int quiet_percent : {50, 20, 5})
    {
        tx_delay_rule_t r = started(g, base);
        int32_t highest = r.estimate_ns;
        for (int i = 0; i < 20000; ++i)
        {
            const int32_t s = static_cast<int>(g() % 100) < quiet_percent ? quiet(g, base) : base + static_cast<int32_t>(g() % 50001);
            tx_delay_rule_update(r, s);
            if (r.estimate_ns > highest)
                highest = r.estimate_ns;
        }
        CHECK(highest <= base + (quiet_percent >= 20 ? 1000 : 4000));
        // after the load: at most TX_DELAY_REBASE_DOWN_SAMPLES quiet samples until the estimate is quiet again
        int n = 0;
        while (r.estimate_ns > base + 500 && n < 100)
        {
            tx_delay_rule_update(r, quiet(g, base));
            ++n;
        }
        CHECK(n <= TX_DELAY_REBASE_DOWN_SAMPLES);
    }
    // Early samples among late ones: the late ones do not end the run of early ones
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 100; ++i)
            tx_delay_rule_update(r, quiet(g, base + 10000)); // too high by the steps below: the estimate at +10 us
        r.estimate_ns = base + 10000;
        for (int i = 0; i < TX_DELAY_REBASE_DOWN_SAMPLES - 1; ++i)
        {
            CHECK(tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::below);
            CHECK(tx_delay_rule_update(r, base + 40000) == tx_delay_step_t::above);
        }
        CHECK(tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::rebased_down);
    }
    // A single spurious low sample (and three): no change
    {
        tx_delay_rule_t r = started(g, base);
        for (int i = 0; i < 200; ++i)
            tx_delay_rule_update(r, quiet(g, base));
        const int32_t before = r.estimate_ns;
        for (int k = 1; k < TX_DELAY_REBASE_DOWN_SAMPLES; ++k)
        {
            for (int i = 0; i < k; ++i)
                CHECK(tx_delay_rule_update(r, 0) == tx_delay_step_t::below);
            CHECK(tx_delay_rule_update(r, quiet(g, base)) == tx_delay_step_t::fed);
            CHECK(near(r.estimate_ns, before, 300));
        }
    }
    // A start under load: the first samples mostly late; the smallest is taken, the answers after the load are right
    {
        tx_delay_rule_t r;
        for (uint32_t i = 0; i < TX_DELAY_START_SAMPLES; ++i)
            tx_delay_rule_update(r, (i % 4 == 3) ? quiet(g, base) : base + 20000 + static_cast<int32_t>(g() % 100000));
        CHECK(near(r.estimate_ns, base - 400, 400));
        // a start where every first sample was late: too high, brought down by the first quiet samples
        tx_delay_rule_t h;
        for (uint32_t i = 0; i < TX_DELAY_START_SAMPLES; ++i)
            tx_delay_rule_update(h, base + 30000 + static_cast<int32_t>(g() % 50000));
        CHECK(h.estimate_ns >= base + 30000);
        int n = 0;
        while (h.estimate_ns > base + 4000 && n < 100)
        {
            tx_delay_rule_update(h, quiet(g, base));
            ++n;
        }
        CHECK(n == TX_DELAY_REBASE_DOWN_SAMPLES);
    }
    // Limits: 0, 500 us, values above, a long run; no overflow, the counters do not wrap
    {
        tx_delay_rule_t r;
        for (uint32_t i = 0; i < TX_DELAY_START_SAMPLES; ++i)
            tx_delay_rule_update(r, 0);
        CHECK(r.estimate_ns == 0);
        for (int i = 0; i < TX_DELAY_REBASE_UP_SAMPLES; ++i)
            tx_delay_rule_update(r, 500000);
        CHECK(r.estimate_ns == 500000);
        for (int i = 0; i < TX_DELAY_REBASE_DOWN_SAMPLES; ++i)
            tx_delay_rule_update(r, 0);
        CHECK(r.estimate_ns == 0);
        tx_delay_rule_t x;
        x.samples = UINT32_MAX - 2;
        x.estimate_ns = 10000;
        for (int i = 0; i < 10; ++i)
            tx_delay_rule_update(x, 10000);
        CHECK(x.samples == UINT32_MAX);
        CHECK(x.estimate_ns == 10000);
        tx_delay_rule_t m;
        m.samples = TX_DELAY_START_SAMPLES;
        m.estimate_ns = INT32_MAX - 10;
        CHECK(tx_delay_rule_update(m, 0) == tx_delay_step_t::below);
        m.estimate_ns = INT32_MIN + 10;
        CHECK(tx_delay_rule_update(m, 500000) == tx_delay_step_t::above);
        // 100000 random samples of 0 .. 500 us: the estimate stays within the range of the samples
        tx_delay_rule_t z;
        for (int i = 0; i < 100000; ++i)
        {
            tx_delay_rule_update(z, static_cast<int32_t>(g() % 500001));
            CHECK(z.estimate_ns >= 0 && z.estimate_ns <= 500000);
            CHECK(z.above_run < TX_DELAY_REBASE_UP_SAMPLES && z.below_run < TX_DELAY_REBASE_DOWN_SAMPLES);
        }
    }
}
