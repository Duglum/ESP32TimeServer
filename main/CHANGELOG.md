# ESP32 Time Server: combined changelog and technical notes

This file combines `Sonnet_CHANGELOG.md` (precision and stability work done earlier) and `Review_2026-10-19_CHANGELOG.md` (review of 2026-10-19 and its follow-ups). Where the later review changed or replaced something from the earlier work, only the current state is described.

State described here: the files in the repository on 2026-10-19 after the review, including the stack sizes set by the user afterwards (section 4.15).

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
| 6 | PTP clock set from the PPS second every second, with write latency compensation, read-back and retry | T2 and hardware T3 are in the same time scale as the model clock |
| 7 | **PTP offset measured after every write** (mean of 16 bracketed read-backs) and removed from T2 / hardware T3 | write error 0.05 µs RMS instead of 0.5 µs RMS (simulation) |
| 8 | PTP write latency measured at run time and stored in NVS | no hard-coded board dependent value; with #7 no longer critical for accuracy |
| 9 | Interrupt latency of the PPS path measured with a software triggered capture and stored in NVS; test triggers keep away from real PPS edges | no oscilloscope needed, no wrong samples |
| 10 | Hardware TX time stamps for the interleaved mode | T3 of interleaved responses is the real transmit time |
| 11 | **Transmit delay compensation for basic mode T3** (running mean of hardware TX time - software T3) | basic clients (Windows, ntpd, phones) no longer see half the transmit delay as offset |
| 12 | Interleaved mode fixes: cache by client IP, lifetime 30 minutes, correct T3 caching | chrony runs in interleaved mode on IPv4 and IPv6 |
| 13 | Precision -20, root dispersion 2 | chrony sees the real delay and selects the server |
| 14 | **No clock step at the periodic GNSS check** when the clock already shows the right second; no stratum 16 window during the check | no disturbance every 10 minutes |
| 15 | **Robust second label** (label data only from after the PPS edge, confirmation of a 1 s difference, consistency check at the first sync) | no risk of a 1 s error from an old NMEA sentence or an early UBX poll |
| 16 | **Raw NTP path only answers well-formed unicast requests to the own address**; everything else goes to lwIP | no replies with broadcast, multicast or foreign source addresses |
| 17 | Stability fixes: seqlock fences, no lock/string work per NTP frame in the RX task, MQTT JSON overflow, `ntp_auth.cpp` status handling, LED/LCD status codes | see 4.13 |
| 18 | Unused code removed | see 4.14 |
| 19 | Compiler optimization -O2 instead of -Og, with the build fixes it needs | shorter and steadier NTP and PPS code paths |
| 20 | Logging convention: brief lines always, details with `DEBUG_ENABLED`; secret keys only with `DEBUG_ENABLED` | quiet console in normal operation |

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

## 3. Architecture

```
 PPS pin -> MCPWM capture (80 MHz hardware tick)  --ISR-->  pps_capture_callback
                                                            (tick value + esp_timer reading, queue)
                                                                  |
                                                                  v
 pps_discipline_task (once per second)
   0 spike rejection        interval to the previous edge vs. expected interval (±50 us)
   1 tick bookkeeping       unwrapped 32 bit ticks, seconds counter
   2 frequency (ppb)        sliding window of 16 s, from ticks only
   3 edge anchor            tick time + central value of (ISR time - tick time) - central interrupt latency
   4 phase of system clock  adjtime(-error), bracketed gettimeofday reads
   5 model clock publish    {second of the edge, edge in esp_timer ns, ppb}  (seqlock)
   6 PTP clock              clock_settime(second + time since the edge + write latency), read-back, retry
   7 PTP offset             16 bracketed read-backs: offset = PTP - model clock  (seqlock with the second)
                                                                  |
        +---------------------------------------------------------+---------------------+
        v                                                                                v
 NTP responses (raw path, hardware time stamps)                                  other consumers
   T2  = EMAC RX time stamp, mapped like the model clock:                        system clock (gettimeofday)
         el = ts - offset - second;  true = second + el - el * ppb
   T3  = model clock = second + (esp_timer - edge) - ppb * (esp_timer - edge)
         + running mean of the transmit delay (basic mode)
   T3 (interleaved) = hardware TX time stamp of the previous response, cached per client IP

 gnss_time_sync_task (first sync, then every 10 minutes)
   PPS edge -> label from data received after the edge (NMEA: buffer emptied at the edge; UBX: poll 300 ms after)
   first sync: two readings must advance by exactly the number of seconds between their edges -> settimeofday
   later: clock right -> nothing is stepped; 1 s difference -> confirmed by a second reading, then stepped
```

