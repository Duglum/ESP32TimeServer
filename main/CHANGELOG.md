# ESP32 Time Server: combined changelog and technical notes

This file combines `Sonnet_CHANGELOG.md` (precision and stability work done earlier) and `Review_2026-10-19_CHANGELOG.md` (review of 2026-10-19 and its follow-ups). Where the later review changed or replaced something from the earlier work, only the current state is described.

State described here: the files in the repository on 2026-10-19 after the review, including the stack sizes set by the user afterwards (section 4.15), and the follow-up of the same evening: fine local timebase and free-running, frequency disciplined PTP clock with optional PPS loopback (sections 4.19 to 4.21, results in 7.6), of 2026-10-20: PTP clock from the 80 MHz PLL, root dispersion 1 (section 4.22), and of 2026-10-21: HTTPS web admin page (section 4.23).

Contents

1. Summary
2. Starting point and what was wrong
3. Architecture
4. Changes in detail
5. Security and open ports
6. Review of the original clock correction code
7. Measured and simulated results
8. Settings reference
9. Data stored in NVS
10. Console messages
11. How the changes were verified
12. Known limitations and open points
13. Recommendations that are not in the code
14. Files and how to build

---

## 1. Summary

| # | Change | Effect |
|---|---|---|
| 1 | PPS capture: real tick rate of the MCPWM timer is queried, hardware ticks are used for frequency and for the edge time | no dependency on a hard-coded 80 MHz, no ISR jitter in the clock |
| 2 | Frequency estimate over a sliding window of 16 s (was: 128 s window with 10 % IIR gain, about 1 h to converge) | frequency known after 4 s, ±4 ppb |
| 3 | Frequency compensation applied to the time stamps themselves (model clock) instead of a step at the PPS | error between the pulses 0.3 µs RMS instead of 5 to 21 µs RMS |
| 4 | PPS spike rejection | a spurious pulse can no longer move the clock |
| 5 | Edge anchor from the central value of the interrupt latency instead of a minimum | systematic anchor error below 0.1 µs instead of -0.35 to +0.37 µs |
| 6 | PTP clock set from the PPS second every second, with write latency compensation, read-back and retry (now only with `PTP_FREE_RUNNING 0`, see #22) | T2 and hardware T3 are in the same time scale as the model clock |
| 7 | **PTP offset measured after every write** (mean of 16 bracketed read-backs) and removed from T2 / hardware T3 (only with `PTP_FREE_RUNNING 0`) | write error 0.05 µs RMS instead of 0.5 µs RMS (simulation) |
| 8 | PTP write latency measured at run time and stored in NVS (only with `PTP_FREE_RUNNING 0`) | no hard-coded board dependent value; with #7 no longer critical for accuracy |
| 9 | Interrupt latency of the PPS path measured with a software triggered capture and stored in NVS; test triggers keep away from real PPS edges | no oscilloscope needed, no wrong samples |
| 10 | Hardware TX time stamps for the interleaved mode | T3 of interleaved responses is the real transmit time |
| 11 | **Transmit delay compensation for basic mode T3** (running mean of hardware TX time - software T3) | basic clients (Windows, ntpd, phones) no longer see half the transmit delay as offset |
| 12 | Interleaved mode fixes: cache by client IP, lifetime 30 minutes, correct T3 caching | chrony runs in interleaved mode on IPv4 and IPv6 |
| 13 | Precision -24 (was -20), root dispersion 1 (was 2) | chrony sees the real delay and selects the server; error bound 15 µs smaller |
| 14 | **No clock step at the periodic GNSS check** when the clock already shows the right second; no stratum 16 window during the check | no disturbance every 10 minutes |
| 15 | **Robust second label** (label data only from after the PPS edge, confirmation of a 1 s difference, consistency check at the first sync) | no risk of a 1 s error from an old NMEA sentence or an early UBX poll |
| 16 | **Raw NTP path only answers well-formed unicast requests to the own address**; everything else goes to lwIP | no replies with broadcast, multicast or foreign source addresses |
| 17 | Stability fixes: seqlock fences, no lock/string work per NTP frame in the RX task, MQTT JSON overflow, `ntp_auth.cpp` status handling, LED/LCD status codes | see 4.13 |
| 18 | Unused code removed | see 4.14 |
| 19 | Compiler optimization -O2 instead of -Og, with the build fixes it needs | shorter and steadier NTP and PPS code paths |
| 20 | Logging convention: brief lines always, details with `DEBUG_ENABLED`; secret keys only with `DEBUG_ENABLED` | quiet console in normal operation |
| 21 | **Fine local timebase**: the raw 16 MHz systimer counter instead of the 1 µs `esp_timer_get_time()` for all time critical readings | 62.5 ns resolution instead of 1 µs (0.29 µs RMS rounding noise removed from software T3, PPS anchor, PTP measurements, interrupt latency test) |
| 22 | **Free-running PTP clock**: set once, then only its frequency is steered (EMAC fine addend); offset and rate are estimated once per second and the hardware time stamps are converted with a continuous mapping | no per-second clock write: the rare errors of up to 33 µs for frames that straddle a write are gone, no write latency in the result, holdover on the learned rate |
| 23 | **Optional PTP PPS loopback** (`PTPPPSLoopbackPin`): the EMAC PPS output is captured by the same MCPWM timer as the GNSS PPS | PTP clock measured against the PPS in hardware: the constant read delay error of the software measurement (some 100 ns) is removed; simulated error of T2 / hardware T3 16 ns RMS |
| 24 | **PTP clock from the 80 MHz PLL** instead of the 40 MHz crystal (2026-10-20) | time stamp steps 13 ns instead of 40 ns |
| 25 | **Web admin page** (HTTPS, TCP 443, 2026-10-21): network, GNSS / PPS, LCD, time zone, LED, MQTT and NTP authentication settings; Apply / Save (NVS) / Factory reset; LCD, LED, MQTT and NTP authentication always compiled in and switched at run time | common settings without rebuilding; per-device TLS key, salted password hash, sessions with CSRF protection, allow-list |

No memory leak was found in the review (section 4.13).

## 2. Starting point and what was wrong

The server takes the second from a GNSS module and a PPS pulse that is captured by the MCPWM hardware of the ESP32-P4. Between two pulses the clock runs on the 40 MHz crystal.

Found in the original code:

- The PPS tick capture was used correctly, but only for the interval. The phase used the esp_timer reading of the interrupt: 1 µs resolution, unfiltered interrupt jitter and no latency compensation, with gain 1 straight into the clock.
- The frequency correction was applied as an `adjtime` step at every PPS (section 6). It could not reduce the error between the pulses.
- The PTP clock (source of T2) was set right after `adjtime()` from the system clock. `adjtime` acts delayed, so the PTP clock got the uncorrected time. The `clock_settime` latency was not compensated.
- T2 came from the PTP clock and T3 from `gettimeofday()`: two clocks with different corrections. A `gettimeofday()` call costs 9.2 µs (measured).
- The tick rate was hard-coded to 80 MHz, the PPS task had no protection against spurious pulses.
- Interleaved mode never worked because the client cache was keyed by IP and source port (chrony uses a new port for every request) and the cache kept a wrong T3.
- `Precision -13` and `Root dispersion 1.007 ms` kept chrony from seeing the real network delay and from preferring the server.

Found in the review of 2026-10-19 (after the earlier work):

- Every 10 minutes the periodic GNSS check stepped the system clock, invalidated the model clock and rewrote the PTP clock from the system clock; for up to 1 s T2/T3 had errors of up to about 19 µs. During each check (1.5 to 3 s) all responses were stratum 16.
- Each per-second PTP write had its own error (write latency jitter, 1 µs quantization of esp_timer), about 0.5 µs RMS.
- Basic mode T3 was the clock reading before the transmit call; the delay to the real transmission was never compensated.
- The second label could be one second wrong (old RMC sentence in the UART buffer, UBX poll right at the edge); the ±1 s sanity window accepted that.
- The raw NTP path answered any UDP/123 frame, including broadcast/multicast, foreign destinations, fragments, bad checksums and invalid sources.
- Seqlocks without fences, a mutex and string work per NTP frame in the RX task, an MQTT buffer overflow, a status handling bug in `ntp_auth.cpp`, inconsistent LED/LCD status codes.

Found in the follow-up (2026-10-19 evening):

- Every software time reading still had the 1 µs truncation of `esp_timer_get_time()`, although the systimer behind it counts at 16 MHz.
- With the per-second PTP write, a frame that is time stamped shortly before the write but converted after it (or while the compensation is switched off during the write) gets an error of up to the drift of one second: about 33 µs with the -33 ppm crystal of the test board. The window is some 10 to 100 µs per second, so about 1 in 10000 frames.
- The PTP offset is measured by software readings bracketed by local time readings. Where the register read falls inside the bracket is not known; this gives a constant error of some 100 ns in T2 and hardware T3 (old and new software measurement alike). Only a hardware comparison can remove it.

## 3. Architecture

```
 PPS pin -> MCPWM capture (80 MHz hardware tick)  --ISR-->  pps_capture_callback
                                                            (tick value + esp_timer reading, queue)
                                                                  |
                                                                  v
 EMAC PTP PPS output -> PTPPPSLoopbackPin -> MCPWM capture, same timer   --ISR-->  ptp_pps_capture_callback
 (optional, nothing to wire)                                                       (tick value, ring of 4)

 pps_discipline_task (once per second)
   0 spike rejection        interval to the previous edge vs. expected interval (±50 us)
   1 tick bookkeeping       unwrapped 32 bit ticks, seconds counter
   2 frequency (ppb)        sliding window of 16 s, from ticks only
   3 edge anchor            tick time + central value of (ISR time - tick time) - central interrupt latency
                            (ISR time on the fine local timebase, 62.5 ns)
   4 phase of system clock  adjtime(-error), bracketed gettimeofday reads
   5 model clock publish    {second of the edge, edge in local ns, ppb}  (seqlock)
   6 PTP servo              PTP_FREE_RUNNING 1 (default):
                              measurement m = PTP - true at the edge: loopback capture (hardware) or
                              bracketed software readings (median of the shortest brackets)
                              alpha-beta filter -> offset and free-running rate of the PTP clock
                              frequency command = -rate - offset / 16 s  (EMAC fine addend, absolute)
                              mapping {reference, offset, rate before / after the update}  (seqlock)
                            PTP_FREE_RUNNING 0: clock_settime every second + offset measurement (4.8)
                                                                  |
        +---------------------------------------------------------+---------------------+
        v                                                                                v
 NTP responses (raw path, hardware time stamps)                                  other consumers
   T2  = EMAC RX time stamp ts, true = ts - offset(ts)                           system clock (gettimeofday)
         offset(ts) = ref_offset + rate * (ts - ref)   (rate before / after the last update)
   T3  = model clock = second + (local - edge) - ppb * (local - edge)
         + running mean of the transmit delay (basic mode)
   T3 (interleaved) = hardware TX time stamp of the previous response, cached per client IP

 gnss_time_sync_task (first sync, then every 10 minutes)
   PPS edge -> label from data received after the edge (NMEA: buffer emptied at the edge; UBX: poll 300 ms after)
   first sync: two readings must advance by exactly the number of seconds between their edges -> settimeofday
   later: clock right -> nothing is stepped; 1 s difference -> confirmed by a second reading, then stepped
```

Time scales: the model clock is aligned to the PPS edge every second and compensated with the measured frequency error. The PTP clock is disciplined to the PPS by the servo; its remaining offset is removed by the mapping, so T2 and hardware T3 are in true time. The basic mode T3 compensation (4.11) learns the difference of the two time scales together with the transmit delay. The system clock (`gettimeofday`) is only kept at the right second and is not used for NTP time stamps of the raw path.

## 4. Changes in detail

### 4.1 PPS capture and tick bookkeeping

- `setup_pps_input()` asks the driver for the real tick rate (`mcpwm_capture_timer_get_resolution`) and stores it in `s_pps_capture_hz`.
- Intervals are calculated modulo 2^32 (the 32 bit counter wraps every 53.7 s at 80 MHz) and added to a 64 bit `unwrapped` counter, together with a seconds counter `seq`. A missed pulse (2 s or 3 s interval) is accepted as such. An interval outside that range restarts the estimators.
- After 3.5 s without a pulse the PPS counts as lost: the estimators restart, the last frequency value is kept as holdover value.

### 4.2 Frequency estimate

- Window: up to 17 edge samples (16 s). Estimate = (ticks of the newest minus ticks of the oldest sample) / (seconds × tick rate) - 1, in ppb. First estimate after 4 s, accepted only within ±200 ppm.
- Sign: positive ppb = the oscillator and therefore the local clock run too fast.
- Precision: 12.5 ns tick / 16 s is below 1 ppb; the noise is dominated by the PPS jitter of the GNSS module (a few ppb).

### 4.3 PPS spike rejection

- Once a frequency estimate exists, the interval to the previous edge can be predicted to a few hundred ns: `expected = seconds × rate × (1 + ppm)`.
- An edge whose interval differs by more than 50 µs (`spikeLimitUs`), or that follows another edge within half a second, is ignored completely: no estimator, no system clock correction, no model update, no sync state.
- After 3 ignored edges in a row (`maxSpikes`) the edges are trusted again and the estimators start over. That covers a real step of the PPS.
- Before, an edge anywhere within ±250 ms of a second was trusted; a glitch could have moved the clock by up to 250 ms for one second.

### 4.4 Edge anchor from the central value

- For every edge `d = ISR time - tick time` is calculated. It contains a constant offset between the tick counter and the local timebase, the interrupt latency and the resolution of the local timebase (62.5 ns since 4.19, 1 µs before).
- The anchor uses the central value of the last 64 values: the mean of the lowest 75 %. `edge_in_esp_time = edge ISR time - (d - central value) - central interrupt latency`. The interrupt jitter of the individual edge cancels out completely (the anchor is effectively tick time + central offset).
- The interrupt latency constant is measured with the same statistic and the same timebase (4.5), so the resolution and the shape of the jitter cancel out. A minimum (used before) has a bias that depends on the noise, up to ±0.35 µs.

### 4.5 Interrupt latency of the PPS path (measured, stored in NVS)

- `PPS_ISR_LATENCY_AUTO 1`: at the first start for a configuration a task measures the latency 30 s after the boot: 160 software triggered capture events (`mcpwm_capture_channel_trigger_soft_catch`), 500 ms apart, on the other core than the PPS interrupt. Per event the latency is the local time reading in the callback minus the middle of two local time readings around the trigger (fine timebase, 4.19). Attempts with a gap above 5 µs between those readings are dropped, at least 32 valid attempts are needed.
- New in the review: test triggers are only issued between 50 ms and 950 ms after the last real PPS edge (the callback stores the low 32 bits of its time). Before, a real edge that arrived while a test was armed was taken as the test event (wrong sample) and was lost for the PPS task.
- Result = mean of the lowest 75 % of the samples ("central value"), accepted within 0.5 to 20 µs. It is applied immediately and stored in NVS with a fingerprint: IDF version, CPU frequency, MCPWM ISR setting (`CONFIG_MCPWM_ISR_IRAM_SAFE` or its IDF 6.1 name `CONFIG_MCPWM_ISR_CACHE_SAFE`), **compiler optimization level**, chip model, revision and cores, `PPS_ISR_LATENCY_CODE_VERSION`. With a matching fingerprint the stored value is read at every start and no measurement runs.
- `PPS_ISR_LATENCY_CODE_VERSION` is 4 (3 with `FINE_LOCAL_TIMEBASE 0`): the value stored with version 3 was measured in whole microseconds, so it is measured again once (about 2 minutes after the first start). Default until a value is measured: 2.6 µs.
- Resolution 62.5 ns (was ±0.5 µs with esp_timer). Measurement on the development board with esp_timer: minimum 2.50 µs, median 2.50 µs, mean 2.82 µs, p90 3.50 µs.
- The test events are not passed on as PPS pulses.

### 4.6 Phase correction of the system clock

- The system clock is kept at the right second with `adjtime()` once per second. The phase error is taken from the edge anchor; `gettimeofday()` is read four times bracketed by two esp_timer readings and the reading with the smallest gap is used.
- Corrections of at least 1 µs are applied, the remainder is carried to the next second. Errors above 250 ms are not corrected by `adjtime`.
- The system clock is not drift compensated between the pulses (sawtooth of about 19 µs). It is not used for the NTP time stamps of the raw path.

### 4.7 Model clock (software T3, reference time)

- At every PPS the task publishes `{second of the edge, edge in local ns, ppb}` (seqlock, single writer). Time = `second + elapsed - elapsed × ppb / 1e9`, elapsed from the fine local timebase (4.19).
- `get_current_time_in_ntp64_format()` uses the model. If the model is not valid (start, after a clock step) it falls back to the system clock with the same compensation. After one hour without a PPS edge the model is no longer used.
- Resolution: 62.5 ns (systimer counter, 4.19); before: esp_timer microseconds (rounding noise 0.29 µs RMS).

### 4.8 PTP clock written every second (`PTP_FREE_RUNNING 0`, previous mode)

This is the mode before the follow-up; it is kept as an option. The default is the free-running PTP clock (4.20).

- `set_hardware_clock_from_pps()` writes the PTP clock every second: `PPS second + (esp_timer reading - edge in esp_timer time) + write latency`. The PTP clock is not set from the system clock in normal operation.
- Read-back: the PTP clock is read bracketed by two esp_timer readings. A deviation beyond 2 µs from the expected write latency causes a rewrite (at most 3 attempts). The warning `PTP set retry ...` is only printed with `DEBUG_ENABLED`.
- **Offset measurement (review, `PTP_OFFSET_MEASUREMENT 1`)**: directly after the write the PTP clock is read 16 times, each read bracketed by two esp_timer readings (brackets above 3 µs are dropped, at least 4 reads are needed). Offset = mean(PTP reading - model time at the middle of the bracket). Offsets beyond ±50 µs are not trusted (0 is used). The offset and the second of the edge are published together (seqlock).
- **Conversion of a hardware time stamp (replaces the earlier `ts - ppb × (ts - value written)`):** `el = ts - offset - second_of_edge`, `true = second_of_edge + el - el × ppb / 1e9`. This is the formula of the model clock, so T2, hardware T3 and software T3 are in the same time scale. The same ppb as for the model clock is used (the PTP clock runs from the same crystal).
- While the PTP clock is being written its compensation is switched off for a few microseconds. A write by `synchronize_hardware_clock()` (clock step, Ethernet restart) also switches it off until the next PPS.
- Systematic limit: the read is assumed in the middle of the bracket (the write latency calibration makes the same assumption).

### 4.9 PTP write latency: calibrated at run time (`PTP_FREE_RUNNING 0` only)

- With `PTP_FREE_RUNNING 1` the clock is not written every second, so there are no samples: the calibration is switched off (`PTP_LATENCY_AUTO` is forced to 0), and the rare clock steps of the servo (offset above 100 µs) use the default 3850 ns; the servo removes the rest within seconds.
- The write latency is the time between the esp_timer reading before `clock_settime(CLOCK_PTP_SYSTEM)` and the moment the EMAC latches the new time. Default 3850 ns (measured on the development board).
- `PTP_LATENCY_AUTO 1`: if no value for this configuration is stored, a task measures it during normal operation: 10 minutes warm-up after the PPS lock, then blocks of 10 minutes (600 samples, one per second, only the first write of each second). Per block the trimmed mean (central 80 %) is calculated. After 3 blocks the result is accepted if the last three block means agree within 250 ns, otherwise a 4th block is added. After 4 blocks without agreement the calibration is given up (default stays, tried again at the next start). A PPS loss restarts the run. Usual run 40 minutes, longest 50 minutes.
- The result is applied immediately and stored in NVS (namespace `ptp_latency`) with a fingerprint (IDF version, CPU frequency, **compiler optimization level**, chip, `PTP_LATENCY_CODE_VERSION`). At every later start it is read and no calibration runs.
- Since the offset measurement (4.8) removes the actual error of every write, the calibrated latency only keeps the residual small; a wrong value no longer affects the time stamps. If the buffer task cannot be started its buffer is freed (review fix).

### 4.10 Hardware TX time stamps (interleaved mode)

- `transmit_with_hw_timestamp()` sends the frame with `esp_eth_transmit_ctrl_vargs()` and an `eth_mac_time_t` control structure (56 bytes of reserve behind it). The returned time stamp is checked (between 2 ms in the future and 100 ms in the past compared with the PTP clock) and converted with the PTP compensation.
- On errors (other than "out of memory", which the caller retries) the frame is sent without a time stamp. After 5 errors in a row hardware time stamps are switched off (warning once). `NTP_HW_TX_TIMESTAMPS 0` switches the feature off at compile time.
- Used in both raw paths (IPv4, IPv6). The socket path cannot get TX time stamps.

### 4.11 Transmit delay compensation for basic mode T3 (review)

- In basic mode T3 has to be in the packet before it is sent; it is read from the model clock just before the transmit call. The real transmit instant is later (rest of the packet, MAC with authentication, EMAC DMA).
- `NTP_BASIC_T3_TX_COMPENSATION 1`: per response the sample `hardware TX time stamp - software T3` (0 to 500 µs accepted) updates a running mean: plain mean for the first 32 samples, then weight 1/32. Compensation starts after 16 samples. Four estimates: IPv4/IPv6 × without/with authentication.
- The compensated value is written into basic responses and is also cached when no hardware TX time stamp exists. Interleaved responses are not affected.
- No samples (hardware TX time stamps off) = no compensation.

### 4.12 Interleaved mode and client cache

- Cache key: client IP only (`NTP_CACHE_MATCH_PORT 0` in `ntp_cache.c`). chrony uses a new source port for every request. Correctness is still protected by the comparison `origin timestamp == stored T2`.
- Lifetime 30 minutes (chrony polls up to 1024 s). 128 clients per address family.
- After a response the cache stores the transmit time of this response: the hardware TX time stamp, otherwise the (compensated) software time. Before, the content of field 40 was stored, which is the previous T3 in an interleaved response, so the cache returned a stale T3 from the third exchange on.
- Interleaved response: origin = receive timestamp of the request, T3 = cached hardware TX time of the previous response.

### 4.13 NTP header fields

- `NTP_PRECISION_EXPONENT = -24` (60 ns, the resolution of the fine local timebase, 4.19; -20 = 0.95 µs with `FINE_LOCAL_TIMEBASE 0`; originally -13 = 122 µs).
- `NTP_ROOT_DISPERSION = 1` (1/65536 s = 15.3 µs, the smallest non-zero value of the field; 2 = 30.5 µs until 2026-10-20, originally 66 = 1.007 ms). The server only reports stratum 1 with active PPS. The field only enters the error bound of the clients (root distance, chrony's `+/-`), not the time.

### 4.14 GNSS time synchronisation (review)

`acquire_sync_candidate()`: the candidate is the UTC second of a PPS edge E (`pps_release_time_us` = E). The label data must be received after E and be complete within 950 ms after E:

- NMEA (the setting in use, `UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED 0`): the UART receive buffer is emptied right after E, then the first valid RMC sentence is used.
- UBX: the PVT is polled 300 ms after E (poll timeout 600 ms). A poll right at the edge can still return the solution of the previous second.

`gnss_time_sync_task()`:

- First synchronisation: two readings; `first_sync_candidates_are_plausible()` requires that the labels advance by exactly the number of seconds between their PPS edges. Then `settimeofday()`.
- Periodic check (`periodicGNSSRefreshEveryThisNumberOfMinutes`): the offset of the system clock at the edge is calculated in whole seconds (`system_clock_second_offset()`).
  - 0 with running PPS discipline and valid model clock: success is recorded, nothing is stepped (before: `settimeofday()`, model and PTP compensation invalid for up to 1 s every 10 minutes).
  - ±1 (within `safeguardThresholdInSeconds`): the next reading must show the same difference before the clock is stepped.
  - Outside the threshold: sanity failure handling as before (reboot if `rebootIfSanityCheckFails`).
- `s_time_setting_in_progress` (stratum 16, LCD `[*]`) is only set during the first synchronisation and while the clock is really stepped (before: during every periodic check).

### 4.15 Stability, memory, task stacks (review)

- Memory leaks: none found (checked: every path of `ntp_ethernet_input`, request buffer pool, one-shot tasks, MQTT/TF queue, NTP cache, LCD).
- `ntp_ethernet_input` (EMAC RX task, up to 14500 frames per second): no mutex, no address string copies and no `inet_pton` per NTP frame any more (only needed before "open for business"; requests of the device to itself never arrive through the EMAC).
- Seqlock fences added for the NTP sync state; the PTP compensation has its own seqlock.
- MQTT report: `len += snprintf(...)` could move past the end of the buffer and corrupt memory on long reports; replaced by `json_append()`. A queued TF-card file larger than a report is deleted instead of being read into the heap.
- `ntp_auth.cpp`: each PSA hash step is checked (a failed setup/update was stored as `PSA_SUCCESS`); a log line with missing printf arguments was removed.
- LED/LCD: "time being set" returns 98 (was 99 = button), LED colours follow the codes of `get_required_top_line_message()`, `[I]` = no usable IP address, `[T]` = transport stalled. The status table in the code was updated.
- `#include <cinttypes>` added (`PRIu32` was used without it).
- -O2 build: `format_local_date_time()` and the LCD lines "Edges .. of .." / "Dwelling for .. secs" limit their values to the field widths so GCC can prove the `snprintf` output fits.
- Task stacks (bytes) as set by the user after the review (Ethernet transport recovery 2405, hardware NTP server 2719 and PPS discipline 3135 were changed):

| Task | Stack |
|---|---|
| Ethernet transport recovery | 2405 |
| GNSS recovery | 12288 |
| GNSS time sync | 2412 |
| Hardware NTP server | 2719 |
| LED / LCD / button | 3217 |
| Ethernet setup | 4096 |
| MQTT service | 3512 |
| NTP cache purge | 2222 |
| NTP server (socket path) | 3560 |
| OTE service | 3560 |
| PPS discipline | 3584 (follow-up: was 3135; the PTP servo runs in this task) |

Check the values with `CALCULATE_STACK_SIZES_ENABLED` after every change of a task. The PPS discipline value of the follow-up was checked by the user with `CALCULATE_STACK_SIZES_ENABLED` (well within the limit).

### 4.16 Unused code removed (review)

| Item | Why |
|---|---|
| `s_safe_guard_tripped` | only written (the LCD shows the sanity fault via `faults.sanity_mismatch`) |
| `sync_state_t::holdover_mode`, `sync_state_t::last_pps_seen_us` | only written |
| `reduceGNSSUART1OutputToTimeMessages`, `saveReducedGNSSUART1OutputPermanently`, `configure_gnss_time_only_uart1_output()` and its call | commented-out pre-release code |
| stopwatch block in `gnss_time_sync_task`, heartbeat block in `ote_service_task` | commented-out code |
| temporary diagnostics of the earlier work (PTP/system clock comparison, `CALLCOST`, interleaving log) | not in the code |

Everything that can be switched on in the settings (MQTT, LCD, LEDs, button, OTE, authentication, health test, stack size calculation) is kept.

### 4.17 Compiler optimization and build (review follow-up)

- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (-O2) instead of `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` (-Og), set in `sdkconfig` and in `sdkconfig.defaults` (`sdkconfig` is in `.gitignore`; the defaults file is the tracked source).
- ESP-IDF compiles everything with `-Werror=all` (`CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` only turns `-Werror` into `-Werror=all`), and -O2 runs flow analyses that -Og does not. The root `CMakeLists.txt` keeps these warnings (`maybe-uninitialized`, `uninitialized`, `stringop-*`, `format-truncation`, `format-overflow`, `array-bounds`, `restrict`, `dangling-pointer`, `use-after-free`) as warnings for the components in the project's `managed_components` and `components` folders (third-party code such as esp-modbus and SparkFun_u-blox_GNSS_v3). `main` and the ESP-IDF components keep the strict setting. `managed_components` and the `3rdparty` submodule are not edited.
- Because the optimization level is part of both calibration fingerprints, the interrupt latency (about 2 minutes) and the PTP write latency (about 50 minutes) are measured again once.

### 4.18 Logging

- Brief lines always, details only with `DEBUG_ENABLED`. Lines for stored values and calibration results are always printed. The `PTP set retry` warning only with debug.
- Follow-up: the per-16-second debug line `PTP: offset after write ...` is replaced by the servo status (`PTP: loopback|software offset ... rate ... cmd ...`, plus `PTP loopback: used ... missing ... rejected ... | software - loopback ...` when the loopback is active); the basic mode TX delay has its own line `Basic T3 TX delay: v4 ... v6 ...`.
- The secret NTP keys (symmetric-key authentication) are only printed with `DEBUG_ENABLED` (review). chrony accepts the ASCII keys of `ESP32TimeServerSettingsSecrets.h` directly (`1 SHA256 ASCII:<key>`).

### 4.19 Fine local timebase (follow-up)

- `esp_timer_get_time()` is the systimer counter (16 MHz on the ESP32-P4, from the 40 MHz crystal) divided by 16 and truncated to whole microseconds. `local_ticks()` reads the counter itself through `esp_timer_impl_get_counter_reg()` (the function behind `esp_timer_get_time()`, also in IRAM; it is declared in a private esp_timer header, so `main.cpp` repeats the declaration). `local_now_ns()` = ticks × 62.5 ns, same zero point as esp_timer.
- Used for: the PPS interrupt (`PpsCaptureEvent::isr_local_ns`; `approximate_edge_us` is the same reading in whole microseconds for timeouts and the GNSS sync), the edge anchor, the model clock reads (software T3, reference time), the bracketed readings of the system clock and of the PTP clock, the PTP write, the interrupt latency test.
- Not changed: timeouts, statistics, MQTT, uptime (whole microseconds are enough there).
- Effect: the 0.29 µs RMS rounding noise is gone from every software time stamp and from all measurements against the local time. The interrupt latency is measured again once (code version 4); with `PTP_FREE_RUNNING 0` the PTP write latency too (code version 2).
- `FINE_LOCAL_TIMEBASE 0` (or a chip other than the ESP32-P4) uses `esp_timer_get_time()` as before.

### 4.20 Free-running PTP clock (follow-up, `PTP_FREE_RUNNING 1`)

The PTP clock is set once (at the time synchronisation, after an Ethernet recovery, or by the servo for offsets above 100 µs) and then never written again. Once per PPS edge, in the PPS task, `ptp_servo_update()`:

1. **Measures** m = raw PTP time - true time at the edge:
   - with the PPS loopback (4.21): from the two captures of the same MCPWM timer, m = -(PTP pulse - GNSS pulse), resolution 12.5 ns, no software delay. The whole seconds come from a short software measurement, which also has to agree within 20 µs (plausibility);
   - otherwise in software: 16 readings of the PTP registers (`esp_eth_mac_get_ptp_time()`, shorter than `clock_gettime`), each bracketed by two fine local time readings. Readings with a bracket longer than 4 µs are dropped, of the rest those within 250 ns of the shortest bracket are used, result = median (robust against a reading that caught the second rollover between the two registers). Each reading is referred back to the edge with the current offset rate.
2. **Filters** with an alpha-beta filter (alpha 0.25, beta 0.035): offset at the edge and free-running rate of the PTP clock (ppb without the frequency command). The command is known exactly and is part of the prediction (piecewise: the old command until the instant of the last update, the new one after it). The start value of the rate is the crystal error measured by the PPS task; the filter refines it, so a PTP clock that does not run exactly with the local timebase is handled too.
   - Outliers (innovation above 1 µs with loopback, 3 µs without, at least 8 × the running mean of the innovations): the filter coasts on the prediction; after 5 in a row the measurement is taken as the new start (and the rate of the PPS task).
   - After a (re)start the first measurement is taken from the software readings only (a loopback pulse captured before the clock was set belongs to the old clock); within the next 3 s a loopback measurement may replace it at once.
3. **Steers** the frequency: command = -rate - offset / 16 s (offset correction at most 20 ppm), set with `esp_eth_mac_adj_ptp_time()`: absolute against the base addend of the EMAC (no accumulation of rounding errors), resolution about 0.4 ppb. The raw PTP clock therefore runs at the GNSS rate and its offset decays with 16 s.
4. **Publishes the mapping** (seqlock): reference point = instant of this update, offset there, offset rate before and after the update. `ntp64_from_hardware_timestamp()` converts `true = ts - offset(ts)`. The mapping is continuous, so a frame time stamped before an update and converted after it is still correct (simulated in-flight error below 60 ns with the loopback, in software mode only the constant measurement error; with the per-second write it was up to 35 µs).

Other cases:

- PPS lost (3.5 s): `ptp_servo_holdover()` sets the command to the pure rate (no offset correction) and continues the mapping from the current offset. The mapping is used for up to one hour, like the model clock. When the PPS returns, the filter starts again with the kept rate.
- Clock set from outside (`synchronize_hardware_clock()`, Ethernet recovery): an epoch counter starts the filter again; after a recovery the EMAC runs with the base addend again (`esp_eth_mac_ptp_enable()`), which the servo is told. Until the next measurement the raw PTP clock is used.
- Steps by the servo set the frequency command first, so the raw clock is right at once (simulated transient after a 3 ms start error: 0.35 µs).
- The servo only runs once the PPS task has a frequency estimate of the crystal.
- Constant error left in the software mode: the position of the register read inside the bracket (the same assumption as in the offset measurement of 4.8). The loopback removes it and measures it (debug `software - loopback`); while the loopback is active its learned value also corrects the software measurement of seconds without a loopback pulse.

### 4.21 PTP PPS loopback (follow-up, optional)

- `PTPPPSLoopbackPin` in `ESP32TimeServerSettings.h` (default -1 = off). With a pin, `setup_ptp_pps_loopback()` routes the PPS output of the EMAC (`esp_eth_mac_set_pps_out_gpio()`, 1 Hz, rising edge at every PTP second) to that pin and creates a second capture channel on the PPS capture timer for the same pin (`mcpwm_new_capture_channel()` enables the input of the pad; the output stays connected). Nothing has to be wired. The interrupt only stores the capture ticks (ring of 4).
- The pin is driven as an output: it must be free and connected to nothing. It must not be `PPSPin`. The PTP PPS output exists from chip revision v3.0 (the board has v3.2, `CONFIG_ESP32P4_REV_MIN_301`).
- The PPS task pairs the GNSS edge with the PTP pulse within ±0.5 s (waits up to 300 µs for a pulse that follows closely). If a pulse is missing or implausible the software measurement of that second is used; after 30 seconds without any usable pulse a warning is printed once (`is the pin free and unconnected?`).
- `PTP_PPS_LOOPBACK_DELAY_NS` (0): delay of the loopback path minus that of the GNSS PPS input; both go through a pad and the GPIO matrix, the difference is a few ns.
- Remaining errors: the PPS jitter of the GNSS module (filtered), 12.5 ns capture resolution, 13 ns steps of the PTP clock with the PLL source, 40 ns from the crystal (4.22) (the pulse comes at the first step after the second, the time stamps show the last step before the frame: on average both are half a step late/early and cancel).

### 4.22 PTP clock from the 80 MHz PLL, root dispersion 1 (2026-10-20)

- `PTP_CLOCK_FROM_PLL 1`: `esp_eth_clock_init()` starts the PTP clock from the 40 MHz crystal (ESP-IDF default, sub-second increment 86 × 2^-31 s = 40.05 ns). `configure_hardware_timestamps()` then calls `ptp_mac_enable()`: the clock gate of `SOC_MOD_CLK_PLL_F80M` is requested (`esp_clk_tree_enable_src()`, kept for good) and the PTP block is started again with `EMAC_PTP_CLK_SRC_PLL_F80M`, period 12.5 ns, `required_accuracy_ns` 13.1. The HAL makes the increment (uint8_t)(13.1 / 0.465) = 28 units = 13.04 ns, addend 0.959 × 2^32 (with the largest frequency command of the HAL, +0.512 %, 0.964 × 2^32: no overflow). PLL_F80M is derived from the 40 MHz crystal, so the servo sees the same rate as before.
- Check after the switch: over 20 ms the PTP clock has to advance like the local timebase within 0.1 %. If not (clock gate, wrong frequency), the crystal configuration is used and a warning is printed. The source chosen at the start is kept: the Ethernet recovery starts the PTP block with the same source (if the PLL then fails, the recovery reports a time stamp failure instead of switching), because the absolute frequency command of the servo (`esp_eth_mac_adj_ptp_time()`) refers to the base addend captured at its first call.
- Effect: time stamp quantization 3.8 ns RMS instead of 11.6 ns per time stamp. The 50 MHz RMII interface (20 ns) now limits the receive time stamps; the gain is not visible to clients with 0.2 µs noise.
- `NTP_ROOT_DISPERSION` 2 → 1 (15.3 µs, smallest non-zero value). Only the error bound shown by the clients changes (chrony `+/-` about 15 µs smaller), not the time.
- `PTP_LATENCY_CODE_VERSION` fingerprint (only with `PTP_FREE_RUNNING 0`) contains `PTP_CLOCK_FROM_PLL`.

### 4.23 Web admin page: HTTPS settings page (2026-10-21)

A settings page on the board replaces `ESP32TimeServerSettings.h` / `ESP32TimeServerSettingsSecrets.h` for the common settings. HTTPS on TCP 443 only, no port 80, no redirect, no option to change that.

**Settings on the page**

| Group | Settings | Takes effect |
|---|---|---|
| Network | FQDN (host name and domain, one field), IPv4 DHCP or address/prefix and gateway (the address received by DHCP is shown), IPv6 SLAAC on/off (the addresses in use are shown), one additional static IPv6 address/prefix with gateway, up to 3 DNS servers (one per line) | at once, with confirmation (FQDN: restart) |
| GNSS / PPS | TX, RX and PPS pin, baud rate (fixed list 4800 to 921600; only used with `UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED 0`, the page says so), PTP PPS loopback pin (empty = off; the page notes that the pin has to be empty: a free GPIO with nothing connected, it is driven as an output) | restart |
| LCD | on/off (restart), top line title, 24-hour clock, hour with leading zero, show time zone, POSIX time zone | at once |
| RGB LED | none / KY-016 / WS2812 and their pins (restart), brightness (at once) | see left |
| MQTT | on/off, broker URI (`mqtt://` or `mqtts://`, host, port), user name, password | restart |
| NTP authentication | on/off, key ID and key (32 letters or digits) or the compiled keys | at once |
| Security | web admin password, allow-list of source networks | at once, stored at once |

Every field shows its compiled default; fields that differ from it are marked. Fields that need a restart carry a "restart" badge.

**Apply / Save / Factory reset**

- **Apply**: the values are in effect at once and are not stored; a restart returns to the saved state.
  - Live settings (time zone, LCD text options, LED brightness, NTP authentication) are switched immediately. The NTP key set is replaced lock-free (seqlock in `ntp_auth.cpp`, the NTP path copies the key it needs and never waits).
  - Network settings (IPv4, IPv6, DNS) are applied at once and have to be **confirmed within 120 s** from the page. With a new static address the confirmation is only accepted over that address (the page lists the URLs; the session cookie is bound to the host name, so the user logs in again there). Without confirmation the previous network settings come back automatically.
  - Settings that need a restart (FQDN, GPIO pins, GNSS baud rate, LED type, LCD on/off, MQTT): Apply writes them as a **one-shot set** (NVS `webcfg/oneshot`) and restarts once. The one-shot set is erased at the start that uses it, before it is used: if it makes the device crash, the next start is back on the saved settings. If the one-shot set also changed the network, the confirmation runs after the start; without it the device restarts with the saved settings.
- **Save**: stores the form values in NVS (`webcfg/saved`, versioned blob with CRC); they are loaded at every start. Network values are only saved if they are the ones in use (apply and confirm them first), so a typo cannot lock the device out at the next start.
- **Factory reset** (needs the current password): erases `webcfg` (saved and one-shot settings) and `webauth` (password, allow-list), keeps the TLS identity (`webtls`) and the calibrations (`isr_latency`, `ptp_latency`, `gnss_state`), then restarts with the compiled settings and the compiled password.
- Order at the start: compiled defaults, then the saved settings (if valid for this firmware), then a one-shot set. A saved set from another firmware version (layout change) or one that fails the validation is ignored with a warning.

**TLS**

- Per-device ECDSA P-256 key, generated on the device at the first start (PSA, hardware RNG), and a self-signed certificate (CN and subjectAltName = FQDN, validity 2026-01-01 to 2099-12-31 because the time may not be known yet). Both in NVS (`webtls`). No private key is compiled into the firmware. A changed FQDN issues a new certificate with the same key.
- The SHA-256 fingerprint of the certificate is printed at every start (`Web admin certificate SHA-256 fingerprint: ...`) and shown on the page.
- TLS 1.2 and 1.3 only (mbedTLS 4 has nothing older), cipher suites TLS 1.3 AES-GCM and TLS 1.2 ECDHE-ECDSA with AES-GCM, no renegotiation, no TLS 1.3 PSK modes, handshake timeout 5 s.
- No HSTS: with a self-signed certificate it would prevent clicking through the browser warning.
- The transport is set up in `web_server.cpp` on `esp_http_server` with `esp-tls` (instead of `esp_https_server`, which does the TLS handshake before the application can look at the connection): the **allow-list is checked before the TLS handshake**, rejected sources cost no TLS work.

**Login, session, CSRF**

- Login form, no HTTP Basic authentication. Password only stored as salted PBKDF2-HMAC-SHA256 (10000 iterations, 16 byte salt, about 0.3 s per check; the iteration count is stored with the hash). Comparison in constant time. The compiled `WebAdminPassword` ("password" by default, `ESP32TimeServerSettingsSecrets.h`) is hashed with a random salt at every start and checked the same way.
- While the factory password is in use: red banner on the page and a console warning at every start.
- Session: random 256-bit token in the cookie `__Host-sid` (`Secure; HttpOnly; SameSite=Strict; Path=/`), only its SHA-256 is kept on the device; 15 min idle timeout, 12 h maximum, at most 4 sessions, logout button.
- CSRF: SameSite=Strict, a per-session token in the `X-CSRF-Token` header, the `Origin` header must equal `https://` + `Host`, `Sec-Fetch-Site` (if sent) must be `same-origin`, and the body must be `application/json` on every state-changing request.
- Login throttling per source address (after 5 failures 30 s, doubling up to 15 min; forgotten after an hour without failures) and global (20 failures in a minute block all logins for 60 s, doubling up to 15 min). It only affects the web admin, never NTP. Checks of the current password (password change, factory reset) count as well.
- Password change and factory reset need the current password. A new password (12 to 128 characters, no control characters) is stored at once and closes all other sessions.
- Secrets (MQTT password, NTP key, admin password) are never sent to the browser and never logged; the page shows "set / not set". An empty field keeps the stored value, a separate check box clears it (MQTT password) or returns to the compiled keys (NTP key).

**Page hardening and resources**

- HTML, CSS and JavaScript are separate files embedded with `EMBED_TXTFILES`; no inline script or style. The JavaScript writes values only with `textContent` / `.value`.
- Headers: `Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Frame-Options: DENY`, `Cross-Origin-Opener-Policy` and `Cross-Origin-Resource-Policy: same-origin`, `Cache-Control: no-store` for the API.
- Dark mode with CSS variables, dark is the default. The toggle button shows the mode it switches to (U+2600 sun = light, U+1F319 crescent moon = dark); the choice is stored in the browser (localStorage).
- At most 3 sockets with LRU purge, request body at most 4 kB, receive / send timeout 5 s, request headers up to 2 kB (`CONFIG_HTTPD_MAX_REQ_HDR_LEN`, browsers send long headers), mbedTLS dynamic buffers (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`).
- The server task runs at priority 3 on core 0: below every timing task (PPS 14, GNSS 15, hardware NTP 17 on core 1, NTP sockets 20, Ethernet recovery 6, MQTT 5). Stack 10 kB (TLS handshake, PBKDF2, JSON); a one-time start task (8 kB) creates the key. The task stacks of `main.cpp` are unchanged.
- Strict validation on the device (`web_settings_validate`): `inet_pton`, IPv4 prefix 8 to 30, address not the network / broadcast / loopback / multicast / link-local address, gateway another host of the subnet; IPv6 address global or unique local, prefix 64 to 128 (lwIP decides on-link with the first 64 bits), gateway link-local or in the /64 of the address; DNS unicast; FQDN after RFC 1123 (host name label at most 32 characters, the esp_netif limit); time zone parsed as POSIX TZ; LCD title 1 to 17 printable characters; MQTT URI scheme, host and port, no user, path or spaces; GPIOs 0 to 54, not used twice and not one of: Ethernet RMII / MDC / MDIO / PHY reset (28, 29, 30, 31, 34, 35, 49, 50, 51, 52), strapping and UART0 console (36, 37, 38), USB Serial/JTAG (24, 25), TF card (39 to 44), LCD I2C (7, 8), the button pin if the button is enabled. Empty PTP PPS loopback pin = off.
- Physical recovery: with `UPTIME_RESTART_BUTTON_ENABLED 1`, holding the button for 10 s while the device starts does the factory reset (the LCD says so).

**Network implementation (`web_net.cpp`)**

- IPv4 through `esp_netif` (DHCP client stop/start, `esp_netif_set_ip_info`); the raw NTP path compares against the own IPv4 address, which was already a lock-free atomic and is updated by the got-IP event.
- Static IPv6 address with `esp_netif_add_ip6_address`. lwIP has no API for a static IPv6 default gateway: `CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM` with `lwip_hook_nd6_get_gw()`, which reads the gateway lock-free (seqlock) in the lwIP thread; the destination cache is cleared after a change. `CONFIG_LWIP_IPV6_NUM_ADDRESSES` 3 → 5 (link-local, SLAAC global and unique local, static, spare).
- SLAAC off: autoconfiguration is switched off on the lwIP netif and the addresses it created are removed (link-local and static stay).
- DNS: up to 3 servers (lwIP default `DNS_MAX_SERVERS`). A DHCP lease overwrites the DNS servers, so the configured ones are set again after every lease; with an empty list the servers of the lease are used.
- The page is started right after the Ethernet driver, before the wait for an address and independent of the GNSS setup: it stays reachable when wrong GNSS pins stop the start.

**Features compiled in, idle when off**

- LCD, KY-016, WS2812, MQTT and NTP authentication are always compiled in (`main/CMakeLists.txt` no longer reads the settings file); the `*_ENABLED` values in `ESP32TimeServerSettings.h` are now their run-time defaults. In `main.cpp` the five macros are set to 1 after the settings file is included, and the boot settings decide at run time (`s_lcd_on`, `s_led_type`, `s_mqtt_on`, `ntp_auth_configure()`).
- When off: no task, no driver, no buffers. The MQTT report buffers (about 76 kB) are allocated only when MQTT is on (`setup_mqtt_tf_queue()`); the per-request MQTT counters in the NTP paths are skipped; the LED/LCD/button task is not created when neither is used; I2C, LEDC and RMT are not initialised.
- MQTT and the LCD switch need a restart (the MQTT client, its TF card queue and the I2C bus have no clean stop in this code); the page marks them. A failing LED driver (e.g. wrong pin) switches the LED off for that start instead of stopping the server.
- New on the page for MQTT: on/off and the broker URI (was `MQTTServerIPAddress` + `MQTTPort`); `mqtts://` brokers are checked against the ESP-IDF CA bundle. New for NTP authentication: on/off.
- `NTP_MAX_PACKET_SIZE` is now always 72 bytes (the raw path buffers grow by 24 bytes each). With authentication off an authenticated request is dropped as before (it is counted as invalid).

**Open points decided**

- Allow-list of source networks: implemented (decision of 2026-10-21): at most 8 addresses or CIDR networks, IPv4 and IPv6, empty = all; a list that would lock out the current client is refused.
- NVS encryption: not enabled (decision of 2026-10-21), the steps are in 13.

**First start, fingerprint check, recovery**

1. Delete `sdkconfig` (the new options in `sdkconfig.defaults` are only used for a new `sdkconfig`), then `idf.py build` and flash. The build stops with an error in `web_net.cpp` if the ND6 hook option is missing.
2. At the first start the device creates its TLS key (about 1 s) and prints `Web admin certificate SHA-256 fingerprint: AB:CD:...`. Note it.
3. Open `https://<address>/` (IPv4 or `https://[IPv6]/`). The browser warns about the self-signed certificate: open the certificate details and compare the SHA-256 fingerprint with the console line before accepting it. After a factory reset the fingerprint stays the same; it changes only with a new FQDN or when `webtls` is erased.
4. Log in with `password` (or the `WebAdminPassword` of the secrets file) and change the password at once (red banner until then).
5. Change settings, **Apply**, check, **Save**. Network changes: confirm within 120 s (over the new address if a static one was set), then Save.
6. Forgotten password: factory reset with the button (hold 10 s during the start, `UPTIME_RESTART_BUTTON_ENABLED 1` needed), or `idf.py erase-flash` + flash (erases all NVS data including the calibrations and the TLS key), or erase only the `webauth` namespace with the NVS partition tool.

## 5. Security and open ports

### 5.1 Open ports

With the current settings (OTE, MQTT off):

| Protocol | Port | Who | Remark |
|---|---|---|---|
| UDP | 123 | raw NTP path (hardware time stamps) and lwIP sockets (IPv4 `0.0.0.0`, IPv6 `::`, IPv6 link-local) | the sockets get what the raw path leaves to lwIP |
| **TCP** | **443** | **web admin page (HTTPS only, IPv4 and IPv6 on one dual-stack socket)** | **new on 2026-10-21 (4.23); TLS 1.2 / 1.3, allow-list checked before the handshake, at most 3 connections** |
| UDP | 68 | lwIP DHCP client | only while DHCP is used |
| UDP | 32769 | control socket of `esp_http_server` | bound to 127.0.0.1 only, not reachable from the network |
| ICMP / ICMPv6 | - | lwIP | ping replies, IPv6 neighbour discovery, MLD; ARP |
| TCP | all others | none | a SYN gets a RST |

With `OTE_UPDATES_ENABLED 1` additionally UDP 3232 (ArduinoOTA) and UDP 5353 (mDNS), plus a TCP connection the device opens to the uploading PC. With `MQTT_ENABLED 1` only an outgoing TCP connection to the broker. Check: `nmap -sU -p 1-65535 <ip>`, `nmap -sT -p 1-65535 <ip>`, the same with `-6`.

### 5.2 Raw NTP path filter (review)

Requests are answered on the raw path only if:

- the Ethernet destination is unicast and the source is unicast,
- IPv4: not a fragment, total length and header checksum correct, destination = the assigned IPv4 address, source not 0.0.0.0/8, 127.0.0.0/8, multicast/reserved/broadcast or the own address,
- IPv6: destination not multicast, source not multicast, unspecified or loopback, UDP checksum present,
- UDP: source port not 0, checksum correct (for IPv4 a checksum of 0 means "not used").

Everything else is handed to lwIP, which applies its own rules (broadcast requests, fragment reassembly, foreign addresses).

### 5.3 Exposure

- Amplification: none worth mentioning (mode 3 → mode 4, same size; no mode 6/7).
- Flooding: global load shedding (8500 or 14500 requests per second depending on the chip revision) and the Ethernet recovery task. A single flooding host triggers drops for all clients; a flood with spoofed sources pushes real clients out of the interleaved cache (fallback to basic mode). No per-client rate limiting.
- Time spoofing: possible on the path without authentication; NTPv4 symmetric keys are available (SHA-256 truncated to 160 bit, constant time comparison). Unauthenticated requests are still answered when authentication is enabled (by design).
- OTE (if enabled): MD5 challenge on the password, image unencrypted and unsigned. Use a long random password, a separate VLAN, consider Secure Boot V2 and flash encryption.
- MQTT (if enabled): `mqtt://` sends the credentials in clear text; prefer `mqtts://` (the broker certificate is checked against the ESP-IDF CA bundle).
- Web admin (4.23): whoever knows the password can change every setting on the page, including the network and the NTP key. Change the default password at once, use the allow-list, keep the device in a management network. The password hash, the settings with the MQTT password and NTP key, and the TLS private key are stored in NVS without encryption (as the compiled secrets are stored in the firmware): someone with physical access can read them with a USB cable (see 13 for NVS and flash encryption). Login attempts are throttled; a flood of TLS connections can keep the page busy (3 connections, LRU purge) but does not affect NTP (priority 3, core 0).

## 6. Review of the original clock correction code

What the original code did: measured the PPS interval from the hardware ticks and averaged 128 intervals into one ppm value per 128 s; smoothed it (`0.9 × old + 0.1 × new`, starting at 0); at every PPS `adjtime(-phase error - estimated ppm)` with a dead band of 2 µs.

| Part | Verdict |
|---|---|
| Measuring the frequency from ticks | correct and precise; the sign of the correction is correct (only the comments said the opposite) |
| Applying it as a step at the PPS | does not work: a step cannot flatten a ramp. The clock then runs from `-drift` to 0 instead of from 0 to `+drift`; mean absolute and RMS error unchanged. A frequency correction has to act continuously over the second |
| Convergence | 63 % after 21 minutes, 90 % after 47 minutes, nothing during the first 128 s |
| Phase measurement | bias of about -7 µs: interrupt latency (2.5 µs) not compensated, `gettimeofday()` read after `esp_timer_get_time()` (the 9.2 µs call counted as 4.6 µs too late) |
| Noise | interrupt jitter and 1 µs rounding into the clock with gain 1 every second; dead band 2 µs, truncation |
| Other | stale time for the PTP clock, no protection against spurious pulses, hard-coded 80 MHz |

## 7. Measured and simulated results

### 7.1 Simulation of the clock correction (earlier work)

Simulated environment: true time, oscillator with frequency error, esp_timer with 1 µs resolution and call costs, MCPWM ticks, system clock with `adjtime`, PPS jitter 25 ns, interrupt latency 2.5 µs plus jitter and 3 % outliers, `gettimeofday` costing 9.2 µs.

Original code (system clock, used for all time stamps then), 3 hours:

| Oscillator | Period | Mean | RMS | Peak to peak |
|---|---|---|---|---|
| -18.7 ppm | first 2 minutes | -16.7 µs | 17.6 µs | 23 µs |
| -18.7 ppm | minutes 5 to 30 | -7.4 µs | 9.6 µs | 33 µs |
| -18.7 ppm | hours 2 to 3 | +1.3 µs | 5.5 µs | 24 µs |
| +25 ppm | hours 2 to 3 | -19.6 µs | 20.8 µs | 29 µs |

Model clock after the earlier work (after 2 minutes): frequency within 4 ppb, mean error -60 to +37 ns, RMS 0.31 µs (esp_timer rounding), peak 0.77 µs; an oscillator step of 0.5 ppm is followed within 16 s; a missed pulse has no effect. Spike tests: a single spurious edge is ignored; three in a row are trusted and the estimators restart; a real PPS step of 100 µs is followed after three ignored edges.

Anchor bias (call timing jitter 0 / 0.1 / 0.2 / 0.5 µs): minimum of 16 on both sides -0.35 / -0.16 / +0.06 / +0.37 µs, central value of 64 -0.04 / -0.02 / +0.02 / +0.09 µs.

### 7.2 Simulation of the PTP offset measurement (review)

2000 s, esp_timer 1 µs, PTP clock 25 ns, oscillator -18.7 ppm, write latency 3850 ± 300 ns, call times with jitter:

| | Error of T2 / hardware T3 |
|---|---|
| Without offset measurement | mean -284 ns, RMS 501 ns, max 1517 ns |
| With offset measurement | mean -1 ns, RMS 47 ns, max 197 ns |

### 7.3 Measurements with chrony (earlier work, before the review)

Client: chrony on a Synology DiskStation, both server addresses with `iburst xleave minpoll 0 maxpoll 2`.

| Step | Observation |
|---|---|
| Original code | no interleaved mode, `Peer delay` clamped to 122.12 µs, large offsets because the server was not preferred |
| After the cache fix and hardware TX time stamps | `Interleaved: Yes` on IPv4 and IPv6, `TX timestamping: Hardware`, response time 87 to 103 µs |
| After precision and root dispersion | peer delay 87 µs (IPv4) and 69 µs (IPv6), peer dispersion 1.4 µs, root distance 53 to 57 µs |
| Statistics after about 20 minutes | Std Dev 257 ns (IPv4) and 303 ns (IPv6), offsets +1.3 µs and -1.4 µs, frequency skew 0.008 and 0.002 ppm |
| Counters over 24 h | 0.36 % of the requests unanswered (probably restarts during the work), 3.5 to 3.6 % valid but not good |

The offset of the client against the server is zero by construction because the client is locked to it; the numbers show the noise of the path and time stamps, not the accuracy against UTC.

### 7.4 Debug output of the test board (review firmware, -O2, 2026-10-19, about 70 minutes)

Test board `test-wifi-board.ntp` (192.168.8.123) with `DEBUG_ENABLED 1`, polled by chrony over IPv4.

| Quantity | Result |
|---|---|
| Stored calibrations used | interrupt latency 2.50 µs (unchanged by -O2), PTP write latency 3463 ns (was 3850 ns with -Og: -O2 changed the code timing, the fingerprint change was needed) |
| Oscillator | -33.04 ppm at the start, -33.08 ppm after 70 minutes (warm-up, 40 ppb), followed by the 16 s window |
| `d_central` | 26220278.31 to .47 µs over the whole run: very little anchor noise |
| `jitter_excess` | mostly within ±0.6 µs, single late interrupts of 4.45 µs and 2.04 µs (no effect, the anchor follows the hardware ticks) |
| Periodic GNSS checks | 7 of 7 "clock already correct, not stepped"; no disturbance of the PPS lines around them |
| PTP offset after write (259 values) | mean -94 ns, RMS 318 ns, range -906 to +541 ns, 90 % within ±500 ns, lag-1 correlation 0.11. Close to the 289 ns RMS of a 1 µs quantization: the error of the per-second write comes almost entirely from the esp_timer resolution, latency jitter adds about 130 ns. This error was in T2 and the hardware T3 before and is removed now |
| Basic mode TX delay (IPv4) | converges from 23 µs to about 14.0 to 15.4 µs (mean about 14.3 µs). Basic clients saw an offset error of about 7 µs with all earlier firmware; it is compensated now |
| `PTP set retry` | 15 in about 68 minutes, mostly with a deviation of about -5.7 µs (probably an interrupt between the clock reading and the write), irregular intervals; one retry always succeeds |

### 7.5 Comparison with chrony (review firmware vs. Sonnet firmware, 2026-10-19, 2.7 hours)

Client: chrony on the DiskStation, `log measurements statistics`, chrony and the test board restarted at the start. Both new boards polled over IPv4 (`test-wifi-board.ntp` 192.168.8.123 with the review firmware, `1.ntp` 192.168.8.7 with the Sonnet firmware), the old boards 2.ntp and 3.ntp over IPv6, all with `iburst xleave minpoll 0 maxpoll 2`.

Observation that dominates the raw numbers: for both new boards the offset depends on the delay with a slope of -0.50 (correlation -0.99). The whole delay variation is on the path from the board to the DiskStation (kernel RX time stamps on the DiskStation and/or the OpenVSwitch bridge; chrony reports a jitter asymmetry of +0.50). The raw offsets of the "good" samples therefore spread by about ±6 µs, and only 43 % (test board) and 48 % (1.ntp) of the samples pass chrony's delay test (all failures are test C, `maxdelaydevratio`). This is a property of the client, not of the servers.

The two directions were therefore evaluated separately: offset + delay/2 = T2 - T1 (DiskStation hardware TX → network → board hardware RX) and offset - delay/2 (return path). Noise = robust standard deviation after removing a 2 minute running median (removes the movement of the client clock).

| | Test board (review) | 1.ntp (Sonnet) | 2.ntp / 3.ntp (old) |
|---|---|---|---|
| Samples | 2431 | 2413 | 2420 / 2404 |
| Interleaved | 100 % | 100 % | 0 % |
| Noise in the direction to the board (T2 - T1) | **0.28 µs** | 0.42 µs | 11 µs |
| Noise in the direction back | 18.5 µs | 18.5 µs | 11 µs (delay clamped to 122 µs, the directions cannot be separated) |
| chrony statistics log: Std dev (median) | **223 ns** | 324 ns | 10.1 / 9.8 µs |
| chrony statistics log: offset SD (median) | **95 ns** | 132 ns | 3.3 / 3.0 µs |
| chrony estimated offset (median, asymmetry corrected) | +0.22 µs | -0.12 µs | +60 / +57 µs |
| Shortest round trip (1st percentile) | 33.8 µs | 27.5 µs | 122 µs (clamped) |
| Missing replies | **0** (largest gap between samples 5 s) | 8 (four gaps of 12 to 13 s) | single ones at most (largest gap 8 / 9 s) |
| Invalid samples | none | 4 (offset, delay and dispersion exactly 0) | none |

Results:

- Noise: the test board has about a third less noise in the direction that contains its receive time stamp (0.28 µs instead of 0.42 µs). The difference √(0.42² - 0.28²) = 0.31 µs equals the PTP write error measured on the board (318 ns RMS, 7.4), which the offset measurement removes. chrony's own figures show the same ratio (223 ns instead of 324 ns).
- 10-minute disturbance: the 8 missing replies and the 4 invalid samples of 1.ntp happened at 14:14:39, 14:34:42, 14:54:43 and 15:24:46 (UTC), i.e. at periodic GNSS checks of the Sonnet firmware (stratum 16 during the check, then a clock step with an uncompensated PTP clock). The test board has none of these.
- Lost replies: none on the test board in 2.7 hours. The 3.6 % missing replies seen earlier in `chronyc ntpdata` came from the reboots during the work, not from the new receive path filter.
- Agreement of the two new boards: the test board is a constant +2.77 µs later than 1.ntp in the direction to the board (20 minute medians between +2.74 and +2.80 µs). Its shortest round trip is 6.3 µs longer, i.e. it sits on a slightly longer network path; with a symmetric path half of it (3.15 µs) explains the difference, leaving about -0.4 µs. chrony's asymmetry corrected offsets differ by 0.34 µs. The boards agree to within about 0.4 µs. To confirm that the 2.8 µs belong to the path and not to the board, swap the switch ports or cables of the two boards.
- Old firmware: 56 to 60 µs away from the new boards, about 10 µs noise.
- Limiting factor now: the receive side of the DiskStation (18.5 µs noise on the return path). Hardware RX time stamps on the DiskStation would remove most of it and raise the share of good samples well above 50 %.

### 7.6 Simulation of the free-running PTP clock (follow-up)

Host simulation (`test/sim_servo.cpp`) with the code of `main.cpp`: crystal -33.06 ppm, systimer 16 MHz, EMAC PTP clock with 40 ns steps (the table; with the PLL source of 4.22 see the note below it), fine addend (integer arithmetic of the HAL) and the two-register read, GNSS PPS jitter 20 ns RMS, error of the PPS anchor +40 ns ± 15 ns, call times with jitter, 1 % interrupted readings, PTP register read 450 ns after the start of the bracket (an assumed value: the real one is not known), PPS task 150 ± 30 ms after the edge. 1800 s per case, error of T2 / hardware T3 at 20 random instants per second after the first 120 s; "in-flight" = frame time stamped 100 µs after the edge and converted after the update of that second. Ranges over 8 random seeds.

| Case | Per-second write (`PTP_FREE_RUNNING 0`) | Free-running, software measurement | Free-running, loopback |
|---|---|---|---|
| Constant crystal: mean / RMS / max | -177 / 180 / 280 ns | -177 / 178 / 247 ns | +3 / 16 / 66 ns |
| In-flight frames: max | **35 µs** | 236 ns | 57 ns |
| Crystal drifting 0.2 ppb/s for 300 s | RMS 180 ns | RMS 178 ns | RMS 17 ns |
| PPS jitter 60 ns | RMS 188 ns | - | RMS 33 ns |
| Holdover, 300 s without PPS: max | 2.0 µs | 0.93 µs | 0.31 µs |
| Ethernet recovery (clock set ±5 µs, base addend): max from 2 s after | 0.27 µs | 0.23 µs | 0.06 µs |
| 30 % of the loopback pulses lost | - | - | RMS 17 ns |
| Loopback pin floating (random edges) | - | RMS 178 ns (all rejected, software used) | - |

- The mean of about -177 ns in the first two columns is the assumed read position inside the bracket; it is a constant error of the software measurement in both modes (the real value of the board is not known, 7.4 suggests some 100 ns). The loopback measures it (debug `software - loopback`, simulated 175 to 183 ns) and removes it.
- The noise around that mean is 29 ns RMS with the per-second write and 19 ns with the free-running clock in software mode.
- The per-second write gives errors of up to the crystal drift of one second (35 µs at -33 ppm) for frames that straddle a write. The simulation has such a frame in every second by construction; on the board it is about 1 in 10000 frames (section 2).
- With the PTP clock from the PLL (4.22, 13 ns steps) the simulated loopback error drops from 16 to 10 ns RMS (max 38 ns). The simulation does not model the 20 ns granularity of the RMII interface, so the real gain is smaller.
- These are simulations: the timing values of the board (read position, bus delays) are assumptions. The relative results (no in-flight error, filter noise, loopback removes the constant error) do not depend on them.

## 8. Settings reference

| Setting | Place | Value | Meaning |
|---|---|---|---|
| `PPS_Discipline_Task_Stack_Size` and the other stacks | `main.cpp`, top | see 4.15 | task stacks |
| `CALCULATE_STACK_SIZES_ENABLED` | `main.cpp`, top | 0 | 1 = report the needed stack sizes |
| `NTP_PRECISION_EXPONENT` | after the local timebase | -24 (-20 without fine timebase) | advertised precision |
| `FINE_LOCAL_TIMEBASE` | before the model clock | 1 | 1 = raw systimer counter (62.5 ns), 0 = `esp_timer_get_time()` (1 µs) |
| `PTP_FREE_RUNNING` | before the PTP write latency block | 1 | 1 = free-running PTP clock with servo (4.20), 0 = written every second (4.8) |
| `PTPPPSLoopbackPin` | `ESP32TimeServerSettings.h`, after `PPSPin` | -1 | GPIO for the PTP PPS loopback (4.21), -1 = off |
| `ptp_filter_alpha`, `ptp_filter_beta` | PTP servo | 0.25, 0.035 | weights of the offset / rate filter |
| `PTP_SERVO_TIME_CONSTANT_S` | PTP servo | 16 | decay time of the PTP offset under the servo |
| `ptp_step_threshold_ns` | PTP servo | 100 µs | larger offsets are stepped, smaller ones slewed |
| `PTP_PPS_LOOPBACK_DELAY_NS` | PTP servo | 0 | delay of the loopback path minus the GNSS PPS input path |
| `NTP_ROOT_DISPERSION` | `main.cpp`, top | 1 | advertised root dispersion, 1/65536 s per unit |
| `PTP_CLOCK_FROM_PLL` | after `PTP_FREE_RUNNING` | 1 | 1 = PTP clock from PLL_F80M (13 ns steps), 0 = 40 MHz crystal (40 ns steps) |
| `PPS_ISR_LATENCY_DEFAULT_US` | compensation block | 2.6 | used until a value is measured |
| `PTP_LATENCY_AUTO` | compensation block | 1 (forced to 0 with `PTP_FREE_RUNNING 1`) | calibrate and store the PTP write latency; 0 = always the default |
| `PTP_LATENCY_FORCE_RECALIBRATION` | compensation block | 0 | 1 = calibrate again at the next start |
| `PTP_WRITE_LATENCY_DEFAULT_NS` | compensation block | 3850 | used until a value is stored or calibrated |
| `PTP_OFFSET_MEASUREMENT` | PTP compensation block | 1 | `PTP_FREE_RUNNING 0` only: measure the PTP offset after every write; 0 = assume the write was exact |
| `NTP_HW_TX_TIMESTAMPS` | before the transmit helper | 1 | 0 = send without hardware TX time stamps |
| `NTP_BASIC_T3_TX_COMPENSATION` | after the transmit helper | 1 | 0 = basic T3 without transmit delay compensation |
| `PPS_ISR_LATENCY_AUTO` | before the PPS callback | 1 | measure and store the interrupt latency; 0 = always the default |
| `PPS_ISR_LATENCY_FORCE_REMEASURE` | before the PPS callback | 0 | 1 = measure again at the next start |
| `PPS_ISR_LATENCY_CODE_VERSION` | before the PPS callback | 4 (3 without fine timebase) | increase when the PPS interrupt path or the statistic changes |
| `PTP_LATENCY_CODE_VERSION` | PTP calibration block | 2 (1 without fine timebase) | increase when the way the PTP clock is written changes |
| `spikeLimitUs`, `maxSpikes` | PPS task | 50 µs, 3 | spike rejection |
| `anchorWindow` | PPS task | 64 | edges for the central value of the anchor |
| `freqCap`, `freqMinSeconds` | PPS task | 17, 4 | frequency window (16 s), first estimate after 4 s |
| `labelDeadlineUs`, `ubxPollDelayMs` | `acquire_sync_candidate` | 950 ms, 300 ms | label data window after the PPS edge |
| `NTP_CACHE_MATCH_PORT` | `ntp_cache.c` | 0 | 1 = also compare the client source port |
| `CACHE_TIMEOUT_MS_IPV4/IPV6` | `ntp_cache.h` | 1800000 | lifetime of a cache entry |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `sdkconfig.defaults`, `sdkconfig` | y | -O2 |
| `WebAdminPassword` | `ESP32TimeServerSettingsSecrets.h` | "password" | password of the web admin page until one is set on the page; again after a factory reset |
| `DeviceName` | `ESP32TimeServerSettings.h` | "ESP32TimeServer" | default FQDN of the page (host name for DHCP and OTE, certificate name) |
| `*_ENABLED` of LCD, LED, MQTT, NTP authentication, the pins, the network values, `timeZoneSpec`, `lcdTopLineTitle`, `display*`, `RGB_LED_Brightness_Percent`, `MQTTServerIPAddress` / `MQTTPort`, `MQTTUsername` / `MQTTPassword`, `symmetric_keys` | settings files | as before | defaults of the web admin page (4.23); saved page settings take precedence |
| `NETWORK_CONFIRM_TIMEOUT_US`, `MAX_SOCKETS`, `MAX_BODY`, `TASK_PRIORITY`, `TASK_CORE` | `main/web/web_server.cpp` | 120 s, 3, 4 kB, 3, 0 | network confirmation, connections, request body, server task |
| `PBKDF2_ITERATIONS`, `MAX_SESSIONS`, `FAILURES_BEFORE_LOCKOUT`, `FIRST_LOCKOUT_US`, `MAX_LOCKOUT_US` | `main/web/web_auth.cpp` | 10000, 4, 5, 30 s, 15 min | password hash and login throttling |
| `CONFIG_MBEDTLS_X509_CREATE_C`, `..._X509_CRT_WRITE_C`, `..._SSL_PROTO_TLS1_3`, `..._DYNAMIC_BUFFER`, `CONFIG_HTTPD_MAX_REQ_HDR_LEN`, `CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM`, `CONFIG_LWIP_IPV6_NUM_ADDRESSES` | `sdkconfig.defaults` | y, y, y, y, 2048, y, 5 | needed by the web admin (4.23) |

## 9. Data stored in NVS

| Namespace | Key | Content |
|---|---|---|
| `isr_latency` | `ns` (int32), `fp` (uint32) | interrupt latency in ns, fingerprint of the configuration |
| `ptp_latency` | `ns` (int32), `fp` (uint32) | PTP write latency in ns, fingerprint of the configuration (only read and written with `PTP_FREE_RUNNING 0`) |

A value is only used if its fingerprint matches and it is within 0.5 to 20 µs; otherwise it is measured again. Other namespaces (`gnss_state`, `restart_evt`) are unchanged from the original project.

Web admin (4.23):

| Namespace | Key | Content | Factory reset |
|---|---|---|---|
| `webcfg` | `saved` (blob) | settings saved with "Save": magic, layout version, size, flags, all settings incl. MQTT password and NTP key, CRC-32 | erased |
| `webcfg` | `oneshot` (blob) | settings of "Apply" that need a restart; erased at the start that uses them | erased |
| `webauth` | `pw` (blob) | admin password: PBKDF2-HMAC-SHA256 hash, salt, iterations, CRC-32 | erased |
| `webauth` | `allow` (blob) | allow-list (at most 8 entries), CRC-32 | erased |
| `webtls` | `key`, `crt` (blob), `name` (string) | TLS private key (DER), self-signed certificate (DER), FQDN it was issued for | kept |

## 10. Console messages

Always printed (brief):

- `PPS ISR latency: using the stored value X us` / `no stored value for this configuration, using X us until it is measured`
- `PPS ISR latency measurement started ...`, the statistics line, `central value (mean of the lowest 75 %) = X us`, `X us applied (was Y us) and stored in NVS`
- `PTP clock: 80 MHz PLL source, time stamp steps of 13 ns` or the warning `PTP clock: the 80 MHz PLL source does not work, using the 40 MHz crystal (40 ns steps)`
- `PTP clock: free-running and frequency disciplined, measured against the PPS by software readings` or `PTP PPS loopback on GPIO n: the PTP clock is measured against the PPS in hardware` (at the Ethernet start)
- Loopback problems: `PTP PPS loopback: PPS output on GPIO n failed ...`, `... capture channel on GPIO n failed ...`, `... GPIO n cannot be used ...`, `PTP PPS loopback on GPIO n: no usable pulses (...); is the pin free and unconnected?`
- `PTP_FREE_RUNNING 0` only: `PTP write latency: N ns read from NVS (calibration skipped)` / `no stored value for this configuration, using the default 3850 ns until the calibration has finished ...`
- `PTP write latency calibration: PPS locked, warm-up started (10 min)`, `warm-up done, measuring (about 30 to 40 min)`, `finished: N ns (default was D ns), stored in NVS`
- Failures: calibration not stable, result outside the range, NVS errors, out of memory, task could not be started, `Hardware TX time stamping disabled after repeated errors`.

- Web admin (always): `Settings: compiled defaults` / `saved settings (NVS)` / `one-shot settings of the web admin (valid for this boot only)`, warnings for saved settings that are ignored, `Web admin certificate SHA-256 fingerprint: ...`, `Web admin: https://<fqdn>/ (TCP 443, IPv4 and IPv6)`, `Web admin: the default password is in use - change it on the settings page`, `Web admin: failed login from <address>`, `too many failed logins, all logins blocked for N s`, `password changed`, `allow-list changed`, `settings applied`, `settings saved to NVS`, `network settings confirmed`, `Network settings were not confirmed within 120 s: ...`, `factory reset ...`, the TLS key and certificate creation at the first start. Secrets are never logged.
- The feature lines at the start (`LED support`, `LCD support`, `Symmetric-key authentication support`, `MQTT support`, `Host name`) now show the state of the boot settings.

Only with `DEBUG_ENABLED`:

- progress of the PTP calibration every 2 minutes, statistics of every block, spread of the last three blocks, PPS lock lost during the calibration
- `PTP set retry n: write deviation X ns`
- `PPS spike ignored (n of 3): interval X s`
- every 16 s: `PPS: ppm=..., raw_phase, resid, jitter_excess, d_central, gap`, then
  - `PTP: loopback|software offset <estimate> ns (meas <m>, innov <i>, noise <n>) rate <r> ppb cmd <c> ppb | outliers <o> steps <s> cmd err <e>` (`PTP_FREE_RUNNING 1`; with loopback also `PTP loopback: used <u> missing <m> rejected <r> | software - loopback <b> ns (<samples>)`) or `PTP: offset after write <n> ns (<k> reads)` (`PTP_FREE_RUNNING 0`)
  - `Basic T3 TX delay: v4 <d> ns (<samples>) v6 <d> ns (<samples>)`
- `PTP clock set (offset was X us)`, `PTP servo: offset jumped by X us, filter restarted`, `PTP servo: holdover, offset ..., rate ..., command ...`
- `GNSS time sync ... (clock already correct, not stepped)`, `GNSS second differs from the clock by N s; confirming before the clock is stepped.`
- the secret NTP keys (only with authentication enabled)

## 11. How the changes were verified

| Area | Method | On the board? |
|---|---|---|
| Frequency estimate, model clock, spike rejection, anchor | simulated environment with the extracted code (7.1) | no |
| PTP write latency read-back and calibration | simulated PTP clock and NVS (stable run, drift, PPS loss, NVS errors, range check) | partly: earlier manual measurements |
| Interrupt latency measurement and NVS flows | simulated callback and NVS | measurement run on the board: 2.5 µs |
| Client cache, interleaved mode | host test with stubs | chrony: `Interleaved: Yes`, `TX timestamping: Hardware` |
| PTP offset measurement and conversion | simulation (7.2) | no |
| Raw path filter and checksums | host test with 24 frames generated by an independent implementation, reference IPv6 checksum | no |
| Transmit delay estimator | host test | no |
| Compilation | `-fsyntax-only` against stubs in 9 feature combinations; host compile with -O2 and the IDF warning flags in 5 combinations; CMake rule tested with mocked IDF functions | build with ESP-IDF 6.1 and -O2 by the user (the build fixes in 4.17 came from those builds) |

| Review changes on the board | debug output of the test board (7.4), 2.7 h chrony comparison against the Sonnet firmware (7.5) | yes |
| Follow-up: fine timebase, free-running PTP clock, loopback | `-fsyntax-only` against stubs (extended by the PTP PPS and frequency functions) in 8 combinations incl. `PTP_FREE_RUNNING 0` and `FINE_LOCAL_TIMEBASE 0`, -O2 with the IDF warning flags; servo simulation with 12 cases and 8 seeds (7.6); the existing host tests (frames, checksums, TX delay, legacy PTP mapping) | yes: built with ESP-IDF 6.1, loopback on GPIO 23 active on all four boards, stack sizes measured, overnight chrony run (13 h) and all boards within about 2 µs on the DiskStation |
| PLL clock source, root dispersion 1 (2026-10-20) | `-fsyntax-only` in 7 combinations incl. `PTP_CLOCK_FROM_PLL 0`, -O2 with the IDF warning flags; unit test of the source selection (PLL works / fails at the start / fails at a recovery, increment and addend headroom); servo simulation with 13 ns steps | **not yet** |

| Web admin page (2026-10-21) | `idf.py build` with ESP-IDF 6.1, -O2 and the IDF warning flags, `sdkconfig` regenerated from `sdkconfig.defaults` (no new warnings in the project code); the page (login, status, apply with network confirmation, dark / light mode) in a browser against a local mock of the JSON API | **not yet** (nothing of 4.23 has run on the board yet) |

The transmit delay compensation of basic mode responses was measured on the board (7.4) but not checked with a basic mode client (chrony uses the interleaved mode).

## 12. Known limitations and open points

- The absolute accuracy against UTC is not verified; an independent reference would be needed.
- Systematic uncertainties: interrupt latency (now 62.5 ns resolution; ±0.5 µs before the follow-up), position of the PTP read inside the bracket (affects the software measurement of the PTP clock in both modes; removed by the loopback, 4.21), network path asymmetry (2.7 µs difference between the IPv4 and the IPv6 path of one board in an earlier run; 2.8 µs between the two new boards in 7.5, most likely the longer network path of the test board).
- The measurements are limited by the client: kernel RX time stamps and the OpenVSwitch bridge on the DiskStation add about 18.5 µs noise on the return path (7.5).
- The PTP clock rate against esp_timer: earlier comparisons showed +0.37 / -0.45 ppm, within what the method resolved. With `PTP_FREE_RUNNING 1` the servo measures the rate of the PTP clock itself, so this no longer matters; with `PTP_FREE_RUNNING 0` the error of T2 would grow by up to 0.4 µs over the second.
- `PTP_FREE_RUNNING 0` only: a frame time stamped shortly before a PTP write but converted after it uses the new compensation (error up to the drift of one second, about 33 µs with the -33 ppm oscillator of the test board; probability about 1 in 10000 samples, chrony filters it). Late PTP writes: about 15 per hour with a deviation of about -5.7 µs (7.4).
- The free-running servo and the loopback run on all four boards (loopback on GPIO 23); the board's own error is below what the DiskStation can resolve (about 0.19 µs). The PLL clock source (4.22) is not yet verified on the board.
- `esp_timer_impl_get_counter_reg()` is an internal function of esp_timer. If a later IDF version renames it, the build fails at the link step; then set `FINE_LOCAL_TIMEBASE 0`.
- The calibrations assume the board is warm and the PPS locked. A run that does not finish starts again at the next boot.
- After a PPS loss the server goes to stratum 16 after 3.5 s (no holdover at stratum 1), although the PTP clock now keeps the learned rate (simulated: below 1 µs in 5 minutes with a constant crystal).
- Leap second warnings (LI) from the GNSS are not passed on.
- If the GNSS receiver stops answering at run time, the recovery can end in `halt_with_display()`; the sync task then waits forever (stratum 16, no reboot).
- Web admin (4.23): not tested on the board yet. Points to check first: TLS handshake time and heap with two browsers, the ND6 gateway hook with a static IPv6 address and SLAAC off, the 120 s rollback, the one-shot restart with changed pins, the button recovery.
- The certificate is self-signed: every browser warns, the fingerprint has to be compared by hand. The certificate names the FQDN only (no IP addresses, they can change).
- The IPv6 prefix length is stored and shown, but lwIP decides on-link with the first 64 bits; prefixes shorter than 64 are refused.
- Changing the time zone at run time calls `setenv()` / `tzset()` while other tasks may call `localtime_r()`; newlib guards both, a reading during the switch may use the old zone once.
- The settings blob has a layout version: after a firmware update that changes `web_settings_t` the saved settings are ignored (compiled defaults) until they are saved again.
- PreferIPvX, MACAddress, OTE and the MQTT reporting details stay compile-time settings.

## 13. Recommendations that are not in the code

NVS encryption (not enabled, decision of 2026-10-21; it burns an eFuse key block permanently):

1. Build once with `CONFIG_NVS_ENCRYPTION=y` and `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y` (key protection with the HMAC peripheral), `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID` = a free key block (0 to 5).
2. At the first start the firmware generates the HMAC key and burns it into that eFuse block (purpose HMAC_UP); this cannot be undone and the block is lost for other uses. Without flash encryption the eFuse key is still readable with JTAG unless read protection is set.
3. The NVS partition is then encrypted (XTS-AES); existing plain NVS data is not readable any more: erase the `nvs` partition before (all calibrations, the TLS key and the web settings are created again).
4. Use it together with flash encryption and Secure Boot V2 for real protection against an attacker with physical access; otherwise the firmware with the compiled secrets stays readable.

- Per-client rate limiting (like chrony's `ratelimit`) to protect the other clients from a single flooding host.
- Holdover at stratum 1 after a PPS loss: not wanted (decision of 2026-10-20); the server goes to stratum 16 after 3.5 s as before.
- OTE: Secure Boot V2 and flash encryption if it is enabled; MQTT over TLS.

## 14. Files and how to build

Current files of the changes:

| File | Content |
|---|---|
| `main/main.cpp` | the complete current program |
| `main/ESP32TimeServerSettings.h` | follow-up: `PTPPPSLoopbackPin` added (default -1) |
| `main/custom/ntp_auth.cpp` | PSA status handling fixed |
| `main/custom/ntp_cache.c`, `main/custom/ntp_cache.h` | client cache (unchanged since the earlier work) |
| `CMakeLists.txt` (root) | -O2 warning handling for third-party components |
| `sdkconfig.defaults`, `sdkconfig` | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` |
| `main/CHANGELOG.md` | this file |
| `main/web/web_settings.*` | web settings: defaults, NVS (saved, one-shot), validation (4.23) |
| `main/web/web_auth.*` | password hash, sessions, CSRF, login throttling, allow-list |
| `main/web/web_tls.*` | TLS key and self-signed certificate |
| `main/web/web_net.*` | network settings at run time, ND6 gateway hook |
| `main/web/web_server.*` | HTTPS server, TLS transport, JSON API |
| `main/web/www/index.html`, `app.css`, `app.js` | the page (embedded) |
| `main/custom/ntp_auth.*` | always compiled; keys and on/off at run time (lock-free) |
| `main/CMakeLists.txt`, `main/idf_component.yml`, `sdkconfig.defaults` | all features compiled in, web sources, cJSON, TLS / lwIP options |
| `main/ESP32TimeServerSettings.h`, `main/ESP32TimeServerSettingsSecrets.h` | note on the web admin, `WebAdminPassword` |
| `main/Sonnet_CHANGELOG.md`, `main/Review_2026-10-19_CHANGELOG.md` | the two source changelogs (kept for reference) |
| `main/Review_2026-10-19_main.cpp.patch` | diff of the review against the earlier `main.cpp` (does not contain the stack sizes set afterwards) |
| `main/Followup_2026-10-19_main.cpp.patch` | diff of the follow-up (fine timebase, free-running PTP clock, loopback) against the `main.cpp` before it |

Build and first start:

1. `idf.py build` (ESP-IDF 6.1). After the optimization change `idf.py fullclean` once if old objects are used. For the web admin (4.23) delete `sdkconfig` first, so that it is created again from `sdkconfig.defaults` (otherwise the stored values stay in effect and the build stops in `web_net.cpp`).
2. Flash. At the first start the interrupt latency (about 2 minutes) and the PTP write latency (about 50 minutes) are measured again because the fingerprints changed; until then the defaults are used.
3. Optional: `DEBUG_ENABLED 1` for a while to see the `PTP:` line (write error and transmit delay on this board), then back to 0.
4. Compare the chrony statistics with the previous firmware (`chronyc sourcestats`, `chronyc ntpdata`).