Time scales: the model clock and the PTP clock are both aligned to the PPS edge every second and both are compensated with the same measured frequency error; the measured offset removes the error of the individual PTP write. The system clock (`gettimeofday`) is only kept at the right second and is not used for NTP time stamps of the raw path.

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

- For every edge `d = ISR time - tick time` is calculated. It contains a constant offset between the tick counter and esp_timer, the interrupt latency and the rounding of esp_timer to whole microseconds.
- The anchor uses the central value of the last 64 values: the mean of the lowest 75 %. `edge_in_esp_time = edge ISR time - (d - central value) - central interrupt latency`. The interrupt jitter of the individual edge cancels out completely (the anchor is effectively tick time + central offset).
- The interrupt latency constant is measured with the same statistic (4.5), so the rounding of esp_timer and the shape of the jitter cancel out. A minimum (used before) has a bias that depends on the noise, up to ±0.35 µs.

### 4.5 Interrupt latency of the PPS path (measured, stored in NVS)

- `PPS_ISR_LATENCY_AUTO 1`: at the first start for a configuration a task measures the latency 30 s after the boot: 160 software triggered capture events (`mcpwm_capture_channel_trigger_soft_catch`), 500 ms apart, on the other core than the PPS interrupt. Per event the latency is the esp_timer reading in the callback minus the middle of two esp_timer readings around the trigger. Attempts with a gap above 5 µs between those readings are dropped, at least 32 valid attempts are needed.
- New in the review: test triggers are only issued between 50 ms and 950 ms after the last real PPS edge (the callback stores the low 32 bits of its time). Before, a real edge that arrived while a test was armed was taken as the test event (wrong sample) and was lost for the PPS task.
- Result = mean of the lowest 75 % of the samples ("central value"), accepted within 0.5 to 20 µs. It is applied immediately and stored in NVS with a fingerprint: IDF version, CPU frequency, MCPWM ISR setting (`CONFIG_MCPWM_ISR_IRAM_SAFE` or its IDF 6.1 name `CONFIG_MCPWM_ISR_CACHE_SAFE`), **compiler optimization level**, chip model, revision and cores, `PPS_ISR_LATENCY_CODE_VERSION`. With a matching fingerprint the stored value is read at every start and no measurement runs.
- `PPS_ISR_LATENCY_CODE_VERSION` is 3. Default until a value is measured: 2.6 µs.
- Accuracy about ±0.5 µs (resolution of esp_timer). Measurement on the development board: minimum 2.50 µs, median 2.50 µs, mean 2.82 µs, p90 3.50 µs.
- The test events are not passed on as PPS pulses.

### 4.6 Phase correction of the system clock

- The system clock is kept at the right second with `adjtime()` once per second. The phase error is taken from the edge anchor; `gettimeofday()` is read four times bracketed by two esp_timer readings and the reading with the smallest gap is used.
- Corrections of at least 1 µs are applied, the remainder is carried to the next second. Errors above 250 ms are not corrected by `adjtime`.
- The system clock is not drift compensated between the pulses (sawtooth of about 19 µs). It is not used for the NTP time stamps of the raw path.

### 4.7 Model clock (software T3, reference time)

- At every PPS the task publishes `{second of the edge, edge in esp_timer ns, ppb}` (seqlock, single writer). Time = `second + elapsed - elapsed × ppb / 1e9`.
- `get_current_time_in_ntp64_format()` uses the model. If the model is not valid (start, after a clock step) it falls back to the system clock with the same compensation. After one hour without a PPS edge the model is no longer used.
- Resolution: esp_timer microseconds (rounding noise 0.29 µs RMS).

### 4.8 PTP clock (T2 and hardware T3)

- `set_hardware_clock_from_pps()` writes the PTP clock every second: `PPS second + (esp_timer reading - edge in esp_timer time) + write latency`. The PTP clock is not set from the system clock in normal operation.
- Read-back: the PTP clock is read bracketed by two esp_timer readings. A deviation beyond 2 µs from the expected write latency causes a rewrite (at most 3 attempts). The warning `PTP set retry ...` is only printed with `DEBUG_ENABLED`.
- **Offset measurement (review, `PTP_OFFSET_MEASUREMENT 1`)**: directly after the write the PTP clock is read 16 times, each read bracketed by two esp_timer readings (brackets above 3 µs are dropped, at least 4 reads are needed). Offset = mean(PTP reading - model time at the middle of the bracket). Offsets beyond ±50 µs are not trusted (0 is used). The offset and the second of the edge are published together (seqlock).
- **Conversion of a hardware time stamp (replaces the earlier `ts - ppb × (ts - value written)`):** `el = ts - offset - second_of_edge`, `true = second_of_edge + el - el × ppb / 1e9`. This is the formula of the model clock, so T2, hardware T3 and software T3 are in the same time scale. The same ppb as for the model clock is used (the PTP clock runs from the same crystal).
- While the PTP clock is being written its compensation is switched off for a few microseconds. A write by `synchronize_hardware_clock()` (clock step, Ethernet restart) also switches it off until the next PPS.
- Systematic limit: the read is assumed in the middle of the bracket (the write latency calibration makes the same assumption).

### 4.9 PTP write latency: calibrated at run time

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

- `NTP_PRECISION_EXPONENT = -20` (0.95 µs, the resolution of esp_timer; was -13 = 122 µs).
- `NTP_ROOT_DISPERSION = 2` (2/65536 s = 30.5 µs; was 66 = 1.007 ms). The server only reports stratum 1 with active PPS.

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
| PPS discipline | 3135 |

Check the values with `CALCULATE_STACK_SIZES_ENABLED` after every change of a task.

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
- The secret NTP keys (symmetric-key authentication) are only printed with `DEBUG_ENABLED` (review). chrony accepts the ASCII keys of `ESP32TimeServerSettingsSecrets.h` directly (`1 SHA256 ASCII:<key>`).

## 5. Security and open ports

### 5.1 Open ports

With the current settings (OTE, MQTT off):

| Protocol | Port | Who | Remark |
|---|---|---|---|
| UDP | 123 | raw NTP path (hardware time stamps) and lwIP sockets (IPv4 `0.0.0.0`, IPv6 `::`, IPv6 link-local) | the sockets get what the raw path leaves to lwIP |
| UDP | 68 | lwIP DHCP client | only while DHCP is used |
| ICMP / ICMPv6 | - | lwIP | ping replies, IPv6 neighbour discovery, MLD; ARP |
| TCP | - | none | a SYN gets a RST |

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
- MQTT (if enabled): `mqtt://` sends the credentials in clear text; prefer `mqtts://`.

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
| Missing replies | **0** (largest gap between samples 5 s) | 8 (four gaps of 12 to 13 s) | none |
| Invalid samples | none | 4 (offset, delay and dispersion exactly 0) | none |

Results:

- Noise: the test board has about a third less noise in the direction that contains its receive time stamp (0.28 µs instead of 0.42 µs). The difference √(0.42² - 0.28²) = 0.31 µs equals the PTP write error measured on the board (318 ns RMS, 7.4), which the offset measurement removes. chrony's own figures show the same ratio (223 ns instead of 324 ns).
- 10-minute disturbance: the 8 missing replies and the 4 invalid samples of 1.ntp happened at 14:14:39, 14:34:42, 14:54:43 and 15:24:46 (UTC), i.e. at periodic GNSS checks of the Sonnet firmware (stratum 16 during the check, then a clock step with an uncompensated PTP clock). The test board has none of these.
- Lost replies: none on the test board in 2.7 hours. The 3.6 % missing replies seen earlier in `chronyc ntpdata` came from the reboots during the work, not from the new receive path filter.
- Agreement of the two new boards: the test board is a constant +2.77 µs later than 1.ntp in the direction to the board (20 minute medians between +2.74 and +2.80 µs). Its shortest round trip is 6.3 µs longer, i.e. it sits on a slightly longer network path; with a symmetric path half of it (3.15 µs) explains the difference, leaving about -0.4 µs. chrony's asymmetry corrected offsets differ by 0.34 µs. The boards agree to within about 0.4 µs. To confirm that the 2.8 µs belong to the path and not to the board, swap the switch ports or cables of the two boards.
- Old firmware: 56 to 60 µs away from the new boards, about 10 µs noise.
- Limiting factor now: the receive side of the DiskStation (18.5 µs noise on the return path). Hardware RX time stamps on the DiskStation would remove most of it and raise the share of good samples well above 50 %.

## 8. Settings reference

| Setting | Place | Value | Meaning |
|---|---|---|---|
| `PPS_Discipline_Task_Stack_Size` and the other stacks | `main.cpp`, top | see 4.15 | task stacks |
| `CALCULATE_STACK_SIZES_ENABLED` | `main.cpp`, top | 0 | 1 = report the needed stack sizes |
| `NTP_PRECISION_EXPONENT` | `main.cpp`, top | -20 | advertised precision |
| `NTP_ROOT_DISPERSION` | `main.cpp`, top | 2 | advertised root dispersion, 1/65536 s per unit |
| `PPS_ISR_LATENCY_DEFAULT_US` | compensation block | 2.6 | used until a value is measured |
| `PTP_LATENCY_AUTO` | compensation block | 1 | calibrate and store the PTP write latency; 0 = always the default |
| `PTP_LATENCY_FORCE_RECALIBRATION` | compensation block | 0 | 1 = calibrate again at the next start |
| `PTP_WRITE_LATENCY_DEFAULT_NS` | compensation block | 3850 | used until a value is stored or calibrated |
| `PTP_OFFSET_MEASUREMENT` | PTP compensation block | 1 | measure the PTP offset after every write; 0 = assume the write was exact |
| `NTP_HW_TX_TIMESTAMPS` | before the transmit helper | 1 | 0 = send without hardware TX time stamps |
| `NTP_BASIC_T3_TX_COMPENSATION` | after the transmit helper | 1 | 0 = basic T3 without transmit delay compensation |
| `PPS_ISR_LATENCY_AUTO` | before the PPS callback | 1 | measure and store the interrupt latency; 0 = always the default |
| `PPS_ISR_LATENCY_FORCE_REMEASURE` | before the PPS callback | 0 | 1 = measure again at the next start |
| `PPS_ISR_LATENCY_CODE_VERSION` | before the PPS callback | 3 | increase when the PPS interrupt path or the statistic changes |
| `PTP_LATENCY_CODE_VERSION` | PTP calibration block | 1 | increase when the way the PTP clock is written changes |
| `spikeLimitUs`, `maxSpikes` | PPS task | 50 µs, 3 | spike rejection |
| `anchorWindow` | PPS task | 64 | edges for the central value of the anchor |
| `freqCap`, `freqMinSeconds` | PPS task | 17, 4 | frequency window (16 s), first estimate after 4 s |
| `labelDeadlineUs`, `ubxPollDelayMs` | `acquire_sync_candidate` | 950 ms, 300 ms | label data window after the PPS edge |
| `NTP_CACHE_MATCH_PORT` | `ntp_cache.c` | 0 | 1 = also compare the client source port |
| `CACHE_TIMEOUT_MS_IPV4/IPV6` | `ntp_cache.h` | 1800000 | lifetime of a cache entry |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `sdkconfig.defaults`, `sdkconfig` | y | -O2 |

## 9. Data stored in NVS

| Namespace | Key | Content |
|---|---|---|
| `isr_latency` | `ns` (int32), `fp` (uint32) | interrupt latency in ns, fingerprint of the configuration |
| `ptp_latency` | `ns` (int32), `fp` (uint32) | PTP write latency in ns, fingerprint of the configuration |

A value is only used if its fingerprint matches and it is within 0.5 to 20 µs; otherwise it is measured again. Other namespaces (`gnss_state`, `restart_evt`) are unchanged from the original project.

## 10. Console messages

Always printed (brief):

- `PPS ISR latency: using the stored value X us` / `no stored value for this configuration, using X us until it is measured`
- `PPS ISR latency measurement started ...`, the statistics line, `central value (mean of the lowest 75 %) = X us`, `X us applied (was Y us) and stored in NVS`
- `PTP write latency: N ns read from NVS (calibration skipped)` / `no stored value for this configuration, using the default 3850 ns until the calibration has finished ...`
- `PTP write latency calibration: PPS locked, warm-up started (10 min)`, `warm-up done, measuring (about 30 to 40 min)`, `finished: N ns (default was D ns), stored in NVS`
- Failures: calibration not stable, result outside the range, NVS errors, out of memory, task could not be started, `Hardware TX time stamping disabled after repeated errors`.

Only with `DEBUG_ENABLED`:

- progress of the PTP calibration every 2 minutes, statistics of every block, spread of the last three blocks, PPS lock lost during the calibration
- `PTP set retry n: write deviation X ns`
- `PPS spike ignored (n of 3): interval X s`
- every 16 s: `PPS: ppm=..., raw_phase, resid, jitter_excess, d_central, gap` and `PTP: offset after write <n> ns (<k> reads) | basic T3 TX delay v4 <d> ns (<samples>) v6 <d> ns (<samples>)`
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

The transmit delay compensation of basic mode responses was measured on the board (7.4) but not checked with a basic mode client (chrony uses the interleaved mode).

## 12. Known limitations and open points

- The absolute accuracy against UTC is not verified; an independent reference would be needed.
- Systematic uncertainties: interrupt latency ±0.5 µs (esp_timer resolution), position of the PTP read inside the bracket (affects offset measurement and calibration alike), network path asymmetry (2.7 µs difference between the IPv4 and the IPv6 path of one board in an earlier run; 2.8 µs between the two new boards in 7.5, most likely the longer network path of the test board).
- The measurements are limited by the client: kernel RX time stamps and the OpenVSwitch bridge on the DiskStation add about 18.5 µs noise on the return path (7.5).
- The PTP clock rate against esp_timer: earlier comparisons showed +0.37 / -0.45 ppm, within what the method resolved. If real, the error of T2 grows by up to 0.4 µs over the second; the offset measurement removes it at every write but not within the second.
- A frame time stamped shortly before a PTP write but converted after it uses the new compensation (error up to the drift of one second, about 33 µs with the -33 ppm oscillator of the test board; probability about 1 in 10000 samples, chrony filters it).
- Late PTP writes: about 15 per hour with a deviation of about -5.7 µs (7.4), cause probably an interrupt between the clock reading and the write; the retry and the offset measurement handle them.
- The calibrations assume the board is warm and the PPS locked. A run that does not finish starts again at the next boot.
- After a PPS loss the server goes to stratum 16 after 3.5 s (no holdover at stratum 1).
- Leap second warnings (LI) from the GNSS are not passed on.
- If the GNSS receiver stops answering at run time, the recovery can end in `halt_with_display()`; the sync task then waits forever (stratum 16, no reboot).

## 13. Recommendations that are not in the code

- Per-client rate limiting (like chrony's `ratelimit`) to protect the other clients from a single flooding host.
- Steering the PTP clock frequency with `clock_adjtime` (available in IDF 6.1, `CONFIG_ETH_CLOCK_ADJTIME_*`) instead of writing it every second; removes the rare write boundary error. Needs testing on the board.
- Holdover at stratum 1 for a few minutes with growing root dispersion after a PPS loss.
- OTE: Secure Boot V2 and flash encryption if it is enabled; MQTT over TLS.

## 14. Files and how to build

Current files of the changes:

| File | Content |
|---|---|
| `main/main.cpp` | the complete current program |
| `main/custom/ntp_auth.cpp` | PSA status handling fixed |
| `main/custom/ntp_cache.c`, `main/custom/ntp_cache.h` | client cache (unchanged since the earlier work) |
| `CMakeLists.txt` (root) | -O2 warning handling for third-party components |
| `sdkconfig.defaults`, `sdkconfig` | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` |
| `main/CHANGELOG.md` | this file |
| `main/Sonnet_CHANGELOG.md`, `main/Review_2026-10-19_CHANGELOG.md` | the two source changelogs (kept for reference) |
| `main/Review_2026-10-19_main.cpp.patch` | diff of the review against the earlier `main.cpp` (does not contain the stack sizes set afterwards) |

Build and first start:

1. `idf.py build` (ESP-IDF 6.1). After the optimization change `idf.py fullclean` once if old objects are used.
2. Flash. At the first start the interrupt latency (about 2 minutes) and the PTP write latency (about 50 minutes) are measured again because the fingerprints changed; until then the defaults are used.
3. Optional: `DEBUG_ENABLED 1` for a while to see the `PTP:` line (write error and transmit delay on this board), then back to 0.
4. Compare the chrony statistics with the previous firmware (`chronyc sourcestats`, `chronyc ntpdata`).
