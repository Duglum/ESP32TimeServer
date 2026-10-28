# ESP32 Time Server: combined changelog and technical notes

This file combines `Sonnet_CHANGELOG.md` (precision and stability work done earlier) and `Review_2026-10-19_CHANGELOG.md` (review of 2026-10-19 and its follow-ups). Where the later review changed or replaced something from the earlier work, only the current state is described.

State described here: the files in the repository on 2026-10-19 after the review, including the stack sizes set by the user afterwards (section 4.15), and the follow-up of the same evening: fine local timebase and free-running, frequency disciplined PTP clock with optional PPS loopback (sections 4.19 to 4.21, results in 7.6), of 2026-10-20: PTP clock from the 80 MHz PLL, root dispersion 1 (section 4.22), and of 2026-10-21: HTTPS web admin page (section 4.23), Ethernet recovery, ESP-IDF Ethernet driver and OTE over HTTPS (section 4.24), of 2026-10-22 / 23: PPS holdover (section 4.25), the per-client NTP rate limit (section 4.26) and scripted firmware uploads with the web admin password (section 4.27).

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
| 25 | **Web admin page** (HTTPS, TCP 443, 2026-10-21): network, GNSS / PPS, LCD, time zone, LED, MQTT and NTP authentication settings; Apply / Save Settings (NVS) / Reboot / Factory reset; LCD, LED, MQTT and NTP authentication always compiled in and switched at run time | common settings without rebuilding; per-device TLS key, salted password hash, sessions with CSRF protection, allow-list |
| 26 | **Ethernet recovery hardening, ESP-IDF Ethernet driver, OTE over HTTPS** (2026-10-21, from release 3.0.5) | recovery under the time mutex with a 45 s watchdog, PHY link read, more triggers; firmware upload on the page or by script over TLS, image checked before it is written |
| 27 | **Per-client NTP rate limit** (2026-10-23): 20 requests per second and client IP address by default, setting in the Network panel | one client cannot occupy the reply path; requests over the limit are dropped silently, no effect on the time stamps |
| 28 | **Scripted firmware uploads with the web admin password** (2026-10-23): the settings OTE password and mDNS host name are gone; `tools/ote_upload.py` finds the device by its A / AAAA records (unicast DNS) | one password for the page and the script (the same compiled `WebAdminPassword` on many devices allows bulk updates); no multicast name resolution in the script |
| 29 | **Quick wins of the review of 2026-10-23** (`REVIEW_2026-10-23.md`): RMC checksum, no software T3 in interleaved replies, MQTT task priority 5, no NVS erase by `initArduino()`, no UBX commands to NMEA receivers, rollover-safe PTP reading in the holdover, interrupt latency default 2.5 µs | no wrong labels from corrupted sentences, no queue-delayed T3 in interleaved replies, MQTT cannot delay NTP replies, password and TLS key survive NVS errors |
| 30 | **Exchange map for the interleaved mode** (2026-10-23, review finding S-02): the recent exchanges of all clients are found by their T2 (the origin timestamp the client sends back), besides the latest exchange of each address | a spoofed request with a client's address or a second client behind the same NAT address no longer switches the client to basic mode; time stamps unchanged |
| 31 | **Own rate limit bucket for requests with a T2 cookie** (2026-10-25, review finding S-17): requests that refer to an earlier exchange of their address are counted apart from the other requests of that address | a host sending with a client's address can no longer exhaust the limit of the client's interleaved requests; time stamps unchanged |
| 32 | **Re-lock after a PPS holdover without a phase step** (2026-10-25, review finding Z-06): the PTP mapping continues from the holdover mapping and the accumulated error decays with 16 s (at most 5 µs/s); no stratum 1 while the PTP time stamps are not mapped | no jump of T2 / T3 when the PPS returns, clients see a short ramp; steps above 100 µs stay steps; normal operation unchanged |
| 33 | **GNSS validity after a cold start** (2026-10-26, review finding Z-04): RMC mode indicator, u-blox `fullyResolved` / `confirmedTime`, web setting *Minimum GNSS fix age before stratum 1* (default off, 750 s recommended) | no stratum 1 with a solution of a dead reckoning, manual or simulator mode, nor with a UTC second that may still be off after a cold start |
| 34 | **Clean-ups of the review** (2026-10-26, findings E-11, E-13, E-16): PPS queues created before the capture starts, outdated comments corrected, one address lookup per NTP request | no handle of the PPS callback is written while it may run; `ntp_cache_update()` no longer scans the 128 records a second time; time stamps unchanged |
| 35 | **One hardware NTP handler for IPv4 and IPv6** (2026-10-26, review finding E-12): `process_hardware_ntp_request()` with a family flag instead of two copies of about 200 lines | one place for every later change of the reply path; the frames on the wire and the time stamps are unchanged |
| 36 | **Integer absolute times in the PPS task and the PTP functions** (2026-10-26, review finding Z-13): the PPS edge as whole ns plus a fraction, doubles only for differences | no rounding error that grows with the uptime (before: the edge up to 23 ns off after a year, 87 ns after three years) |
| 37 | **Native NMEA reader** (2026-10-26, review option b, stage 1): the NMEA protocol reads the receiver with the ESP-IDF UART driver in its own task instead of the Arduino serial port; optional GGA (fix quality, satellites in use), web setting *Minimum satellites in use before stratum 1* (default off), RMC phase after the PPS on the status page | 2 kB receive buffer instead of 256 B, continuous validity, a sentence that started before the PPS edge never labels it; nothing is sent to the receiver |
| 38 | **Native UBX protocol for u-blox M10** (2026-10-26, review option b, stage 2): one UART reader for NMEA and UBX, configuration with CFG-VALSET / CFG-VALGET only, NAV-PVT / NAV-TIMEUTC / TIM-TP sent by the receiver, `validUTC` required, qErr of the time pulse in the PTP servo measurement with a learned sign; the SparkFun library is no longer used | label from the first NAV-PVT after the PPS edge instead of a poll; no UTC second before the UTC parameters are known; with qErr a few ns less noise of the PTP clock against the pulse (simulated: hanging bridges 5.9 to 2.2 ns RMS); NMEA receivers unchanged |
| 39 | **Arduino core and SparkFun library removed** (2026-10-26, review option b, stage 3): plain ESP-IDF project; `millis()` replaced by esp_timer, no `initArduino()` / `Serial.begin()`, the SparkFun submodule and its component wrapper deleted | 8 instead of 25 managed components, image 105 kB smaller than with stage 1 (35 kB of it in this stage), no third-party warnings in the build, no longer tied to ESP-IDF below 6.2 by arduino-esp32 |
| 40 | **Settings panels collapsed** (2026-10-26): Network, GNSS and PPS, LCD and time zone, RGB LED, MQTT, NTP authentication, Firmware Update and Security open with an arrow, like the GPIO pin areas | a short page; the heading row shows how many values inside differ from the default, an input error opens its panel |
| 41 | **`NTP_CACHE_MATCH_PORT` removed** (2026-10-26): the compile option to tell clients apart by their source port, and the stored port | nothing changes in operation (the option was 0); the cache API has no port parameter any more, 1 kB less RAM |
| 42 | **PTP PPS loopback capture on the core of the PPS interrupt** (2026-10-26): the interrupt of the loopback capture channel is installed on the core that owns the PPS capture interrupt | the loopback no longer fails at boot with `install interrupt service for cap channel failed` (`ESP_ERR_INVALID_STATE`) and falls back to the software measurement when the Ethernet setup task ran on the other core |
| 43 | **Diagnostics of the time keeping** (2026-10-26, review 4.5): collapsed *Diagnostics* panel on the page (servo, ADEV / TDEV of the PPS, PPS interrupt, NTP paths, GNSS receiver), time quality in the MQTT report | the quality of the time keeping can be judged without an oscilloscope; nothing in the time path changes |
| 44 | **PPS not followed without valid GNSS time** (2026-10-26, review Z-05): edges are ignored while the GNSS time is not valid, the same holdover as for a lost PPS | a time pulse that keeps running without fix (u-blox) no longer pulls the PTP clock; the replies get the PPS holdover (stratum 1 for the holdover time) instead of stratum 16 at once |
| 45 | **u-blox time pulse and antenna** (2026-10-26, review G-03): CFG-TP of time pulse 1 read and logged at the setup (warnings for settings that do not fit the PPS input), MON-RF polled every 10 s (antenna, jamming, AGC on the status page; noise and CW indicator in the diagnostics) | a wrong time pulse configuration or an antenna fault shows up without an oscilloscope; nothing is written to the receiver |
| 46 | **lwIP and UART interrupt in IRAM** (2026-10-26, review E-04): `CONFIG_LWIP_IRAM_OPTIMIZATION`, `CONFIG_UART_ISR_IN_IRAM` | the lwIP RX/TX path runs without flash cache misses; the GNSS UART keeps receiving during flash writes (NVS, OTE); DIRAM about +17 kB |
| 47 | **Core dump of a crash** (2026-10-26, review E-07): written to a new `coredump` partition (128 kB at the end of the flash), logged at the next start, shown in the diagnostics with download and delete | a crash in the field can be analysed afterwards (`idf.py coredump-info`); **the partition table has to be flashed once over USB** |
| 48 | **Rollback of a failed firmware update** (2026-10-26, review E-06): an image written by an OTE update is kept only after a self check (Ethernet, NTP server, web admin, GNSS data for 60 s); otherwise, after a crash, a reset or 15 min, the previous image starts again | a broken update no longer leaves the server down until someone flashes it over USB; **the bootloader has to be flashed once over USB** |
| 49 | **`driver` no longer required by main** (2026-10-26, review E-09) | the main component names the `esp_driver_*` components it uses; same image |
| 50 | **Last holdover in the diagnostics** (2026-10-26): duration and the time error of the served time when the PPS came back, measured by the board against the PPS | the holdover quality is visible with ns resolution; chrony on the DiskStation cannot resolve it |
| 51 | **No clock step after a GNSS loss while the clocks coast** (2026-10-26, board finding) | after a loss of GNSS and PPS of more than 30 s the PTP clock was set from the system clock (us) instead of re-locking without a phase step; the holdover row of the diagnostics never ended |
| 52 | **RMC phase only within 1 s of the PPS edge** (2026-10-26, board finding) | an RMC after a missing PPS edge no longer counts as about 1 s late (status page maximum, diagnostics histogram) |
| 53 | **Robust check of the 80 MHz PLL source of the PTP clock** (2026-10-26, board finding) | a reading disturbed at the start no longer drops the 13 ns time stamps for the whole run; the warning names the failed step |
| 54 | **qErr flagged invalid by the receiver shown as such** (2026-10-26, board finding with the M10) | the status line no longer says *TIM-TP missing*; the diagnostics show the raw qErr and flags of the latest TIM-TP |
| 55 | **qErr and MON-RF lines moved to the diagnostics** (2026-10-26, request) | the status shows only protocol, baud rate, model and satellites of the GNSS receiver |
| 56 | **Board result: the M10 gives no qErr** (2026-10-26, documentation only) | the qErr correction is not available with this module; the code stays for a receiver that gives a valid qErr |
| 57 | **Diagnostics of the basic mode TX delay** (2026-10-27, request) | minimum, maximum and the mean per 100 ms of the second show what delays the transmission; the time path is unchanged |
| 58 | **Board result: the basic mode TX path is not slower** (2026-10-28, documentation only) | minimum 13.9 us as in 7.4; the higher mean comes from occasional delays without a relation to the PPS or the GNSS data; the open point is closed |
| 59 | **Root dispersion growth in the holdover 15 ppm by default** (2026-10-28, review Z-22) | the advertised uncertainty after a PPS loss grows by 4.5 ms in 5 minutes instead of 30 ms, so clients keep the server longer; saved settings keep their value |
| 60 | **Stack reserve of the tasks and the heap in the diagnostics** (2026-10-28, review E-14) | the release build shows how close each task came to its stack size and how low the internal heap went since the start, without a debug build |
| 61 | **MQTT reports after 49.7 days of uptime** (2026-10-28, code review) | the report schedule no longer sends reports back to back when the FreeRTOS tick count wraps |

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
- `PPS_ISR_LATENCY_CODE_VERSION` is 4 (3 with `FINE_LOCAL_TIMEBASE 0`): the value stored with version 3 was measured in whole microseconds, so it is measured again once (about 2 minutes after the first start). Default until a value is measured: 2.5 µs (2.6 µs until 4.28).
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

- Cache key: client IP only. chrony uses a new source port for every request. Correctness is still protected by the comparison `origin timestamp == stored T2`. (The option `NTP_CACHE_MATCH_PORT` to compare the source port as well was removed on 2026-10-26, 4.40.)
- Lifetime 30 minutes (chrony polls up to 1024 s). 128 clients per address family.
- After a response the cache stores the transmit time of this response: the hardware TX time stamp, otherwise the (compensated) software time. Before, the content of field 40 was stored, which is the previous T3 in an interleaved response, so the cache returned a stale T3 from the third exchange on.
- Interleaved response: origin = receive timestamp of the request, T3 = cached hardware TX time of the previous response.
- Since 4.29 the recent exchanges of all clients are also kept in an exchange map found by T2, so spoofed requests and NAT clients no longer cost a client its interleaved mode.

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
| Ethernet transport recovery | 2405 (3072 since 4.24, also for the recovery worker) |
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

- PPS lost (3.5 s): `ptp_servo_holdover()` sets the command to the pure rate (no offset correction) and continues the mapping from the current offset. The mapping is used for up to one hour, like the model clock. When the PPS returns, the filter starts again with the kept rate; since 4.31 the mapping continues from the holdover mapping and the error accumulated in the holdover decays with 16 s instead of appearing as a step.
- Clock set from outside (`synchronize_hardware_clock()`, Ethernet recovery): an epoch counter starts the filter again; after a recovery the EMAC runs with the base addend again (`esp_eth_mac_ptp_enable()`), which the servo is told. Until the next measurement the raw PTP clock is used (since 4.31 without stratum 1).
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

`WEB_ADMIN_ENABLED` (`ESP32TimeServerSettings.h`, after `DEBUG_ENABLED`, default 1) switches the page off completely: with 0 no web server task, no TLS key, no password hash, nothing listens on TCP 443, and only the settings of the settings files are used (the previous behaviour). Settings saved on the page stay in NVS untouched and are used again when the switch is set back to 1; a pending one-shot set is erased. The page code is still compiled but never called (the linker removes most of it: the firmware is about 160 kB smaller). The console prints `Web admin page support: Disabled (compiled settings only)` and `Settings: compiled settings (web admin disabled, WEB_ADMIN_ENABLED 0)`. The button recovery at the start still works.

**Settings on the page**

| Group | Settings | Takes effect |
|---|---|---|
| Network | FQDN (host name and domain, one field), IPv4 DHCP or address/prefix and gateway (the address received by DHCP is shown), IPv6 SLAAC on/off (the addresses in use are shown), one additional static IPv6 address/prefix with gateway, up to 3 DNS servers (one per line, IPv4 or IPv6; empty = the servers received by DHCPv4 and IPv6 router advertisements, both shown in the status), MAC address (`MACAddress`; while none is set the field is empty and shows the hardware MAC as its grey placeholder; the hardware MAC is always shown below it and in the status) | at once, with confirmation (FQDN: restart; MAC: restart, then confirmation) |
| GNSS / PPS | Protocol ("NMEA Standard Protocol", which needs RMC sentences (any talker, e.g. `$GNRMC` or `$GPRMC`; the page says "needs RMC sentence"), or "U-Blox UBX Protocol", default `UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED`; with UBX the dynamic platform model of the receiver is set to Stationary, see below), TX, RX and PPS pin, baud rate (fixed list 4800 to 921600; greyed out with UBX, which finds and configures the baud rate itself), PTP PPS loopback pin (empty = off; the page notes that the pin has to be empty, i.e. a free GPIO with nothing connected that is driven as an output, and that it needs an ESP32-P4 of chip revision v3.0 or higher; the revision of the board is shown) | restart |
| LCD | on/off (restart), top line title, 24-hour clock, hour with leading zero, show time zone, POSIX time zone | at once |
| RGB LED | none / KY-016 / WS2812 and their pins (restart), brightness and the colour of each state (start-up, normal, time sync, warning, alert, critical, button pressed; preset or `#RRGGBB`, defaults `LED_startup` ... `LED_button_push`) (at once) | see left |
| MQTT | on/off, broker URI (`mqtt://` or `mqtts://`, host, port), user name, password, topic (`MQTTTopic`), QoS (`MQTT_QOS`), retain (`MQTTBrokerRetain`), reporting period (`MQTTReportingPeriod`, 30 to 86400 s), report contents (`MQTT_CLIENT_REPORTING_ENABLED`, `MQTT_MEMORY_REPORTING_ENABLED`, `MQTT_HISTORICAL_REPORTING_ENABLED`, `MQTT_DISTINGUISH_AUTHENTICATED_REPORTING_ENABLED`), client limit with the TF card queue (`MQTT_TF_Client_Limit`, 50 to 1000), TF card pins (`TFCardCommandPin` ... `TFCardData3Pin`, with the warning not to change them on the Waveshare ESP32-P4-ETH, -POE-ETH, -WIFI-ETH and -WIFI-POE-ETH) | restart |
| NTP authentication | on/off, up to 8 keys (key ID 1 to 65535 and key of 32 letters or digits, `[0-9A-Za-z]{32}`) in a table with "Add key", "Generate" (random key) and "Remove", or the compiled keys | at once |
| Over The Ethernet updates | on/off (`OTE_UPDATES_ENABLED`); shown together with the upload in the card *Firmware Update* below the buttons (4.24; the field stays part of the settings form through `form="settings-form"`, so Apply and Save Settings include it). `tools/ote_upload.py` uses the web admin password (4.27) | restart |
| Security | web admin password, allow-list of source networks | at once, stored at once |

Every field shows its compiled default; fields that differ from it are marked. Fields that need a restart carry a "restart" badge.

**Apply / Save / Factory reset**

- **Apply**: the values are in effect at once and are not stored; a restart returns to the saved state.
  - Live settings (time zone, LCD text options, LED brightness, NTP authentication) are switched immediately. The NTP key set (at most `NTP_AUTH_MAX_KEYS` = 8, `ntp_auth_configure()`) is replaced lock-free (seqlock in `ntp_auth.cpp`, the NTP path copies the key it needs and never waits).
  - Network settings (IPv4, IPv6, DNS) are applied at once and have to be **confirmed within 120 s** from the page. With a new static address the confirmation is only accepted over that address (the page lists the URLs; the session cookie is bound to the host name, so the user logs in again there). Without confirmation the previous network settings come back automatically.
  - Settings that need a restart (FQDN, GPIO pins, GNSS baud rate, LED type, LCD on/off, MQTT): Apply writes them as a **one-shot set** (NVS `webcfg/oneshot`) and restarts once. The one-shot set is erased at the start that uses it, before it is used: if it makes the device crash, the next start is back on the saved settings. If the one-shot set also changed the network, the confirmation runs after the start; without it the device restarts with the saved settings.
- **Save Settings** (button): stores the form values in NVS (flash) (`webcfg/saved`, versioned blob with CRC); they are loaded at every start. Network values are only saved if they are the ones in use (apply and confirm them first), so a typo cannot lock the device out at the next start.
- **Reboot**: restarts the device (controlled restart with the MQTT notification); applied but unsaved settings are discarded.
- **Factory reset** (needs the current password): erases `webcfg` (saved and one-shot settings) and `webauth` (password, allow-list), keeps the TLS identity (`webtls`) and the calibrations (`isr_latency`, `ptp_latency`, `gnss_state`), then restarts with the compiled settings and the compiled password.
- Order at the start: compiled defaults, then the saved settings (if valid for this firmware), then a one-shot set. A saved set from another firmware version (layout change) or one that fails the validation is ignored with a warning.

**TLS**

- Per-device ECDSA P-256 key, generated on the device at the first start (PSA, hardware RNG), and a self-signed certificate (CN and subjectAltName = FQDN, validity 2026-01-01 to 2099-12-31 because the time may not be known yet). Both in NVS (`webtls`). No private key is compiled into the firmware. A changed FQDN issues a new certificate with the same key.
- The SHA-256 fingerprint of the certificate is printed at every start (`Web admin certificate SHA-256 fingerprint: ...`) and shown on the page.
- TLS 1.2 and 1.3 only (mbedTLS 4 has nothing older), cipher suites TLS 1.3 AES-GCM and TLS 1.2 ECDHE-ECDSA with AES-GCM, no renegotiation, no TLS 1.3 PSK modes, handshake timeout 5 s.
- No HSTS: with a self-signed certificate it would prevent clicking through the browser warning.
- The transport is set up in `web_server.cpp` on `esp_http_server` with `esp-tls` (instead of `esp_https_server`, which does the TLS handshake before the application can look at the connection): the **allow-list is checked before the TLS handshake**, rejected sources cost no TLS work.

**Login, session, CSRF**

- Login form; HTTP Basic authentication only for the scripted firmware upload (4.27). Password only stored as salted PBKDF2-HMAC-SHA256 (10000 iterations, 16 byte salt, about 0.3 s per check; the iteration count is stored with the hash). Comparison in constant time. The compiled `WebAdminPassword` ("password" by default, `ESP32TimeServerSettingsSecrets.h`) is hashed with a random salt at every start and checked the same way.
- While the factory password is in use: red banner on the page and a console warning at every start.
- Session: random 256-bit token in the cookie `__Host-sid` (`Secure; HttpOnly; SameSite=Strict; Path=/`), only its SHA-256 is kept on the device; 15 min idle timeout, 12 h maximum, at most 4 sessions, logout button.
- CSRF: SameSite=Strict, a per-session token in the `X-CSRF-Token` header, the `Origin` header must equal `https://` + `Host`, `Sec-Fetch-Site` (if sent) must be `same-origin`, and the body must be `application/json` on every state-changing request.
- Login throttling per source address (after 5 failures 30 s, doubling up to 15 min; forgotten after an hour without failures) and global (20 failures in a minute block all logins for 60 s, doubling up to 15 min). It only affects the web admin, never NTP. Checks of the current password (password change, factory reset) count as well.
- Password change and factory reset need the current password. A new password (12 to 128 characters, no control characters) is stored at once and closes all other sessions.
- Secrets (MQTT password, admin password) are never sent to the browser and never logged; the page shows "set / not set". An empty field keeps the stored value, a separate check box clears it (MQTT password).
- Exception (on request, 2026-10-21): the NTP keys are shown in clear text on the page, since they have to be copied to the clients anyway. Without keys of the web settings (e.g. after a fresh install) the table shows the keys compiled in from `ESP32TimeServerSettingsSecrets.h`; changing them there stores them as keys of the web settings. The check box "Use the keys compiled into the firmware" returns to the compiled keys. The keys are only sent to a logged-in session over TLS and are still never logged.
- Password fields (login, MQTT, current and new admin password) have an eye button that shows the typed text in clear. It only appears once something was typed in this session (stored secrets never reach the browser), and a field that is emptied or disabled is masked again.
- GPIO pins (2026-10-22): the pin fields of the GNSS and PPS (TX, RX, PPS, PTP PPS loopback), RGB LED and MQTT (TF card) panels are in a collapsed area *GPIO pins* at the bottom of each panel (`<details>`, opened with the arrow). While it is closed, its summary shows how many visible pins differ from the default and whether a warning inside it (PTP loopback on a chip below v3.0) is shown.

**Page hardening and resources**

- HTML, CSS and JavaScript are separate files embedded with `EMBED_TXTFILES`; no inline script or style. The JavaScript writes values only with `textContent` / `.value` (the only HTML it inserts is the German text of `i18n.js`, a file of the firmware, never data from the server).
- Favicon `/favicon.ico`: an alarm clock (U+23F0) drawn for this project (own artwork, MIT like the project), PNG images of 16, 32, 48 and 64 px in an ICO container (`main/web/www/favicon.ico`, embedded with `EMBED_FILES`, cached by the browser for a day).
- Headers: `Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Frame-Options: DENY`, `Cross-Origin-Opener-Policy` and `Cross-Origin-Resource-Policy: same-origin`, `Cache-Control: no-store` for the API.
- Dark and light theme with CSS variables. Without a choice the page follows the system / browser setting (`prefers-color-scheme`, also before the script runs). The toggle button shows the mode in use (U+1F319 crescent moon = dark, U+2600 sun = light); a choice made with it is stored in the browser (localStorage).
- Languages (2026-10-23): English (default, also when the browser prefers another language) and German, for the settings page and the login page. The button left of the theme button shows the flag of the language in use. The flags are small SVG images in the CSS, not flag emoji: Windows has no flag emoji (Chrome and Edge would show the letters "DE" / "GB"). A click stores the other language in the cookie `lang` (`de` / `en`, one year, `SameSite=Strict`, `Secure` over HTTPS) and reloads the page; with unapplied changes in the form it asks first.
  - `main/web/www/i18n.js` (served as `/i18n.js`, loaded before `app.js` and not deferred) reads the cookie and holds the German texts, keyed by the English text (white space reduced to one space; texts with inline markup by their HTML). While German is loading, the page stays hidden until `app.js` has translated it (at most 3 s).
  - `app.js` translates the static texts once at load: an element with text of its own and no form controls inside as a whole, loose text next to form controls one text node at a time, and the `title`, `placeholder` and `aria-label` attributes. Texts the script writes go through `t()` (with `{name}` values). Messages of the firmware go through `ts()`: fixed texts, or the printf formats of the C++ source as patterns whose values are kept (`{1}`, `{2}` in the German text).
  - Technical terms, network terms, abbreviations and acronyms stay English (e.g. DHCP, SLAAC, Gateway, Prefix, Broker URI, Topic, QoS, Stratum, Holdover, Root Dispersion, Heap, Allow-List, Fingerprint). Texts without a German entry stay English; in German mode `window.I18N.missing` lists them (empty at the time of writing). New or changed English texts need an entry in `i18n.js`.
  - Routes: 18 of `max_uri_handlers` 20.
- Status: protocol, baud rate and (UBX) dynamic model of the GNSS receiver in separate lines; IPv6 and DNS addresses in lower case (RFC 5952), one per line, the kind (link-local, SLAAC, ...) in a second line. The page is up to 1240 px wide; below 1000 px the two status groups are stacked so the addresses are not broken.
- The header shows the host name (first label of the FQDN, also on the login page through `GET /api/info`, the only API call without login, which returns nothing but the host name), the favicon and the FQDN.
- The login page names the factory default password.
- NTP keys: "Generate" fills a random key of 32 characters `[0-9A-Za-z]` from the cryptographic random generator of the browser (`crypto.getRandomValues`, rejection sampling, no bias); "Add key" adds a row with the lowest free key ID and a random key. Duplicate IDs and malformed keys are marked, and the keys are only sent when they were changed.
- The buttons (Apply, Save Settings, Reboot, Factory reset) and their explanation share one panel, which stays visible at the bottom while scrolling (on screens below 680 px it is a normal panel at the end of the form).
- A banner shows when the settings in use differ from what the next start loads (saved settings, otherwise the compiled defaults).
- At most 3 sockets with LRU purge, request body at most 4 kB, receive / send timeout 5 s, request headers up to 2 kB (`CONFIG_HTTPD_MAX_REQ_HDR_LEN`, browsers send long headers), mbedTLS dynamic buffers (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`).
- The server task runs at priority 3 on core 0: below every timing task (PPS 14, GNSS 15, hardware NTP 17 on core 1, NTP sockets 20, Ethernet recovery 6, MQTT 5). Stack 10 kB (TLS handshake, PBKDF2, JSON); a one-time start task (8 kB) creates the key. The task stacks of `main.cpp` are unchanged.
- Strict validation on the device (`web_settings_validate`): `inet_pton`, IPv4 prefix 8 to 30, address not the network / broadcast / loopback / multicast / link-local address, gateway another host of the subnet; IPv6 address global or unique local, prefix 64 to 128 (lwIP decides on-link with the first 64 bits), gateway link-local or in the /64 of the address; DNS unicast; FQDN after RFC 1123 (host name label at most 32 characters, the esp_netif limit); time zone parsed as POSIX TZ; LCD title 1 to 17 printable characters; MQTT URI scheme, host and port, no user, path or spaces; GPIOs 0 to 54, not used twice and not one of: Ethernet RMII / MDC / MDIO / PHY reset (28, 29, 30, 31, 34, 35, 49, 50, 51, 52), strapping and UART0 console (36, 37, 38), USB Serial/JTAG (24, 25), TF card (39 to 44), LCD I2C (7, 8), the button pin if the button is enabled. Empty PTP PPS loopback pin = off.
- Physical recovery: with `UPTIME_RESTART_BUTTON_ENABLED 1`, holding the button for 10 s while the device starts does the factory reset (the LCD says so).

**Network implementation (`web_net.cpp`)**

- IPv4 through `esp_netif` (DHCP client stop/start, `esp_netif_set_ip_info`); the raw NTP path compares against the own IPv4 address, which was already a lock-free atomic and is updated by the got-IP event.
- Static IPv6 address with `esp_netif_add_ip6_address`. lwIP has no API for a static IPv6 default gateway: `CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM` with `lwip_hook_nd6_get_gw()`, which reads the gateway lock-free (seqlock) in the lwIP thread; the destination cache is cleared after a change. `CONFIG_LWIP_IPV6_NUM_ADDRESSES` 3 → 5 (link-local, SLAAC global and unique local, static, spare).
- SLAAC off: autoconfiguration is switched off on the lwIP netif and the addresses it created are removed (link-local and static stay).
- DNS: up to 3 servers (lwIP default `DNS_MAX_SERVERS`). Before 2026-10-21 (second change) only the IPv4 DNS servers of the DHCP lease were used; the RDNSS option of IPv6 router advertisements was off (`CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS 0`) and DHCPv6 is not used. Now RDNSS is on (2 servers). The DHCPv4 client and RDNSS would both write into the one DNS table of lwIP from slot 0 (the last one wins; a first version that repaired the table every 2 s still showed the router advertisement's server twice for a moment). With `CONFIG_LWIP_DNS_SETSERVER_WITH_NETIF` both hand their servers to a callback instead (`dns_server_callback()` in `web_net.cpp`, lwIP thread): it records each server under its source and index (shown in the status as "DNS received (DHCPv4)" and "DNS received (IPv6 RA)"; an RDNSS lifetime of 0 removes it) and sets the whole table at once, to the configured servers or, without configured servers, to a mix of the received ones (first IPv4, first IPv6, then the second of either). No polling, no moment in which a received server replaces a configured one. Servers received before the callback was registered are taken over from the table. Link-local DNS servers keep their interface zone. `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF` must stay off (it uses the same callback; the build stops otherwise).
- The page is started right after the Ethernet driver, before the wait for an address and independent of the GNSS setup: it stays reachable when wrong GNSS pins stop the start.

**GNSS dynamic platform model (UBX)**

- With the U-Blox UBX protocol `configure_gnss_dynamic_model()` (end of `setup_gnss()`, also after a runtime GNSS recovery) reads `CFG-NAVSPG-DYNMODEL` (key 0x20110021: 0 = Portable, the M10 default, 2 = Stationary). If it is not Stationary, Stationary is written to RAM and battery backed RAM (not to the flash of the receiver) and read back. Stationary is recommended for timing with a fixed antenna (MAX-M10 integration manual).
- Always logged: `GNSS dynamic model: Stationary`, `... Stationary (was Portable, set and read back)`, or a warning if it could not be read or was not taken over. Not done when the receiver only works in NMEA fallback mode (UBX commands unreliable).
- Shown in the status ("Stationary mode").

**Features compiled in, idle when off**

- LCD, KY-016, WS2812, MQTT, NTP authentication and OTE updates are always compiled in (`main/CMakeLists.txt` no longer reads the settings file); the `*_ENABLED` values in `ESP32TimeServerSettings.h` are now their run-time defaults. In `main.cpp` the five macros are set to 1 after the settings file is included, and the boot settings decide at run time (`s_lcd_on`, `s_led_type`, `s_mqtt_on`, `ntp_auth_configure()`).
- When off: no task, no driver, no buffers. The MQTT report buffers (about 76 kB) are allocated only when MQTT is on (`setup_mqtt_tf_queue()`); the per-request MQTT counters in the NTP paths are skipped; the LED/LCD/button task is not created when neither is used; I2C, LEDC and RMT are not initialised.
- MQTT, OTE and the LCD switch need a restart (the MQTT client, its TF card queue and the I2C bus have no clean stop in this code); the page marks them. With OTE off uploads are refused and no OTE task is started (mDNS runs anyway since 4.24).
- `DeviceName` is the default of the FQDN (certificate); its first label is the DHCP host name and, since 4.27, the mDNS host name (`<name>.local`). A failing LED driver (e.g. wrong pin) switches the LED off for that start instead of stopping the server.
- New on the page for MQTT: on/off and the broker URI (was `MQTTServerIPAddress` + `MQTTPort`); `mqtts://` brokers are checked against the ESP-IDF CA bundle. New for NTP authentication: on/off.
- MQTT settings, TF card pins and MAC address (follow-up, 2026-10-21):
  - The four report content switches are compiled in as well (set to 1 in `main.cpp`) and decided at run time (`s_mqtt_client_reporting`, `s_mqtt_memory_reporting`, `s_mqtt_historical_reporting`, `s_mqtt_distinguish_auth`). Without client reporting the NTP paths queue no client events and the client table is not allocated. QoS, retain, topic and period come from the boot settings (`s_mqtt_qos`, `s_mqtt_retain`, `s_cfg`).
  - The report buffer is sized at the start: `web_mqtt_report_size()` = 1166 bytes plus 76 bytes per client (the same formula as tools/json_message_calculator.xlsx: 50 clients = 4966 = `MQTT_REPORT_SIZE`, 500 clients = 39166, the former `MQTT_MAX_REPORT_SIZE`); `MQTT_MAX_REPORT_SIZE` is now the size for 1000 clients and only bounds the reports read back from the TF card. Without client reporting the buffer is `MQTT_REPORT_SIZE`. The page shows the buffer size for the limit entered. No PSRAM is configured, so the limit is capped at 1000 (about 77 kB).
  - With QoS as a run-time value, the TF card queue (FAT, SDMMC, file I/O) is linked in even when the compiled QoS is 0: the firmware grows by about 107 kB of flash (63 % of the app partition stays free); without QoS 1 / 2 it is not used.
  - TF card pins: SDMMC slot 1 (the default of the ESP32-P4) routes the pins through the GPIO matrix, so other pins work. GPIO 39 to 44 are no longer reserved as such; the six TF pins are always checked against the other pins of the settings (also while MQTT or QoS 1 / 2 is off).
  - MAC address: stored normalised (lower case with ':'); accepted are `xx:xx:xx:xx:xx:xx`, `xx-xx-xx-xx-xx-xx`, `xxxx.xxxx.xxxx` (Cisco) and `xxxxxxxxxxxx` (no separator) in any case, on the page and in `MACAddress`, "" = the hardware MAC (the hardware MAC entered on the page is stored as ""). Only unicast addresses (lowest bit of the first byte 0), not 00:00:00:00:00:00. A changed MAC counts as a network change and as a restart field: Apply writes a one-shot set, restarts, and the change has to be confirmed within 120 s from the new address (DHCP / SLAAC may hand out other addresses), otherwise the device restarts with the saved settings. Save only accepts the MAC that is in use.
- `NTP_MAX_PACKET_SIZE` is now always 72 bytes (the raw path buffers grow by 24 bytes each). With authentication off an authenticated request is dropped as before (it is counted as invalid).

**Open points decided**

- Allow-list of source networks: implemented (decision of 2026-10-21): at most 8 addresses or CIDR networks, IPv4 and IPv6, empty = all; a list that would lock out the current client is refused.
- NVS encryption: not enabled (decision of 2026-10-21), the steps are in 13.

**First start, fingerprint check, recovery**

1. Delete `sdkconfig` (the new options in `sdkconfig.defaults` are only used for a new `sdkconfig`), then `idf.py build` and flash. The build stops with an error in `web_net.cpp` if the ND6 hook option is missing.
2. At the first start the device creates its TLS key (about 1 s) and prints `Web admin certificate SHA-256 fingerprint: AB:CD:...`. Note it.
3. Open `https://<address>/` (IPv4 or `https://[IPv6]/`). The browser warns about the self-signed certificate: open the certificate details and compare the SHA-256 fingerprint with the console line before accepting it. After a factory reset the fingerprint stays the same; it changes only with a new FQDN or when `webtls` is erased.
4. Log in with `password` (or the `WebAdminPassword` of the secrets file) and change the password at once (red banner until then).
5. Change settings, **Apply**, check, **Save Settings**. Network changes: confirm within 120 s (over the new address if a static one was set), then Save Settings.
6. Forgotten password: factory reset with the button (hold 10 s during the start, `UPTIME_RESTART_BUTTON_ENABLED 1` needed), or `idf.py erase-flash` + flash (erases all NVS data including the calibrations and the TLS key), or erase only the `webauth` namespace with the NVS partition tool.

### 4.24 Ethernet recovery, ESP-IDF Ethernet driver, OTE over HTTPS (2026-10-21)

Taken over from release 3.0.5 of roblatour/ESP32TimeServer (comparison of 2026-10-21), adapted to this fork. Not taken over: the interleaved cache with extra stored time stamps (the cache stayed keyed by the client IP, 4.12; an exchange map found by T2 was added in its own form on 2026-10-23, see 4.29) and the stratum 1 holdover after a GNSS loss (see 13; taken over on 2026-10-22 with a default of 2 s, see 4.25).

**Ethernet transport recovery** (`ethernet_transport_recovery_task()`, `restart_ethernet_transport()`)

- The PTP clock is enabled, set and read back under `s_time_mutex`. Before, the recovery wrote the PTP clock while the PPS phase correction and the time synchronisation could write it as well.
- The restart runs in a worker task (`eth_recover`). If it does not report within `ETHERNET_RECOVERY_TIMEOUT_MS` (45 s; e.g. `esp_eth_stop()` hangs), the device restarts (`ethernet_recovery_timeout`) instead of the recovery task hanging for good.
- `physical_ethernet_link_status()` reads the link bit of the PHY itself (basic status register, read twice because the bit is latch-low): up, down or unknown. Before, `physical_ethernet_link_is_up()` returned the link flag of the driver. It is only read when a decision depends on it (the driver polls the PHY over the same MDIO bus). A restart without link is `link_unavailable` only if the PHY has no link either; a PHY link that the driver does not report is `driver_failed`.
- New triggers, retried with an interval that doubles from 60 s up to 15 min while they persist (`ETHERNET_RECOVERY_INTERVAL_US`, `ETHERNET_RECOVERY_MAX_INTERVAL_US`), all only after the server is open for business:
  - link up but no usable address for 35 s (e.g. a lost DHCP lease); the result `no_ip` only retries, it does not restart the device (3.0.5 restarts it);
  - the driver reports the link down for 35 s while the PHY reports a link (a missed link-up); if the driver still reports no link after the restart, the device restarts (`ethernet_recovery_link_state`);
  - the hardware TX time stamps were switched off after 5 errors in a row (`s_hw_tx_timestamp_recovery_requested`); the replies use software T3 meanwhile, a successful restart switches the hardware TX time stamps on again; at most `ETHERNET_RECOVERY_TIMESTAMP_ATTEMPTS` (3) per boot, then they stay off until the next boot (before: off until the next boot after the first 5 errors).
- The existing triggers (confirmed stall, silent transport failure) and their result handling are unchanged.
- The recovery no longer refuses to run while `s_hardware_ntp_accepting` is false: after a `timestamp_failed` result that flag stayed false, so every later recovery returned `retry_later` and the hardware NTP path stayed off until a restart.
- Stack of the recovery task and the worker: 3072 bytes (was 2405; the worker runs the code that ran in the recovery task, the recovery task has two new log lines with `DEBUG_ENABLED`).

**Restart deadline**: `controlled_restart()` first arms a 5 s one-shot timer that calls `esp_restart()`, so a hanging MQTT publish or NVS write can no longer stop a restart (the MQTT wait itself is 1 s).

**ESP-IDF Ethernet driver instead of the Arduino Ethernet library** (`start_ethernet_driver()`)

- `esp_eth_mac_new_esp32()` (RMII, external 50 MHz clock on GPIO 50, the pins of the Waveshare boards as constants `ETH_RMII_*_GPIO` in `main.cpp`), `esp_eth_phy_new_generic()` for the IP101 (as the Arduino library did with ESP-IDF 6), `esp_netif_new(ESP_NETIF_DEFAULT_ETH())` with the netif glue. The MAC settings of the Arduino library are kept: software reset timeout 1000 ms, RX task stack 4096 bytes, receive all multicast frames (`ETH_CMD_S_ALL_MULTICAST`, set again after every `esp_eth_start()`, also in the recovery).
- The host name (first label of the FQDN) is set before the start, so the first DHCP request carries it (before: at the start event).
- `ethernet_event_handler()` and `ethernet_ip_event_handler()` (default event loop) replace `arduino_eth_event_handler()` with the same behaviour. The default event loop task gets 4096 bytes (`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`, was 2304; the Arduino event task had 4096). A new `sdkconfig` is needed for it.
- `s_eth_handle` / `s_eth_netif` replace `ETH.handle()` / `ETH.netif()`. The Arduino core is still used (`initArduino()`, the u-blox library); only `ETH`, `Network` and `ArduinoOTA` are gone.

**Over The Ethernet updates over the HTTPS web admin server** (`main/web/web_ote.*`, `POST /api/ote`)

- ArduinoOTA (UDP 3232 invitation, then a TCP connection from the device to the PC) is replaced by an upload over the existing HTTPS server on TCP 443. Only the PC connects (no firewall rule on the PC, works over routed networks and VPNs).
- Authentication: the page uses its login (session cookie, same origin, CSRF header, as every other state changing request); scripts (`tools/ote_upload.py`) send the web admin password with HTTP Basic authentication inside the TLS connection (4.27; before: a separate OTE password as `Authorization: Bearer`); wrong passwords count for the login throttling. Release 3.0.5 sends the OTE password in clear text over plain HTTP on TCP 3232; ArduinoOTA never sent it (challenge response).
- The image is checked before the first flash write (from the native updater of 3.0.5): image magic and segment header, the application description, the project name of the running firmware and the chip and chip revision range of the build (`bootloader_common_check_chip_validity()`: an image of the pre-v3 build is refused on a v3 chip and the other way round). Then it is written sequentially (`OTA_WITH_SEQUENTIAL_WRITES`), verified by `esp_ota_end()` and selected for the next boot; after that, further uploads are refused (409) until the restart. Limits: 15 s without data, 300 s for the whole upload, image size at most the OTA partition (4 MB).
- Progress, failure (with the reason) and success are shown on the LCD as before; the OTE service task restarts the device 5 s after an accepted image (`controlled_restart("over_the_ethernet_update")`).
- The page has a new card *Firmware Update* (file, progress bar, upload; since 2026-10-22 it also holds the Over The Ethernet update settings). The button *Restart with updates enabled* (shown while updates are off, `POST /api/ote/once`) writes the settings in use with updates on as a one-shot set and restarts: updates are on until the next restart (the one after an upload included), then the saved settings apply again; the card then says "Updates are on until the next restart". Since 2026-10-23 one explanation block at the top of the card describes both ways (page upload with the login, `tools/ote_upload.py` with the web admin password), the file, the steps on the device, the setting and the security notes. The page reads the version and project name from the image and shows them in the confirmation. The upload holds the server task: other requests of the page wait until it is finished (the page pauses its refresh).
- The OTE port setting is gone from the page, the API and the validation (`OTEPort` removed from `ESP32TimeServerSettings.h`). The field stays in `web_settings_t` as `ote_port_unused`, so the saved settings (blob version 5) remain valid.
- `tools/ote_upload.py`: upload over HTTPS to an IP address or a DNS name (A / AAAA records, 4.27); the self-signed certificate is checked by its SHA-256 fingerprint (`--fingerprint`, as shown on the page and the console; without it the script stops and prints the presented fingerprint, `--insecure` skips the check). See `misc/UsefullPowerShellCommands.md`.
- Migration: a device that still runs ArduinoOTA is updated once with `tools/espota.py` (or by USB).
- OTE needs the web admin server: with `WEB_ADMIN_ENABLED 0` no firmware can be uploaded (a warning is printed when OTE is on).

**mDNS**: started at every boot with the ESP-IDF mdns component (`setup_mdns()`): `<host name>.local` (the first label of the FQDN since 4.27; before, a field of its own) and the service `_https._tcp` on port 443 (with the web admin). Before, only ArduinoOTA started mDNS, and only while OTE was on. `tools/ote_upload.py` does not use it (4.27).

### 4.25 PPS holdover and behaviour after it (2026-10-22)

Holdover from release 3.0.5 of roblatour/ESP32TimeServer (`ntpHoldoverDurationUs`, `ntpHoldoverDispersionPpm`), with a default of 2 s instead of 180 s, plus settings for what happens after it. All five are on the page (GNSS and PPS, *When the PPS is lost*) and take effect at once with Apply (`app_apply_live_settings()`).

- The PPS counts as lost after 3.5 s without a pulse (`Gnss_Validity_Timeout_Us`, unchanged). The holdover is counted from that moment (3.0.5 counts from the last pulse, so its 180 s are 176.5 s here; with the 2 s default the counting from the last pulse would never take effect).
- **PPS holdover** (`pps_holdover_s`, 0 to 600 s, default `ntpHoldoverDurationUs` = 2 s, 0 = off): stratum 1 continues ("GPS", leap indicator 0) if the GNSS was valid when the PPS was lost (the GNSS may be lost as well, e.g. antenna fault), no time step is running, no sanity mismatch and no stale sync, the model clock is valid and, with hardware time stamps, the PTP compensation as well. The root dispersion grows by the ppm of the setting below of the time since the last pulse; the replies now carry the root dispersion of the status (`build_ntp_reply()`). At most 600 s, so the 15 min sync stale limit cannot end it early.
- **Root dispersion growth** (`pps_dispersion_ppm`, 2026-10-23; on the page in ms per second, 0.001 to 10 = 1 to 10000 ppm, default `ntpHoldoverDispersionPpm` = 100 ppm = 0.1 ms per second; 15 ppm since 4.58): how fast the advertised root dispersion grows while the server coasts after the PPS loss (holdover and stratum 2 to 15). The advertised value is capped at 1 s (65536 in the 16.16 NTP short format). The age used is capped at the longest coasting time (one hour): before, the growth was computed with the full age, so the 64-bit intermediate overflowed (undefined behaviour) after about 8 days without PPS at 200 ppm, although the value was not used then. Now the product stays below 2.4e18 whatever the serving time after the PPS loss (also with "no limit"); the stratum 16 replies keep the fixed `NTP_ROOT_DISPERSION`.
- **After the holdover** (defaults in `ESP32TimeServerSettings.h`):
  - `pps_lost_serve` (`ntpServeAfterHoldover`, default on): off = the requests are not answered at all (raw IPv4 / IPv6 path and sockets; internal requests of the health test still are).
  - `pps_lost_serve_min` (`ntpServeAfterPpsLostMinutes`, 0 to 1440 min, default 60, 0 = no limit): with answering on, the answers stop this long after the PPS was lost (the holdover included).
  - `pps_lost_stratum` (`ntpStratumAfterHoldover`, 2 to 16, default 16): 16 = stratum 16, leap indicator 3, "INIT" (as before). 2 to 15: leap indicator 0, "GPS", the growing root dispersion, while the coasting conditions of the holdover hold, at most one hour after the last pulse (`Pps_Lost_Coast_Max_Us`, the limit of the model clock and the PTP mapping); then stratum 16.
  - The page and the settings file warn that relying on time that is no longer synchronised is probably not a good idea, and to use a stratum below 16 and a long answering time only when the clients absolutely need a time source.
- Defaults: the old behaviour apart from the 2 s holdover and that the answers stop 60 min after a PPS loss.
- While the server does not answer on purpose, the Ethernet transport recovery does not count the requests without replies as a stall (and does not verify a recovery by them); the time of the last such second counts as a reply when answering resumes.
- LCD and LED: `[H]` (code 15), LED colour of the warning state, flashing, during the holdover. The status on the page shows "stratum 1, PPS holdover (PPS lost)".
- Settings blob version 6: the holdover is stored in the place of the former OTE port, the three other fields are appended. A version 5 set is still read (its fields are a prefix of the new layout); the four fields then get their defaults.
- Settings blob version 7: the root dispersion growth is appended (it fits into the tail padding of version 6, so both have the same size; the version number tells them apart). Versions 5 and 6 are still read; the new field then gets its default.
- Fix from the review: the OTE default password check of the page (`handle_state()`) had a raw NUL byte in a character literal; it was written as `'\0'` (the check is gone since 4.27).

### 4.26 Per-client NTP rate limit (2026-10-23)

Requests per second and client, as a web setting (Network panel, *NTP clients*, `ntp_client_rate_limit`, 0 to 10000, 0 = off, default `ntpClientRateLimitPerSecond` = 20). Takes effect at once with Apply (`app_apply_live_settings()` → `ntp_cache_set_rate_limit()`); it is not a network change, so no confirmation.

- **Client:** the source IP address (IPv4, or the full 128-bit IPv6 address; a /64 is usually a whole LAN). The source port cannot be used: chrony uses a new one for every request and NAT routers rewrite ports. All clients behind one NAT address share one limit.
- **Default 20:** the worst-case single client is chrony with `minpoll -4 maxpoll -4` (16 requests per second; iburst and the interleaved mode do not add requests). A limit of exactly 16 would sooner or later drop requests of such a client whose clock runs slightly fast compared to the server (any lasting rate difference adds up); the 25 % margin covers that, e.g. chrony slewing its clock by up to 83333 ppm. The reasoning is in `ESP32TimeServerSettings.h`. Newer chrony versions accept `minpoll -6` / `-7` (64 / 128 per second); such clients are limited to the setting.
- **Algorithm:** generic cell rate algorithm (virtual scheduling) with one `int64_t rate_tat_us` per cache record (theoretical arrival time of the next request, `esp_timer` µs): an answered request moves it on by 1 s / limit, a request that arrives more than 1 s - interval ahead of it is over the limit and leaves it unchanged. Sustained rate exactly the limit, bursts of up to one second worth of requests (absorbs jitter and processing stalls of the NTP tasks), no window boundaries. A simulation of the same integer arithmetic (1 h each): a 16/s client with ±5 ms jitter, with a clock 8.3 % fast, or with 300 ms stalls every 10 s gets no drop at a limit of 20; a 100/s flood is held to 20/s.
- **Placement:** inside `ntp_cache_find_or_create()` / `ntp_cache_find_or_create_ipv6()`, which both NTP paths already call (same lookup, same mutex; the check costs one clock reading, a compare and an add). Hardware path: after the authentication check, before the reply is built; the RX callback is unchanged. Socket path: after the global load shedding, before the MQTT request counters. Requests of the device to itself (startup health test) are not limited.
- **Over the limit:** dropped without an answer (no Kiss-o'-Death "RATE": a KoD costs the same transmit path as an answer and can be abused with spoofed sources to make a victim back off; chrony's own `ratelimit` drops as well). The interleaved mode is not disturbed: the cache keeps T2 / T3 of the last answered request, which is what the client refers to next.
- **Timing:** T2 is taken before the check (hardware RX time stamp, or right after `recvfrom()`), T3 after it, so the time stamps are unchanged; the server turnaround grows by well under 1 µs, which the NTP calculation removes. No new lock, no allocation, no change to the PPS / PTP code.
- **Memory:** 8 bytes per cache record, about 2 kB for the 128 + 128 records.
- **Status:** the Status panel shows the limit and the number of requests dropped by it since the start (`s_ntp_rate_limited_requests`, `ntp_rate_limited` in `/api/state`). The MQTT report is unchanged; with MQTT the dropped requests count in the requests per second, not as valid requests.
- Limits: a record that is pushed out of the cache (more than 128 active clients of one family) starts again with a full burst; a flood with many spoofed sources is left to the global load shedding; a host sending with a spoofed source address shares that address's limit for requests without a T2 cookie (since 4.30 the interleaved requests of the real client have a bucket of their own).
- Settings blob version 8: the field is appended (versions 5 to 7 are still read; the field then gets its default).

### 4.27 Scripted firmware uploads with the web admin password (2026-10-23)

No separate OTE password and no mDNS host name setting any more. The script and the page use the same password, so many devices with the same compiled `WebAdminPassword` can be updated by one script run (a loop over the hosts).

- **Removed settings:** *mDNS host name* (`ote_device_name`) and *OTE password* (`ote_password`, `OTEPassword` in `ESP32TimeServerSettingsSecrets.h`) are gone from the page, the API (`ote_device_name`, `ote_password`, `ote_password_set`, `ote_password_default`), the validation and the secrets file, together with the default password warning of the card and of the *Restart with updates enabled* confirmation. Only *Accept firmware updates over the network* is left in the card.
- **Saved settings:** no new blob version. The two fields stay in `web_settings_t` as `ote_name_unused` / `ote_password_unused` (the layout of versions 5 to 8 is unchanged) and are zeroed whenever a set is read. If the saved set still holds the former OTE password or name, it is written back without them at the next start (log "the former OTE password and device name were erased"; NVS marks the old entry as erased, the flash page is reused later). The one-shot set is erased at the start anyway.
- **Authentication of the script** (`handle_ote()`): `Authorization: Basic base64(<user name>:<web admin password>)` inside the TLS connection; the user name is ignored (the script sends `admin`), the password is the part after the first `:`. It is checked like a login (`web_auth_check_password()`: PBKDF2 hash of the stored or compiled password, throttling per source address and global, success resets the count). Another scheme (e.g. the `Bearer` of the former script) gets 401 "Use HTTP Basic authentication with the web admin password" without a password check; an undecodable or too long header counts as a failure. Basic instead of Bearer: the admin password may contain spaces and non-ASCII characters (UTF-8), which the base64 encoding carries unchanged.
- **Factory password:** while the factory password "password" is in use (`web_auth_default_password_active()`), scripted uploads are refused (403 "Scripted uploads are refused while the web admin password is the factory default"); uploads on the page still work after the login. For bulk updates set an own `WebAdminPassword` in the secrets file.
- **mDNS:** announces the host name of the FQDN (`s_hostname`, also the DHCP host name) instead of the former field; with the defaults the name is the same (`DeviceName`, `ESP32TimeServer.local`).
- **`tools/ote_upload.py`:** `-i` takes an IP address or a fully qualified DNS name. The name is resolved with `getaddrinfo()` (A and AAAA records, `-4` / `-6` for one family) and the addresses are tried in the order of the resolver, each with a unicast TCP connection (10 s connect timeout). `.local` names and single-label names are refused, since the operating system would resolve them with mDNS, LLMNR or NetBIOS (multicast / broadcast). The password comes from the environment variable `WEB_ADMIN_PASSWORD` (`--password-env`) or is asked for. The `Host` header and the TLS server name carry the name given with `-i`.
- **Migration:** a device with the previous firmware is updated once with the previous script (`OTE_PASSWORD`, OTE password) or on the page; after that only the web admin password works.

### 4.28 Quick wins of the review of 2026-10-23

Priorities 1 to 6 of the action list in `REVIEW_2026-10-23.md` (finding IDs in brackets), plus the interrupt latency default asked for in its open questions. No change to the PPS interrupt, the servo or the time stamps of the replies.

- **RMC checksum (Z-02):** `wait_for_nmea_rmc_time()` uses a sentence only if its checksum is right (`is_nmea_checksum_valid()`, before only used for GSV). A corrupted or cut RMC could carry a wrong time or date. The checksum digits are read without `sscanf()` (the GNSS time sync task has a small stack).
- **Interleaved replies without software T3 (Z-07):** the EMAC driver gives up waiting for the TX time stamp after 50 polls, e.g. when the reply waited behind other frames in the TX queue (`esp_eth_mac_esp_dma.c`, `PTP_TX_TIMESTAMP_TO`). The software T3 stored until now missed that queueing delay and was sent in the next interleaved reply. Now the cache stores T2 = 0 in that case, so the next request of that client is answered in basic mode (chrony accepts that) and the interleaved mode resumes with the following one. The same applies while the hardware TX time stamps are switched off after errors.
- **MQTT task priority 5 (Z-16):** the esp-mqtt client task ran at priority 23, above all timing tasks (PPS 14, GNSS 15, hardware NTP 17, lwIP 18, NTP socket server 20); a TLS connect could delay the NTP replies. Now 5, the esp-mqtt default.
- **No NVS erase by `initArduino()` (S-01):** `initArduino()` erases the whole NVS partition when `nvs_flash_init()` fails with "no free pages" or "new version" (`esp32-hal-misc.c`), although `initialize_nvs_storage()` deliberately does not erase. Password hash, TLS key, settings and calibrations would be lost and the page would fall back to the compiled password. `initArduino()` is now called only after a successful NVS start (console warning otherwise). On the ESP32-P4 it does nothing else this firmware needs: it sets the default log level and calls the empty `init()` / `initVariant()`.
- **No UBX commands to NMEA receivers (G-01):** with the NMEA protocol the u-blox library is no longer started (`s_gnss.begin()`) and the port configuration (`setUART1Output()` etc., UBX CFG-VALSET) is sent only to u-blox receivers. Other chip sets could misread the UBX frames.
- **Rollover-safe PTP reading in the holdover (Z-08):** the driver reads the seconds and the sub-seconds register one after the other; a reading on the second rollover can be off by about 1 s. The reference point that `ptp_servo_holdover()` publishes for the whole holdover is now taken from two readings that must be in order and less than 100 µs apart (`ptp_read_raw_ns_consistent()`, up to three attempts). The software offset measurement keeps its single bracketed readings (median, already robust).
- **Interrupt latency default 2.5 µs** (was 2.6): the first measurement and the stored value of the -O2 build are 2.50 µs. It is only used until the automatic measurement has run.
- Verified: build with the IDF 6.1 warning flags (-O2, no warning in `main/`) and a compile check with `CALCULATE_STACK_SIZES_ENABLED 1` (debug code). Not yet tested on the board; the stack of the PPS task (two more `int64_t` in the holdover path) should be checked once with `CALCULATE_STACK_SIZES_ENABLED 1`.

### 4.29 Exchange map for the interleaved mode (2026-10-23)

Review finding S-02. In interleaved mode (RFC 9769) the client sends the T2 of the last reply it received back as the origin timestamp, and the reply carries the T3 of that exchange. Until now only the latest exchange of each client address was kept (4.12). Two cases cost a client its interleaved mode:
- **Spoofing:** one request with a forged source address (the client's) replaced the stored exchange, so the client's next interleaved request no longer matched and got a basic reply. An off-path sender could do this permanently with one packet per poll interval.
- **NAT:** several clients behind one address replaced each other's exchange.

What changed (`main/custom/ntp_cache.c`):
- **Exchange map:** besides the latest exchange in the address record (kept for 30 minutes, also for clients that poll rarely), the recent exchanges of all clients are stored in a map found by their T2. T2 is a receive time stamp with ns resolution that an off-path sender cannot know, so it works as a cookie. In addition the entry must belong to the same source address: a 32-bit tag, the IPv4 address itself or an FNV-1a hash of the IPv6 address.
- **Structure:** 4-way set associative, 512 sets (2048 entries of 16 bytes: T2, T3 − T2 as 32 bits, address tag = 32 kB BSS). The set is chosen by Fibonacci hashing of T2. A new exchange replaces the oldest entry of its set. The age is the difference to the new T2 in modulo arithmetic, so the NTP era change in 2036 is handled, and so is a T3 that is already in the next era. No expiry is needed: an entry stays a correct pair at any age.
- **Lookup:** `ntp_cache_find_or_create()` / `_ipv6()` now take the origin timestamp of the request and return the T3 for an interleaved reply (0 = basic mode). It is the record first (latest exchange of the address), then the map. This happens in the same mutex section as the address lookup and the rate limit check, with at most 4 extra compares and one 64-bit multiply. The caller still requires receive timestamp ≠ transmit timestamp in the request. The record structures are now private to `ntp_cache.c`.
- **Storing:** `ntp_cache_update()` / `_ipv6()` write the record and the map. T2 = 0 (no hardware TX time stamp, 4.28) stores nothing in the map.
- **All three paths** (hardware IPv4 and IPv6, socket path) use the same lookup. The socket path compared the IPv4 origin with a tolerance of ±8 units of 2^-32 s (`NTP_CACHE_TIMESTAMP_MATCH_TOLERANCE`); now every path compares exactly, as the client echoes T2 unchanged. The request time stamps are read with `read_ntp_timestamp()`.
- **Robustness against floods** (simulation of the same algorithm): an entry survives 500 later exchanges of all clients almost surely, 1000 with 87 % and 2000 with 40 % probability. A spoofing source therefore needs about 1000 to 2000 answered requests per poll interval of its victim (before: one). Under such a flood the replies stay correct, only in basic mode.
- **Side effect:** a client whose last reply was lost now also stays interleaved. It refers to the reply before, which is still in the map.
- **Not changed:** the per-client rate limit (per address, 4.26), the record lifetime and the time stamps. The map adds no work before T2 (hardware RX time stamp) and none between the reading of a basic T3 and the transmission.
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, and a Python model of the map with the same constants and rules. Results: single client 99 of 100 interleaved; spoofed basic requests with the client's address: still 99 of 100, the record-only cache 0 of 100; two clients behind one address: 99 + 99; no hardware TX time stamp: basic once, then interleaved; T2 of another address: basic; era change. Not yet tested on the board: `chronyc ntpdata` should keep showing `Interleaved: Yes`.

### 4.30 Own rate limit bucket for requests with a T2 cookie (2026-10-25)

Review finding S-17. The per-client rate limit (4.26) counted all requests of an address in one bucket. A host that can send with a client's source address (usually from the same LAN) and sends more than the limit (20 per second) used it up, and the real requests of the client were dropped.

What changed (`main/custom/ntp_cache.c`):
- **Two buckets per address record:** `rate_tat_us` as before, and `rate_tat_cookie_us` for requests whose origin timestamp is the T2 of an earlier exchange with this address (in the address record or in the exchange map, 4.29). Such a request carries a value that only the real client got, in the reply to that exchange (T2 is a receive time stamp with ns resolution, and the reply went to the real address). Same limit and same algorithm in both buckets.
- **Order:** `check_request_locked()` does the interleaved lookup first and then the rate limit check in the bucket the lookup selects. Before, the lookup came after the check and only for requests within the limit. Both stay in the same mutex section. A request over the limit still gets no interleaved T3 (it is dropped).
- **Effect:** a client in interleaved mode (chrony with `xleave`) keeps all its replies while a spoofing host fills the other bucket. Its requests without a cookie (the first request, the request after a reply without hardware TX time stamp, 4.28) still share the bucket with the spoofed requests.
- **Not changed:** requests of the device to itself are not limited; T2 is taken before the check and T3 after it; no new lock, no change to the time stamps. The lookup now also runs for requests over the limit (at most 4 compares and a 64-bit multiply, for IPv6 also the FNV-1a hash of the address).
- **Side effect:** one address gets up to twice the limit answered if it sends requests with its latest T2 and requests without (20 + 20 per second by default). The other clients stay protected, the global load shedding is unchanged.
- **Limits:** a host that can read the replies to the client (on the path, or on a shared medium) knows the T2 values and can fill the cookie bucket as well; only authentication helps against that. Basic mode clients (no cookie) are not protected.
- **Memory:** 8 bytes per record, 2 kB BSS (DIRAM 176 092 B, was 174 044 B).
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, and a Python model of the address record, the exchange map and both buckets with the same constants and integer arithmetic (10 minutes per case). A chrony-like client with 16 requests per second alone: 9600 of 9600 answered, all but the first interleaved (the same with one bucket). With a spoofing host sending 50 per second with the client's address: one bucket 3591 of 9599 answered, two buckets 9599 of 9599 (9598 interleaved). Spoofing host with 1000 per second: 9598 of 9598. Client starting during the flood: its first (basic) request needs some tries (median 2, 12 and 24 lost requests at 50, 200 and 1000 per second, at most 182 in 10 runs). Three replies without hardware TX time stamp during the flood: 4 requests lost in total, then interleaved again. Not tested on the board yet.

### 4.31 Re-lock after a PPS holdover without a phase step (2026-10-25)

Review finding Z-06, `PTP_FREE_RUNNING 1` (hardware time stamps). During a PPS holdover (4.20, 4.25) the PTP mapping extrapolates the offset with the estimated rate. The true rate drifts meanwhile (temperature), so when the PPS returns, the measured offset differs from the mapped one by the error accumulated in the holdover. Until now the first measurement after the holdover set the mapping at once: T2 / T3 jumped by that error (about 0.3 µs after 300 s, some 10 µs after an hour). In addition the time stamps between a step of the PTP clock and the next servo update were not mapped, but were served with stratum 1.

What changed (`main/main.cpp`):
- **(a) No stratum 1 without a valid mapping:** `get_ntp_reply_status()` gives stratum 1 only if the PTP clock is not in use or its mapping is valid (`!s_ptp_clock_ready || s_ptp_comp_valid`). This covers the second after a step by the servo, after `synchronize_hardware_clock()` and after an Ethernet recovery. The holdover already required it (4.25).
- **(b) Continuous mapping at the re-lock:** `ptp_servo_holdover()` marks the published mapping (`relock_pending`). At the first servo update after the holdover, `ptp_relock_residual()` evaluates that mapping at the new reference point; the difference to the new estimate (the residual) is added to the published offset, so the served time continues exactly where the holdover left it. The residual then decays with `PTP_RELOCK_TIME_CONSTANT_S` (16 s, the time constant of the servo), at most `ptp_relock_max_slew_ppb` (5 µs per second), as an extra rate of the mapping. Below 1 ns it ends.
- **Not affected:** the filter, the frequency command and the outlier handling work on the estimate as before; the residual only changes the published mapping. Without a holdover the residual is 0 and the published values are the same as before (also in the model, bit for bit). Steps stay steps: an offset above 100 µs after the holdover sets the PTP clock as before and ends the re-lock (the time stamps then jump, with stratum 16 until the next update). A filter restart within the re-lock (the first loopback measurement in the 3 s after the start, or 5 outliers in a row) is absorbed in the residual the same way. If the PPS is lost again during the ramp, the next holdover continues from the mapping including the residual.
- **Root dispersion:** while the residual is above 15 µs (one unit), the stratum 1 replies advertise it as root dispersion (`s_ptp_relock_residual_ns`), otherwise `NTP_ROOT_DISPERSION` as before.
- **Not changed:** the socket path (model clock, system clock) and `PTP_FREE_RUNNING 0`. With `PTP_FREE_RUNNING 0` the mapping is invalid for the few µs of each write, so a reply built in that moment now gets stratum 16.
- **Debug output:** `PTP servo: re-lock, mapping residual … ns`, and a status line `PTP re-lock: mapping residual … ns, rate … ppb` while it decays.
- **Cost:** in normal operation one compare per second in the PPS task and one atomic load per stratum 1 reply. Frame of the PPS task 704 B instead of 688 B (`ptp_servo_update()` is inlined; stack 3584 unchanged, to be checked on the board with `CALCULATE_STACK_SIZES_ENABLED 1`). DIRAM 176 108 B (+16).
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, and a Python model of the filter, the servo and the mapping with the same constants (loopback noise 15 ns, update a few ms after the edge). Normal operation (20 min): identical served time with and without the change. Jump of the served time at the re-lock and time until the error is below 50 ns:

| Holdover, rate change in it | Error at the end of the holdover | Before: jump | Now: jump | Now: below 50 ns after |
|---|---|---|---|---|
| 300 s, 1 ppb | 0.74 µs | −0.72 µs | 1 ns | 41 s |
| 600 s, 2 ppb | 2.06 µs | −2.04 µs | 0 ns | 60 s |
| 3600 s, 8 ppb | 33.9 µs | −33.9 µs | 0 ns | 98 s (largest change 2.0 µs per s) |
| 3600 s, 40 ppb | 149 µs | step | step | – |

  After the ramp the served error is the same as without the change (RMS 6.80 ns in both). Over 10 seeds the jump at the re-lock is at most 1 ns (rounding). Not tested on the board yet.

### 4.32 GNSS validity after a cold start (2026-10-26)

Review finding Z-04. A receiver that has just started can report a valid time whose UTC second is off: until it has received the UTC parameters (leap seconds, broadcast every 12.5 minutes) it may count the wrong number of leap seconds. The NMEA path only checked the RMC status "A", the u-blox path only `validDate` / `validTime`, which do not mean that the leap seconds are known.

What changed (`main/main.cpp`, web admin):
- **RMC mode indicator** (`parse_nmea_rmc_sentence()`, field 12, NMEA 2.3 and later): accepted are A (autonomous), D (differential), P (precise), R (RTK) and F (float RTK). E (dead reckoning), N (not valid), M (manual input), S (simulator) and unknown letters are refused like status V. A receiver without the field (NMEA 2.1) or with an empty field is judged by the status alone, as before. The navigation status of NMEA 4.1 (field 13) is not used. This applies to every RMC the firmware reads: the sync candidate after the PPS edge, the validity check every second and the start-up qualification.
- **u-blox** (NAV-PVT): the sync candidate (`acquire_sync_candidate()`) and the validity check (`current_gnss_timing_is_valid()`) also require `fullyResolved` (UTC time of day without seconds uncertainty) and `confirmedTime` when the receiver reports the confirmation (`confirmedAvai`; otherwise it is not required, so receivers without it are not locked out). The start-up qualification (fix and PPS) is unchanged, so the escalation to the NMEA fallback after 2 minutes does not start earlier; the first synchronisation then waits until the time is fully resolved. `validUTC` of NAV-TIMEUTC follows with the native UBX parser (option b of the review).
- **Minimum GNSS fix age before stratum 1** (web setting, GNSS and PPS panel, *After a start*, `gnss_min_fix_age`, 0 to 3600 s, default `gnssMinimumFixAgeSeconds` = 0 = off, recommended 750 s = 12.5 min for a receiver without backup battery): after each start the server answers at stratum 16 (also no holdover or coasting stratum) until
  - the GNSS time has been valid for this long without a gap of more than 30 s (`Gnss_Invalid_Reacquisition_After_Us`; a longer gap starts the count again), counted from the first valid reading of the GNSS time sync task (`gnss_fix_note_valid()`), and
  - a sync check was made after that: the time sync task leaves its periodic validity loop as soon as the age is reached, compares the GNSS second with the clock (a difference of 1 s is confirmed and stepped as before, a larger one trips the existing sanity check) and then allows stratum 1 (`s_gnss_fix_qualified`, console line `GNSS fix older than N s and time checked: stratum 1 allowed`).
  - It then holds until the next start: a later loss of the GNSS or PPS is handled as before (holdover, stratum 16) without a new wait. With the setting off the qualification happens at the first synchronisation, so the behaviour is the same as before.
  - Takes effect at once with Apply while the server waits; a change after stratum 1 was reached applies from the next start. The time is set from the GNSS during the wait as usual, so the clock and the PPS discipline are ready when stratum 1 is allowed.
  - Status panel: `not stratum 1 yet: GNSS fix N s old, M s required after the start` (`gnss_fix_waiting`, `gnss_fix_age_s` in `/api/state`). German texts in `i18n.js`.
- **Why per start and not per cold start:** an NMEA receiver does not tell whether it started cold. The setting is meant for receivers without backup battery (every power-up is a cold start); with a backup battery leave it off.
- **Settings blob version 9:** the field is appended; sets of versions 5 to 8 are read and the field gets its default.
- **GGA** (fix quality, satellites) is not used: it is switched off in the GT-U16 (open question about configuring the module).
- **Cost:** none in the PPS task and in the NTP path except one more atomic load per reply; frame of the GNSS time sync task 176 B instead of 160 B (stack unchanged). DIRAM 176 132 B (+24), image 1 614 314 B (+5.9 kB, mostly the texts of the page).
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the page against a local mock of the JSON API (field, default, range check in English and German, Apply, status text, `I18N.missing` empty), and a Python model of the RMC rules and of the fix age logic. RMC: 12 sentences (no mode field, A, D, R, E, N, M, S, empty, NMEA 4.1, status V, unknown letter). Fix age (receiver valid from 10 s): setting off: stratum 1 at the first sync, as before; a 1 s label error until 600 s is served at stratum 1 for 600 s with the setting off and never with 750 s (stratum 1 from 761 s); an 18 s error is never served at stratum 1 with 750 s (the existing sanity check restarts the device, stratum 1 after the new wait), with the setting off it is served for 601 s; a 20 s outage keeps the count, a 60 s outage starts it again; an outage after stratum 1 returns at once; the setting raised after stratum 1 has no effect, raised while waiting it applies. Not tested on the board yet.

### 4.33 Clean-ups of the review (2026-10-26)

Review findings E-11, E-13 and E-16. No change to the time stamps, the PPS discipline or the servo.

- **PPS queues before the capture (E-11):** `setup_pps_input()` runs at the very beginning of `app_main` and enables the capture channel, while the PPS semaphore and the two PPS queues were only created later in `setup_mutexes_and_semaphores()`. The callback checked the handles for `nullptr`, so a pulse in between was only lost, but it read handles that another task was writing. They are now created at the start of `setup_pps_input()`, before the channel is enabled (error message if out of memory); `setup_pps_input()` still runs first.
- **Comments (E-13):**
  - `ESP32TimeServerSettings.h`, timing notes: the replies are time stamped by the EMAC hardware (PTP clock) or, on the socket path, by the model clock, not by `gettimeofday()`; the error is some 10 ns, not "10 to 100 µs" of interrupt latency (the capture timer takes the edge); the baud rate only decides when the label of a second arrives.
  - IPv4 fragments: lwIP does not reassemble them (`CONFIG_LWIP_IP4_REASSEMBLY` is off), it drops them; NTP requests are never fragmented.
  - `hardware_ntp_server_task()`: handles IPv4 and IPv6; a request without hardware receive time stamp is dropped in `ntp_ethernet_input()`, not handed to the socket path.
  - Status table in the LCD task: the PPS holdover code 15 (`[H]`) was missing and the note said the holdover is not shown. `misc/status_indicators.md` now uses the same texts as the code for `[I]` and `[*]`.
- **One address lookup per request (E-16):** `ntp_cache_find_or_create()` / `_ipv6()` return the index of the client record (`uint16_t *slot`), and `ntp_cache_update()` / `_ipv6()` take it back. The update uses that record if it still belongs to the client (active, same address; a client has at most one record) and only searches again if it was purged or given to another client in between (`NTP_CACHE_SLOT_NONE` after a failed lookup). This saves the second scan of up to 128 records per answered request (the IPv6 scan compares 16 bytes per record), about 1 µs by the estimate of the review, after the reply was sent. All three paths (hardware IPv4 and IPv6, socket path) pass the slot.
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, and a Python model of the record table with and without the slot reuse (5 client counts from 20 to 1000, 20 runs of 20 000 random requests each, other requests and purges between the lookup and the update): identical tables after every step; the update searched again only when its record had been displaced in between (in the model, with very many requests in flight at once, 25 to 65 % of the updates; with one request at a time practically never). DIRAM unchanged (176 132 B), image +326 B; stack frame of ntp_cache_update() 80 B instead of 64 B, the frames of the NTP tasks and request handlers are unchanged. Not tested on the board yet.

### 4.34 One hardware NTP handler for IPv4 and IPv6 (2026-10-26)

Review finding E-12. `process_hardware_ntp_request()` and `process_hardware_ipv6_ntp_request()` were copies of each other except for the address family (about 90 % of their 400 lines). They are now one function, `process_hardware_ntp_request(..., bool ipv6)`; `hardware_ntp_server_task()` passes the family of the frame (EtherType 0x86DD) and, as before, drops the frame if that family is switched off (`PreferIPvX`).

- **Shared:** request check, version / mode, authentication (and its MQTT counters), client record and rate limit, MQTT request counters, reply status and `serve`, the reply, the interleaved T3, the transmit loop with the retries, the cache update (T2 = 0 without hardware TX time stamp), the transmit delay estimate and the response counters.
- **Per family:** the parser (`is_ipv4_ntp_request()` / `is_ipv6_ntp_request()`), the client address (IPv4 as a number, IPv6 as 16 bytes) and its cache functions, the MQTT source address (IPv6 with the scope of the interface), the IP header of the reply, and in the transmit loop the lengths and checksums (IPv4: header checksum, UDP checksum 0; IPv6: UDP checksum over the reply). `compensate_basic_t3()` and `update_tx_delay_estimate()` get the family, so the transmit delay estimates of IPv4 and IPv6 stay separate.
- **Timing unchanged:** T2 is the hardware receive time stamp as before. T3 is still read right before the transmission, inside the retry loop; between the reading and `transmit_with_hw_timestamp()` the same steps run in the same order as in the former function of that family (T3 and, for authenticated requests, the MAC into the reply, then lengths and checksum, then the copy into the frame). `transmit_with_hw_timestamp()` and `update_tx_delay_estimate()` are marked `noinline`: each now has a single caller, and inlined they would deepen the stack of every other call chain of the handler.
- **Removed:** stores before the transmit loop that the loop always overwrote before anything was sent (the basic T3 written with `get_current_time_in_ntp64_format()`, the initial lengths, the IPv4 header checksum and the IPv6 UDP checksum over a reply without T3). Nothing was sent with them; the request is now handled a little sooner, which does not touch T2 or T3.
- **Stack:** frame of the handler 400 B (before: IPv4 352 B, IPv6 384 B); the call chains below it are unchanged, so the deepest chain grows by 16 B. Stack of the hardware NTP task unchanged at 2719 B; to be checked on the board with `CALCULATE_STACK_SIZES_ENABLED 1`.
- **Size:** image −1.7 kB, DIRAM unchanged.
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, stack frames from the disassembly, and a reading of each family's path against the former function (the frame written to the EMAC checked field by field; there is no host compiler for a test). Not tested on the board yet: IPv4 and IPv6 requests with chrony (`Interleaved: Yes`, `TX timestamping: Hardware`), an authenticated request of each family, and the transmit delay estimates in the debug output (`Basic T3 TX delay: v4 … v6 …`).

### 4.35 Integer absolute times in the PPS task and the PTP functions (2026-10-26)

Review finding Z-13. The ESP32-P4 computes `double` in software (ROM routines, IEEE binary64). The PPS task held absolute times as doubles of the µs since boot (`isr_us`, `edge_local_us`) and of the system time (`sys_us`); the PTP functions multiplied `edge_local_us` by 1000. A double of the time since boot has steps of about 1 ns after 100 days and 4 ns after a year, and the anchor of the edge is formed from the difference of two such numbers and a mean of 48 of them.

What changed (`main/main.cpp`):
- **`local_instant_t`:** an instant on the fine local timebase as whole ns (`int64_t`) and the rest of −0.5 to 0.5 ns (`double`). `local_ns_since()` gives the ns from such an instant to a local time: the difference is formed in integers, so the double holds only a small number. `floor_div()` rounds integer divisions down.
- **Anchor of the edge:** d (ISR time − tick time) is formed in integers: the whole seconds of the capture ticks in integer ns, the rest of the second as a double below 1e9 ns. The values of the window are kept relative to `anchor_ref_ns` (the integer part of the first d of the window), so the statistic (mean of the lowest 75 % of 64 values) works on numbers of a few µs. The statistic itself is unchanged (no new measurement of the interrupt latency). The edge is tick time + central value − interrupt latency, which is the former ISR time − excess − latency.
- **System clock phase:** the system clock value at the edge is formed in integer ns (a double of the Unix time in µs has steps of 0.25 µs); the second of the edge and the phase follow from it.
- **PTP functions:** `ptp_write_from_edge()`, `set_hardware_clock_from_pps()`, `ptp_measure_offset_sw()`, `ptp_servo_step()` and `ptp_servo_update()` take the edge as `local_instant_t` and work with `local_ns_since()`. The model clock gets the edge in whole ns as before (it was rounded there already).
- **Interrupt latency measurement:** the differences of the local times are formed before the conversion to double.
- **Unchanged:** the filters of the frequency estimate, the servo and the PTP mapping stay in double (they hold small numbers); the statistic of the anchor; the measured interrupt latency in NVS (`PPS_ISR_LATENCY_CODE_VERSION` unchanged); the debug line prints the same quantities.
- **Stack:** frame of the PPS task 736 B instead of 704 B (+32 B; +48 B since 4.29 with 4.31). The stack stays at 3584 B (`PPS_Discipline_Task_Stack_Size`, fixed); to be checked on the board with `CALCULATE_STACK_SIZES_ENABLED 1`.
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, and a Python model (IEEE binary64 like the ROM routines) of the former and the new arithmetic against exact rational arithmetic of the same formulas, 200 windows of 64 edges per uptime:

| Uptime | Edge, before: max / RMS | Edge, now | System clock phase, before / now (max) | Software PTP reading (time since the edge), before / now (max) |
|---|---|---|---|---|
| 1 h | 0.002 / 0.001 ns | < 0.001 ns | 124 / 1 ns | 0.002 / < 0.001 ns |
| 30 d | 1.7 / 0.54 ns | < 0.001 ns | 123 / 1 ns | 1.7 / < 0.001 ns |
| 100 d | 5.2 / 1.8 ns | < 0.001 ns | 124 / 1 ns | 5.2 / < 0.001 ns |
| 1 year | 23 / 6.9 ns | < 0.001 ns | 139 / 1 ns | 22 / < 0.001 ns |
| 3 years | 87 / 32 ns | < 0.001 ns | 177 / 1 ns | 88 / < 0.001 ns |

  The edge error reached the served time stamps through the software PTP measurement (without loopback, or for seconds without a loopback pulse) and through the model clock (socket path); the loopback measurement itself does not use the edge time. The phase of the system clock only steers `adjtime()` (µs resolution). Not tested on the board yet.

### 4.36 Native NMEA reader (2026-10-26)

Option (b) of the review of 2026-10-23, stage 1: the GNSS code without the Arduino core and the SparkFun library. This stage replaces the NMEA protocol path; the U-Blox UBX protocol still uses the SparkFun library and the Arduino serial port (stage 2: native UBX parser for M10, stage 3: Arduino and SparkFun removed).

- **Reader** (`main/custom/gnss_nmea.cpp`, `gnss_nmea.h`): ESP-IDF UART driver on UART1 (the port of the former `HardwareSerial(1)`) with a 2 kB receive buffer (the Arduino port had 256 B, review Z-03) and an event queue; its own task `gnss_nmea` (priority 10, core 0, below every timing task; stack `GNSS_NMEA_Reader_Task_Stack_Size` = 3072 B, with an entry in the stack measurement of `CALCULATE_STACK_SIZES_ENABLED`). The bytes are parsed one by one: `$` starts a sentence, CR or LF ends it, a byte outside the printable range drops it. The checksum is required as before (4.28). Nothing is sent to the receiver (G-01; decided again on 2026-10-26: the firmware stays passive towards the GT-U16). On a FIFO or buffer overflow the receive buffer is emptied and counted.
- **RMC** as before (status A, mode indicator of 4.32, date and time). The reader keeps the latest RMC with its reception time, so the validity check every second (`current_gnss_timing_is_valid()`) asks whether the latest RMC is valid and at most 1.5 s old instead of waiting up to 1.2 s for the next sentence (review Z-03: validity kept continuously).
- **Label after the PPS edge** (`acquire_sync_candidate()`): as before the receive buffer is emptied right after the edge and the first valid RMC within 950 ms labels it. New: a flush generation. `gnss_nmea_flush()` empties the driver (ring buffer and FIFO) and then increments the generation; the reader reads the generation before it reads bytes, and a sentence counts only if all its bytes carry the generation of its start. A sentence whose reception had started before the edge is therefore dropped even if it ends after the flush (before, its rest without `$` was dropped by the same rule; the generation also covers bytes the reader had already taken from the driver).
- **GGA, optional** (the receiver has to be set to send it; the GT-U16 is configured by hand with `$CFGMSG,0,0,1`): fix quality (field 6) and satellites in use (field 7). While GGA sentences arrive (the latest at most 2 s old), an RMC only counts if the fix quality is 1 to 5 (0 no fix, 6 dead reckoning, 7 manual, 8 simulator are refused). Without GGA the RMC alone decides, as before. The satellites in use go to the MQTT report (with GGA; without it the satellites in view of GSV, as before).
- **Minimum satellites in use before stratum 1** (new web setting, GNSS and PPS panel, *Validity of the GNSS time*, `gnss_min_satellites`, 0 to 50, default `gnssMinimumSatellites` = 0 = off, takes effect at once): the GNSS time only counts while the position solution uses at least this many satellites. NMEA protocol: needs GGA in addition to RMC; while the setting is on and no GGA arrives, no RMC counts (stratum 16). U-Blox UBX protocol: `numSV` of NAV-PVT (`getSIV()`) in the sync candidate and the validity check. Settings blob version 10 (versions 5 to 9 are still read; the field then gets its default). The panel heading *After a start* is now *Validity of the GNSS time* and holds both GNSS validity settings.
- **RMC phase** (review Z-01 hardening): the reader notes when each RMC was received after the last PPS edge (esp_timer, includes the reading delay of a few ms). The status page shows the latest value and the range since the start (`gnss_rmc_after_pps_ms`, `_min_ms`, `_max_ms` in `/api/state`); with GGA also the satellites in use and the fix quality (`gnss_satellites_used`, `gnss_fix_quality`). Debug output once a minute: phase, sentences, checksum errors, overflows.
- **Moved:** the NMEA helpers (field access, checksum, RMC, GSV) from `main.cpp` to the module. The NMEA fallback of a u-blox receiver still reads through the SparkFun serial port (`wait_for_ublox_nmea_rmc_time()`) until stage 2.
- **Not changed:** the PPS interrupt, the PPS task, the NTP paths and the UBX path. The UART interrupt is installed from the same task as before (the Arduino port used the same ESP-IDF driver).
- **Size:** image +11 kB (UART driver functions linked for the native path; stage 3 removes the Arduino core), DIRAM +680 B.
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1` and `DEBUG_ENABLED 1` (also the debug code of the module), the page against a local mock (field, default, range check, Apply, status lines, German, `I18N.missing` empty), and a Python model of the reader: RMC alone, mode N, without mode field; GGA quality 0 and 6 refused, 1 accepted, GGA stops: RMC alone again after 2 s; minimum satellites with and without GGA; a sentence started before the flush is dropped, the next one counts; buffered bytes before the flush are gone; 2000 runs with random chunking and noise bytes never accept the sentence from before the flush; wait semantics without generation; corrupted checksum. Not tested on the board yet.

### 4.37 Native UBX protocol for u-blox M10 (2026-10-26)

Option (b) of the review of 2026-10-23, stage 2: the U-Blox UBX protocol without the SparkFun library. Only M10 and newer receivers are supported (configuration with CFG-VALSET / CFG-VALGET, no legacy CFG messages), as decided on 2026-10-23. Stage 3 removes the Arduino core and the SparkFun submodule.

- **One UART reader for both protocols** (`main/custom/gnss_uart.cpp`, `gnss_uart.h`; the reader of 4.36 moved out of `gnss_nmea.cpp`, which keeps the sentence processing): `$` up to CR or LF is a sentence for `gnss_nmea`; `B5 62`, class, ID, length (at most 512 bytes, longer frames are skipped), payload and Fletcher checksum is a frame for `gnss_ubx`. The flush generation of 4.36 covers both, and a sentence or frame in progress is now dropped as soon as the generation changes: a frame cut by the flush at the PPS edge would otherwise take the following bytes, possibly the awaited NAV-PVT, as its payload (found with the Python model). New: the baud rate of the running UART can be changed (`uart_set_baudrate()`, the receive buffer is emptied), bytes can be written (UBX commands only), and the traffic since a reset is counted (bytes, sentences and frames with a correct checksum, the first 32 bytes) for the baud rate detection. Task `gnss_uart` (priority 10, core 0 as before; `GNSS_UART_Reader_Task_Stack_Size` = 3072 B, renamed from `GNSS_NMEA_Reader_Task_Stack_Size`).
- **UBX module** (`main/custom/gnss_ubx.cpp`, `gnss_ubx.h`): one command at a time (mutex); the reader task hands the answer over (poll response, ACK-ACK, ACK-NAK). CFG-VALSET (up to 8 keys in one transaction, waits for ACK or NAK), CFG-VALGET (one key from RAM), polls of SEC-UNIQID and MON-VER. The periodic messages NAV-PVT, NAV-TIMEUTC and TIM-TP (the last four) are kept with their reception time.
- **Configuration** (RAM and battery backed RAM as before, never the flash of the receiver):
  - Baud rate ladder and NVS identity as before (`setup_gnss_ublox_receiver()` unchanged). The baud rate command is CFG-VALSET of CFG-UART1-BAUDRATE; its ACK may come at the new rate and is only reported, as before.
  - Connection test at a baud rate: CFG-VALGET of CFG-UART1INPROT-UBX (the test of the library's `isConnected()`), up to three tries of 1.1 s (the library: 2.5 s each, and a second round with `assumeSuccess`). A receiver that does not answer but sends valid NMEA or UBX traffic counts as found with the NMEA fallback, as with `assumeSuccess` before.
  - Ports: I2C output off, UART2 output off (NAK on the M10, only reported), UART1 sends UBX and NMEA. Dynamic model Stationary (read, written if different, read back).
  - New: NAV-PVT, NAV-TIMEUTC and TIM-TP on UART1 once per navigation epoch (CFG-MSGOUT, one command per message so an unknown message does not stop the others). Without NAV-PVT the NMEA fallback is used; without NAV-TIMEUTC `validUTC` is not checked; without TIM-TP there is no qErr correction (console warning in each case).
- **Identity unchanged:** SEC-UNIQID as upper case hexadecimal text of 5 bytes (version 1) or 6 bytes (M10, version 2), MON-VER as `MOD|firmware type|FWhigh.low|PRhigh.low` with `NONE` when MOD is missing: the same text as the library produced, so a stored identity still matches and the fast start path stays.
- **Label after the PPS edge** (`acquire_sync_candidate()`): the receiver sends NAV-PVT itself after the solution of the epoch of the pulse; the rule is now the same as for RMC: flush at the edge, the first NAV-PVT after it within 950 ms labels the edge (before: a poll 300 ms after the edge with up to 600 ms wait). `nano` rounds to the whole second (in practice a few ns to µs).
- **Validity** (Z-04 complete): fix type 3, 4 or 5, `gnssFixOK`, `validDate`, `validTime`, `fullyResolved`, `confirmedTime` when `confirmedAvai`, the minimum satellites (`numSV`), and new **`validUTC` of NAV-TIMEUTC** of the same epoch (required once NAV-TIMEUTC is switched on). The check every second takes the latest NAV-PVT (at most 1.5 s old) instead of a poll with up to 1.5 s timeout. The debug output names every reason that is not met.
- **NMEA fallback of a u-blox receiver:** read by the same reader as the NMEA protocol (`wait_for_ublox_nmea_rmc_time()` is gone), with the flush generation of 4.36. GGA counts as for the NMEA protocol.
- **qErr correction** (review G-03, option 2 of 2026-10-26: directly in the servo measurement): the receiver puts the time pulse on the grid of its own clock, a few ns off the exact second; TIM-TP announces this quantization error (`qErr`, ps) before the pulse (u-blox application note GPS.G6-X-11007).
  - Assignment to the pulse by the time of week of TIM-TP: on the UTC time base (edge second - 1980-01-06) mod 604800 s; on the GPS or Galileo time base plus the leap seconds (GPS - UTC, from `iTOW` of NAV-PVT against its UTC date and time once fully resolved). Other time bases (BeiDou, GLONASS) and `qErrInvalid` (flags bit 4; to be checked on the board) give no correction.
  - Sign: the sources disagree (pulse = second + qErr or second - qErr). It is learned: the innovation of the loopback measurement without correction is regressed on qErr (exponential window of 600 samples, from 120 samples and a qErr variance of at least 4 ns²). A slope within 0.3 of +1 or -1 switches the correction on with that sign (measurement - sign x qErr), a slope more than 0.5 away switches it off; after 60 s without qErr it starts again. A TIM-TP assigned to the wrong pulse shows no slope near ±1 and gives no correction.
  - Only the loopback measurement is corrected (its 12.5 ns capture shows qErr; the software measurement has about 100 ns noise), only with the U-Blox UBX protocol. With the NMEA protocol `gnss_ubx_qerr_for_edge()` is not called and the measurement is the same as before. The delay error of the software measurement is still learned against the uncorrected loopback measurement.
  - Status page (GNSS line, German texts in `i18n.js`): satellites of NAV-PVT, qErr correction *learning the sign*, *active (slope …)*, *off: no correlation (slope …)* or *no qErr*; JSON `gnss_pvt_satellites`, `gnss_qerr_state`, `gnss_qerr_slope`. Debug output: a qErr line in the PTP status and a UBX line of the reader once a minute (frames, NAKs, qErr matched / unmatched / invalid, leap seconds).
- **Not changed:** the PPS interrupt, the NTP paths, the NMEA protocol path (same parser, only moved), the baud rate ladder and the NVS data.
- **Stack:** PPS task frame 800 B (736 before, the qErr variables of the servo), plus 48 B for the qErr lookup in u-blox mode; the task stack stays 3584 B (fixed value), to be measured on the board. Reader task: frame 96 B, UBX frame handling 128 B.
- **Size:** image 68 kB smaller (0x18DE20 to 0x17CC90: the SparkFun library and the Arduino serial port are no longer linked; the Arduino core itself goes in stage 3), DIRAM 1.5 kB less (175 244 B).
- Verified: build (-O2, no warning in `main/`), compile check with `CALCULATE_STACK_SIZES_ENABLED 1` and `DEBUG_ENABLED 1`, stack frames from the disassembly, and a Python model: CFG-VALSET bytes (CFG-UART1-BAUDRATE 921600: `B5 62 06 8A 0C 00 00 03 00 00 01 00 52 40 00 10 0E 00 50 BF`), the reader with NMEA and UBX mixed, a corrupted checksum, a frame cut by a flush; leap seconds from NAV-PVT also across the week change; TIM-TP assignment on the UTC and GPS time bases, a TIM-TP of another pulse, BeiDou, qErrInvalid, Saturday / Sunday; qErr learning in the alpha-beta filter of the servo (alpha 0.25, beta 0.035, 5 ns measurement noise, qErr as the sawtooth of a 48 MHz grid): sign +1 and -1 learned after 119 s with slope 1.00, RMS 2.8 to 2.2 ns; with a hanging bridge 5.9 to 2.2 ns; TIM-TP of the wrong pulse: slope 0.29, never switched on; no qErr in the pulse: never switched on. Not tested on the board yet (an M10 is available).

### 4.38 Arduino core and SparkFun library removed (2026-10-26)

Option (b) of the review of 2026-10-23, stage 3 (review finding E-02): after 4.36 and 4.37 nothing used the Arduino core except `millis()`, `Serial.begin()` and `initArduino()`, and nothing used the SparkFun library.

- `main.cpp`: no `#include "Arduino.h"`. `millis()` (button hold time, wait for the GNSS fix, periodic GNSS refresh) is `uptime_ms()`: esp_timer in ms, 32 bit, so differences behave as before across the wrap. `Serial.begin(serialMonitorSpeed)` and its 100 ms wait are gone: the console is the ESP-IDF log on UART0 at `CONFIG_ESP_CONSOLE_UART_BAUDRATE` (115200), as it was before (`serialMonitorSpeed` is removed from `ESP32TimeServerSettings.h`). `initArduino()` is gone: with this `sdkconfig` (no PSRAM, no app rollback, no Bluetooth) it only set the default log level and could erase the NVS (4.28); the console warning `initArduino() skipped` no longer exists.
- `Arduino.h` put `using std::abs`, `std::min`, `std::max` and `std::round` into the global namespace; an unqualified `abs()` of a `double` or `int64_t` would now call the C `abs(int)`. Checked: the project code has no unqualified call of these (it uses `std::`, `fabs()`, `llabs()`, `llround()`).
- Build: `main/CMakeLists.txt` requires `esp_driver_gpio`, `esp_driver_ledc`, `esp_driver_sdmmc` and `esp_driver_uart` directly (arduino-esp32 brought them in before); `arduino-esp32` and `SparkFun_u-blox_GNSS_v3` are gone from `REQUIRES`, `espressif/arduino-esp32` from `main/idf_component.yml`, the hosted-mode workaround from the root `CMakeLists.txt`, `CONFIG_ARDUINO_VARIANT` from `sdkconfig.defaults`. The submodule `3rdparty/SparkFun_u-blox_GNSS_v3` (`.gitmodules`) and the wrapper `components/SparkFun_u-blox_GNSS_v3` are deleted (approved on 2026-10-26). `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y` and the relaxed warnings for third-party components (4.17) stay, only their comments changed.
- Managed components: 8 instead of 25 (hd44780, pcf8574, i2cdev, esp_idf_lib_helpers, cjson, led_strip, mdns, mqtt). The component manager updates `dependencies.lock` and `managed_components` at the next build; an existing `sdkconfig` loses the `CONFIG_ARDUINO_*` entries by itself.
- A clone needs no `--recursive` and no `git submodule update` any more. An existing checkout keeps an empty `3rdparty` folder and `.git/modules/SparkFun_u-blox_GNSS_v3` until they are deleted by hand.
- **Not changed:** the program logic; the timing code paths are the same source.
- **Stack:** STACK_TEXT
- **Size:** image 0x174510 (35 kB smaller than with 4.37, 105 kB smaller than with 4.36), DIRAM 173 106 B (2.1 kB less). The build shows no warning at all now (before: 39 lines from arduino-esp32, esp-modbus and SparkFun).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1` and `DEBUG_ENABLED 1`, stack frames from the disassembly. Not tested on the board yet.

### 4.39 Settings panels collapsed (2026-10-26)

Request of 2026-10-26: the settings panels are collapsed when the page is loaded and open with an arrow, as the *GPIO pins* areas inside them (4.23).

- `index.html`: the panels Network, GNSS and PPS, LCD and time zone, RGB LED, MQTT, NTP authentication, Firmware Update and Security are a `<details class="panel">` inside their card, the heading (`h2`) is the summary. Status, the action bar (Apply, Save Settings, Reboot, Factory reset) and the factory reset dialog stay as they were. A panel stays open while the page is open (the refresh every 10 s does not close it); a reload closes all of them.
- `app.css`: the arrow, hover and focus ring of the GPIO pin areas, slightly larger; a long heading wraps beside the arrow on a phone.
- `app.js`:
  - `markPanels()`: like the GPIO pin areas, the heading row of a panel shows how many visible values inside differ from the compiled default (`1 value differs from the default`, `{n} values differ from the default`) and *see the warning* while a warning inside is shown (the PTP loopback on a chip below v3.0). The note *Updates are off on the device* of the Firmware Update panel is a state, not a warning about a setting (`device-state`), and is not counted.
  - An input error opens the panel (and the GPIO pin area) of its field, so the field named in the message is not hidden: the checks before Apply / Save Settings now name their field (`fieldError()`), and an error of the device of the form `Field <name>: …` opens the panel of `<name>` (`revealField()`).
- German texts for the two new markers in `i18n.js`.
- Verified in the browser against a local mock of the JSON API: all eight panels closed after loading, markers (1 in Network, 3 in GNSS and PPS, none for the OTE state note), arrow turns when opened, an invalid MQTT reporting period, an empty TF card pin and an invalid MAC address each open their panel (and the GPIO area), German headings and markers (`I18N.missing` empty), phone width (375 px, no horizontal scroll) in light and dark mode. The path for errors of the device (`Field <name>:`) is checked by reading only (the mock accepts every request). Firmware build with the embedded page: no warning.

### 4.40 `NTP_CACHE_MATCH_PORT` removed (2026-10-26)

Request of 2026-10-26: with `NTP_CACHE_MATCH_PORT 1` the cache would have told interleaved clients apart by their UDP source port, which makes no sense: chrony and other clients use a new source port for every request, so a record keyed by the port never finds the previous exchange.

- `main/custom/ntp_cache.c`: the option, `port_matches()` and the stored `client_port` of the IPv4 and IPv6 records are removed; a record belongs to an IP address (as with the option at 0, the only value it ever had in this fork).
- `ntp_cache_find_or_create()`, `ntp_cache_update()` and their IPv6 variants have no `port` parameter any more; the three NTP paths (hardware IPv4 and IPv6, socket path) no longer pass it. The reply is still sent to the source port of the request.
- Behaviour and time stamps: unchanged. Memory: the IPv6 records are 8 bytes smaller (the port and its padding before the 8-byte fields), DIRAM 1 kB less (172 082 B); the IPv4 records keep their size (the port was in padding).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`.

### 4.41 PTP PPS loopback capture on the core of the PPS interrupt (2026-10-26)

Seen on the board on 2026-10-26: at boot `mcpwm_capture_channel_register_event_callbacks(402): install interrupt service for cap channel failed`, then `PTP PPS loopback: capture channel on GPIO 23 failed (ESP_ERR_INVALID_STATE), software measurement is used`.

- Cause: the capture channels of MCPWM group 0 share one CPU interrupt (`ESP_INTR_FLAG_SHARED_PRIVATE`, bound by the name of the module). It belongs to the core that allocated it first: `setup_pps_input()` in `app_main` (core 0). `esp_intr_alloc` refuses a further channel of the group from the other core with `ESP_ERR_INVALID_STATE`. The loopback channel is created in `configure_hardware_timestamps()`, called by the Ethernet setup task, which has no core affinity: on core 1 the loopback failed and the PTP clock was measured by the software readings only (about 100 ns noise and a learned constant error instead of the 12.5 ns capture). So the result depended on the core the scheduler picked at boot.
- `setup_ptp_pps_loopback()`: the callback registration (which installs the interrupt) and the enable of the loopback channel run in `ptp_loopback_capture_start()`. On the core of the PPS interrupt it is called directly, on the other core through `esp_ipc_call_blocking()` on the PPS core, as the IDF GPIO driver installs its ISR service. The channel itself, the PPS output of the EMAC and the error path (the channel is deleted, the interrupt is freed by IDF through IPC as well) are unchanged.
- `s_pps_isr_core` (core of the PPS interrupt) is now always set by `setup_pps_input()`, not only with `PPS_ISR_LATENCY_AUTO`.
- Not changed: the core and priority of both capture interrupts (the loopback interrupt was always meant to share the PPS interrupt on core 0), the PPS task, the timing values.
- Stack: `ptp_loopback_capture_start()` runs on the IPC task (1024 bytes, `CONFIG_ESP_IPC_TASK_STACK_SIZE`); its call chain needs about 0.5 kB (frames 32 + 80 + 144 bytes plus the allocator).
- Verified: build (-O2, no warning). Not yet on the board: after a restart the console should show `PTP PPS loopback on GPIO 23: the PTP clock is measured against the PPS in hardware` at every boot.

### 4.42 Diagnostics of the time keeping (2026-10-26)

Review 4.5; the user wanted the figures on the page as well, as long as the page stays clear. Nothing of it is in the time path: the PPS task hands values over once per second and never waits for a reader.

- New `main/custom/time_diag.*`:
  - **Phase series** for the stability: at every PPS edge with a PTP measurement the PPS task stores the phase of the free-running PTP clock (crystal) against the GNSS PPS: the measurement the servo uses (loopback, qErr corrected) minus the phase the frequency commands added since the start of the series (the same terms as in the prediction of the filter). The rate estimated at the start of the series is taken out as well, which keeps the stored floats small (a constant rate does not change ADEV or TDEV). Ring of 4096 floats (16 kB, one per second). A series starts again after a start of the filter, a step, a missing second or another measurement source. Outliers of the filter get the prediction, as the filter coasts on it; while the loopback is active, a second without a loopback pulse also gets the prediction (the software measurement has some 100 ns noise).
  - **ADEV and TDEV** (overlapping Allan deviation, time deviation from the modified Allan variance) for tau = 1, 2, 4, ... 1024 s, computed on request in the web server task (some 10 ms) directly on the ring: the reader leaves out the 8 oldest slots, which the PPS task cannot reach within the computation, so no copy and no lock is needed. Checked against a Python port: white phase noise of 3 ns gives ADEV 5.1e-9 at 1 s (theory sqrt(3) x 3 ns / tau = 5.2e-9) and TDEV 2.96 ns (theory 3.0 / sqrt(tau)) up to 1024 s; the sliding window sum of the TDEV equals the direct sum; the phase bookkeeping with servo commands and a wandering crystal reproduces the free-running phase to the measurement noise.
  - **RMS of the innovations** of normal filter updates over the last complete minute and the last hour (60 minute sums).
  - **Histogram of the PPS interrupt excess** (interrupt time minus the central value of its latency, the `jitter_excess` of the debug line): up to 0.25, 0.5, 1, 2, 5 us and above, plus the maximum; counted once the latency window is full.
- `main.cpp`:
  - `ptp_servo_update()` passes the phase and the innovation to `time_diag`; `ptp_diag_publish()` copies the servo values (offset, noise, rate, command, loopback counters, outliers, steps, qErr sign and spread) after every update and at the holdover. Readers take a mutex for the copy; if a reader holds it, the PPS task skips that copy instead of waiting.
  - NTP counters since the start, independent of MQTT: replies of the hardware path (interleaved, without hardware TX time stamp, review Z-07) and of the socket path (interleaved; requests of the device to itself are not counted), requests dropped by the load shedding. `ntp_cache` counts where an interleaved lookup found its exchange: in the record of the address (latest exchange), in the exchange map (an earlier exchange, review S-02), or not at all (the request is answered in basic mode; this includes all requests of basic mode clients).
  - `app_get_diag()` for the page; MQTT: the report has `current.time_quality` (measurement source, offset, noise, innovation RMS of the last minute and hour, rate, command, outliers, steps, missing loopback pulses). The base size of the report buffer grows by 260 bytes to 1426 bytes plus 76 bytes per client (`web_mqtt_report_size()`, `MQTT_REPORT_SIZE` 5226; the page shows the new formula). `tools/json_message_calculator.xlsx` is not updated.
- GNSS modules: `gnss_nmea` counts the RMC receptions per 100 ms after the PPS edge (histogram since the start); `gnss_ubx` keeps when NAV-PVT `fullyResolved` and NAV-TIMEUTC `validUTC` were first set and how often they were lost afterwards.
- Web page: new card *Diagnostics* below the status, collapsed like the settings panels; `GET /api/diag` is only requested while it is open (every 10 s). Left: PTP clock (measurement, offset, innovation RMS, rate, filter counters, PPS loopback, qErr with U-Blox, PPS interrupt), NTP (replies, interleaved lookup, basic mode TX delay, load shedding), GNSS receiver (UBX: frames, NAK, UART overflows, `fullyResolved` / `validUTC`; NMEA: sentences, checksum errors, UART overflows, RMC phase histogram). Right: table of ADEV and TDEV with the length and source of the series. Shares below 0.1 % are shown as "< 0.1 %". All texts in German as well; the MQTT hint mentions the time quality.
- Memory and stack: DIRAM 172 082 -> 190 266 B (16 kB phase ring), image 1 535 024 -> 1 554 518 B. Stack frames (release, before -> after): PPS task 800 -> 816 B (calls of 16 to 96 B added in the servo update), `ntp_server_task` 544 -> 560 B, hardware NTP path unchanged (400 B), `mqtt_build_report` 288 -> 400 B (copy of the servo values). The web server task (10 kB) computes the stability (frames 128 + 336 + 144 B).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the formulas against a Python port, the page against a local mock (NMEA and UBX variants, English and German with `I18N.missing` empty, 375 px in light mode, no horizontal scrolling). Not on the board yet.

### 4.43 PPS not followed without valid GNSS time (2026-10-26)

Review Z-05: when the GNSS time became invalid while the PPS kept running, the replies went to stratum 16 at once, but the servo kept following the PPS. A u-blox receiver can be set to output its time pulse without a fix; that pulse runs on the receiver's own clock and would steer the PTP clock. With a lost PPS the holdover path applies instead (4.25, 4.31). Both cases are now handled the same way.

- PPS task, after the spike rejection: once the time was set, every edge checks `gnss_recently_valid()`: the published sync state says the GNSS time was valid within `Gnss_Validity_Timeout_Us` (3.5 s), the same test as `gnss_synchronized` in `get_ntp_reply_status()`. At the first edge where it fails the PPS task enters the holdover exactly as for a lost PPS (`enter_pps_holdover()`, now shared with the PPS timeout: servo holdover, `s_pps_discipline_active` off, sync state *PPS not active*, estimators reset) and ignores the edges: no tick bookkeeping, no system clock correction, no model or servo update, no sync state update.
- The reply status takes the existing holdover path: the last edge used was within 3.5 s of the last valid GNSS time, so stratum 1 with growing root dispersion for the PPS holdover time of the settings, then as set (stratum of the settings while coasting, or no answers). Before, the replies went to stratum 16 at once in this case.
- Hysteresis: the edges are used again once the GNSS time has been valid for 5 s (`gnssTrustAfterUs`). The next edge then starts the estimators and the servo as after a PPS loss, including the re-lock without phase step (4.31).
- Console: `PPS not followed: GNSS time not valid (holdover)` and `PPS followed again: GNSS time valid for 5 s` (not only in debug builds; rare).
- Not affected: the start (before the time is set the GNSS validity is not known yet, nothing changes there), the synchronisation of the GNSS time sync task (it uses its own PPS queue), the GT-U16 in normal operation (it sends no PPS without fix: its PPS stops, and the PPS timeout applies as before). One change for any receiver: if the GNSS time is not valid for more than 3.5 s while the PPS keeps running (e.g. RMC sentences missing), the PTP clock now holds over and re-locks 5 s after the GNSS time is valid again, where it followed the PPS before; the stratum was 16 in this case before as well.
- Stack: the PPS task frame stays at 816 B (release); the shared holdover code is a lambda inlined in the task.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`. Not on the board yet.

### 4.44 u-blox time pulse and antenna status (2026-10-26)

Review G-03, the part left after 4.37: the time pulse configuration (CFG-TP) and the antenna status (MON-RF). Only for the U-Blox UBX protocol; nothing is written to the receiver, NMEA receivers are not touched.

- `gnss_ubx`: the CFG-TP keys of time pulse 1 (IDs of the u-blox M10 interface description: `PULSE_DEF`, `PULSE_LENGTH_DEF`, `PERIOD(_LOCK)_TP1` or `FREQ(_LOCK)_TP1`, `LEN(_LOCK)_TP1`, `TP1_ENA`, `SYNC_GNSS_TP1`, `USE_LOCKED_TP1`, `ALIGN_TO_TOW_TP1`, `POL_TP1`, `TIMEGRID_TP1`, `ANT_CABLEDELAY`, `USER_DELAY_TP1`) and `gnss_ubx_query_rf()`: poll of UBX-MON-RF, first RF block (antenna status and power, jamming state, CW jamming indicator, noise per ms, AGC count).
- Setup (`read_gnss_time_pulse()`, after the dynamic model): the keys are read from the RAM layer (CFG-VALGET, one key per request) and logged in one console line, e.g. `GNSS time pulse: on, period 1000000 us (without lock 1000000 us), length 100000 us (without lock 0 us), rising edge, UTC grid, cable delay 50 ns, user delay 0 ns`. Warnings: time pulse off; falling edge at the second (the PPS input captures the rising edge, the time would be off by the pulse length); period once locked not 1 s; not aligned to the second of GNSS time. A note when the pulse also runs without lock (then 4.43 applies). If a key is not answered, `GNSS time pulse: configuration could not be read (CFG-TP)`.
- GNSS time sync task: in its validity loop (once per second) MON-RF is polled every 10th time (300 ms timeout); answers are kept for 30 s.
- Status page, GNSS receiver (moved to the diagnostics with 4.54): `antenna OK, jamming none, AGC 62 % (MON-RF)` (antenna: starting, unknown, OK, short circuit, open; jamming: unknown, none, warning, critical). Diagnostics panel: the time pulse (period and length once locked and without lock, or *no pulse without lock*; edge, time grid, *not aligned to the second* if so, cable and user delay) and the RF values (noise, AGC count, CW jamming indicator). German texts added.
- Stack: GNSS time sync task frame 176 -> 192 B; the poll adds 32 + 192 B (`run_command` with the frame buffer) and the UART driver below it, less than the `printf` depth that set the measured size. Measure the task once more with `CALCULATE_STACK_SIZES_ENABLED 1`.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, page against the mock (UBX variant, English and German, `I18N.missing` empty). The key IDs and the MON-RF layout are taken from the interface description, not checked on the M10 yet.

### 4.45 lwIP and UART interrupt in IRAM (2026-10-26)

Review E-04. `sdkconfig.defaults`:

- `CONFIG_LWIP_IRAM_OPTIMIZATION=y`: the RX/TX functions of lwIP in IRAM (about 10 kB). The hardware NTP path does not go through lwIP, but the socket path, DHCP, neighbour discovery, mDNS and the web admin do; they run without flash cache misses now.
- `CONFIG_UART_ISR_IN_IRAM=y` (selects `CONFIG_VFS_SELECT_IN_RAM`): the UART interrupt stays active while the flash cache is off (NVS commits, OTE). With the own UART reader of 4.36 this is possible; before, a sentence or UBX frame arriving during a flash write could be lost (counted as a UART overflow in the diagnostics).
- Not taken: `CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION` (TCP, 17 kB; only the web admin and MQTT use TCP).
- Memory: DIRAM 208 226 B after this change (190 266 B after 4.42; 4.43 and 4.44 add little), 447 kB free. No stack changes.
- **The file `sdkconfig` has to be created again** (delete it, or set the two options with `idf.py menuconfig`): a `sdkconfig` that exists keeps its values, the defaults only apply to a new one.
- The other part of review E-03 / E-04, `CONFIG_SPI_FLASH_AUTO_SUSPEND`, is **not** switched on: ESP-IDF supports it only for the flash chips XM25xxD, GD25QxxE and FM25Q32 (and a few IDs in the vendor drivers); with another chip `esp_flash_init_default_chip()` fails at the start and the device does not boot. The boards have a GigaDevice GD25Q256 (`esptool flash-id`: manufacturer C8, device 4019, 32 MB; boot log `spi_flash: detected chip: gd`, `flash io: dio`); ESP-IDF 6.1 allows suspend only for the GigaDevice IDs C84016, C84017, C84018 and C84319, so auto suspend stays off (review E-03 closed, 2026-10-26).
- Verified: build (-O2, no warning) with the two options set. Not on the board yet.

### 4.46 Core dump of a crash (2026-10-26)

Review E-07. Until now a panic only printed its register dump on the console and restarted; in the field nothing was left.

- `partitions.csv`: new partition `coredump, data, coredump, 0xC20000, 0x20000` (128 kB) after `ota_1`. The existing partitions do not move (NVS, OTA data and both OTA slots stay as they are). **An OTE update does not write the partition table: flash it once over USB** (`idf.py partition-table-flash` or a full `idf.py flash`). Without the partition the firmware runs as before and logs `No core dump partition: crashes are not recorded`.
- `sdkconfig.defaults`: `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` (ELF format, the IDF defaults otherwise: checked at the start, at most 64 tasks, summary with 1 kB of the crashed stack). As with 4.45 an existing `sdkconfig` has to be created again.
- New `main/custom/crash_dump.*`: at the start (`crash_dump_init()` after the opening console messages) the partition is checked once (`esp_core_dump_image_check()`); a stored dump is logged with task, program counter and panic reason (`A core dump of a crash is stored: task ..., PC 0x..., ... (download it on the web admin page or with idf.py coredump-info)`) and its summary is kept. Reading the flash stops the cache for a moment, so the partition is not read again except for a download.
- Web admin: the diagnostics panel has a row *Last crash* (task, PC, reason, size, or *none recorded*) with *Download* (`GET /api/coredump`, the raw partition content `coredump.bin`, 2 kB blocks) and *Delete* (`POST /api/coredump/erase`, with confirmation). Decode it with `idf.py coredump-info --core coredump.bin --core-format raw` and the ELF of the same build. 21 routes (handler limit 22). German texts added.
- Memory: image 1 585 536 B, DIRAM 210 088 B (core dump code and buffers). No change of the time path or of task stacks (the summary is read on the heap once at the start).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the generated partition table, the page against the mock (dump present, delete). Not on the board yet.

### 4.47 Rollback of a failed firmware update (2026-10-26)

Review E-06: after an OTE update the new image was started and kept whatever happened. If it crashed at the start or did not get onto the network, the server stayed down until it was flashed over USB.

- `sdkconfig.defaults`: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. The bootloader starts an image that an update selected once in the state *pending verify*; if that image resets before it confirmed itself (crash, watchdog, any reset), the bootloader marks it invalid and starts the previous image. The bootloader grows by 0x80 bytes (0x5E10, 0x1F0 bytes left before the partition table). **The new bootloader has to be flashed once over USB** (`idf.py bootloader-flash` or a full `idf.py flash`); with the old bootloader nothing changes (the image is never *pending verify*, and nothing is checked).
- Self check (`start_app_update_check()`, right after the opening console messages; task `update_check`, 3 kB, only in this case, ends itself): every 2 s it looks at Ethernet connected, NTP server ready (hardware path and sockets), web admin running (`web_admin_running()`, not required with `WEB_ADMIN_ENABLED 0`) and data from the GNSS receiver (NMEA sentences or UBX frames since the start). Once all of them hold for 60 s, the image is kept (`esp_ota_mark_app_valid_cancel_rollback()`, console `Firmware update checked: the new firmware is kept`). If not within 15 minutes of the start, the console names what is missing and the previous image is started (`esp_ota_mark_app_invalid_rollback_and_reboot()`). The criteria are functions of the firmware, not of the sky: neither a GNSS fix nor the PPS is needed, so an update with the antenna unplugged is not rolled back.
- Restarts requested on the web admin page (Apply with restart, Reboot, factory reset, the network confirmation time-out, restart with updates enabled) keep a new image that is still being checked: the page worked, and the bootloader would otherwise take the restart as a failure. A restart with the button during the check starts the previous image.
- While the running image is being checked, ESP-IDF refuses a new upload (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`); the page and `tools/ote_upload.py` get `409 The running firmware is still being checked after its update (at most 15 minutes): upload again afterwards`. The status page shows `new after an update: being checked (at most 15 min)` next to the firmware version. German texts added.
- Images flashed over USB are not checked. No eFuse is involved (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` stays off).
- Verified: build (-O2, no warning; bootloader 0x5E10), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the status line against the mock. Not on the board yet.

### 4.48 `driver` no longer required by the main component (2026-10-26)

Review E-09. In ESP-IDF 6.1 the component `driver` only holds the legacy I2C, touch sensor and TWAI drivers; the code of `main` uses none of them (its driver headers are `driver/gpio.h`, `ledc.h`, `mcpwm_cap.h`, `sdmmc_host.h` and `uart.h`, all from the `esp_driver_*` components named since 4.38). `driver` is removed from `MAIN_REQUIRES` in `main/CMakeLists.txt`. It is still part of the build because the I2C component of the LCD (`esp-idf-lib__i2cdev`) requires it itself. The image is the same size. Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`.

### 4.49 Last holdover in the diagnostics (2026-10-26)

Request of 2026-10-26 after the board test of the 10 minute holdover: chrony on the DiskStation showed offsets of +0.4 to +26 us after the PPS came back, far within its network noise (about 18.5 us). The board itself measures the time error of the holdover when the PPS comes back: the mapping of the PTP clock ran on through the holdover, and the first measurement after it is the residual that the re-lock ramp (4.31) then removes. Until now it was only visible while the ramp ran, or in the debug log.

- Servo (`ptp_servo_state_t`): `ptp_servo_holdover()` notes the start of a holdover (also the one of 4.43 without valid GNSS time). `ptp_relock_residual()` with continuity ends it: duration, end, and the time error *served time - true time* = -residual. A loopback start in the first seconds after the re-lock (the first measurement after a start comes from the software readings) measures it once more with the 12.5 ns capture; the first normal filter update closes the measurement. A holdover that ends with more than 100 us (the time is set instead of ramped, or the PTP clock is stepped) is marked as such.
- `ptp_diag_publish()` copies it; `/api/diag` has `holdover_running_s`, `holdovers`, `last_holdover_end_s`, `last_holdover_s`, `last_holdover_error_ns`, `last_holdover_stepped`.
- Diagnostics panel, PTP clock: row *Last holdover*: `running for 00:03:12`, or `00:10:00 long, ended at uptime 1 d 01:20:00: served time -812.6 ns off when the PPS came back (1 since the start)`, or `none since the start`. German texts added.
- Stack: PPS task frame unchanged (816 B). Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, page against the mock (English, German). Not on the board yet.

### 4.50 No clock step after a GNSS loss while the clocks coast (2026-10-26)

Board test of 4.49: with the GNSS antenna and the PPS disconnected and connected again, the diagnostics row *Last holdover* kept counting (`running for ...`) long after the loopback was used again.

- Cause: after more than 30 s without valid GNSS time the GNSS time sync task acquires the time again. Its check compares the GNSS second with the clock; with the right second it skipped the step only while the PPS discipline was active. After the loss it is not: since 4.43 the PPS is followed again only 5 s after the GNSS time is valid, and the GNSS time becomes valid by this very check. So the task set the system clock from the PPS-aligned GNSS time and `synchronize_hardware_clock()` set the PTP clock from the system clock (us resolution) and started a new epoch. The servo then started its estimate without continuity: the re-lock ramp of 4.31 did not happen (the served time jumped by the holdover error plus the error of the setting), and the holdover of the diagnostics never ended. Before 4.43 the same could happen whenever the check came before the first PPS edge.
- Fix (GNSS time sync task): with the right second the clock is not stepped as long as the model clock is running (its one hour of holdover not over) and either the PPS discipline is active or the PTP mapping is valid (it stays valid through the holdover). The PPS task then re-locks without a phase step once it follows the PPS again. A wrong second, a holdover of more than one hour or an invalid mapping still step the clock as before.
- Diagnostics: a holdover also ends when the estimate starts again without continuity (new epoch of the PTP clock, e.g. an Ethernet recovery, or a step); the row then says *the time was set instead of ramped*. A re-lock without a valid mapping is shown the same way instead of an error of 0.
- Stack: PPS task frame 816 -> 800 B, GNSS time sync task 192 B (unchanged). Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`. Not on the board yet.

### 4.51 RMC phase only within 1 s of the PPS edge (2026-10-26)

Board test of 4.50: the RMC histogram of the diagnostics showed 1.6 % at 900 ms and later. With the PPS lost (or ignored while the GNSS time is not valid, 4.43) the RMC sentences kept coming and were measured against the last edge within the window of 1.5 s, so they counted about 1036 ms late; they also raised the maximum of the RMC phase on the status page. A real phase of more than 1 s cannot occur (the next edge comes first). `gnss_nmea`: the window is 1 s (`RMC_PHASE_WINDOW_US`); the last bin of the histogram is 900-999 ms. Verified: build (-O2, no warning). Not on the board yet.

### 4.52 Robust check of the 80 MHz PLL source of the PTP clock (2026-10-26)

Board finding: one start logged `PTP clock: the 80 MHz PLL source does not work, using the 40 MHz crystal (40 ns steps)`, where the start before showed the PLL source (13 ns steps). The `sdkconfig` made again from the defaults differs from the earlier one only in the PPP options, so the cause is at run time.

- The source is taken only if the PTP clock runs at its rate: two readings 20 ms apart, compared with the local timebase within 0.1 % (20 us). Each reading was a local time and a PTP reading one after the other; an interrupt, or the cache stopped by a flash access of another task on the other core (NVS at the start), between the two made the check fail, and the crystal was used for the whole run (the source is kept because the frequency command refers to its base value). A PTP reading across the second rollover could do the same.
- Now each PTP reading is bracketed by two local readings and taken again if the bracket is longer than 5 us (`ptp_clock_reading()`), and the check is tried up to three times. A clock that does not run at its rate fails all tries.
- The warning names the step that failed: `could not be switched on (<error>)` (clock gate), `the EMAC refused ... (<error>)`, or `does not run at its rate (<n> ns in <m> ns)`.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`. Not on the board yet: several starts should all log `PTP clock: 80 MHz PLL source, time stamp steps of 13 ns`; if not, the warning shows the step.

### 4.53 qErr flagged invalid by the receiver shown as such (2026-10-26)

First board test with the u-blox M10 (UBX protocol): stratum 1, NAV-PVT, `fullyResolved` and `validUTC` after 40 s, time pulse 1 s / 100 ms / rising edge / UTC grid / no pulse without lock, cable delay 50 ns. Every TIM-TP (185 of 185) was flagged *qErrInvalid* (flags bit 4), so the qErr correction stayed off as intended, but the status line said `no qErr (TIM-TP missing or PTP loopback off)`.

- Status page: a new qErr state 4, `no qErr: the receiver flags it invalid (TIM-TP)`, when TIM-TP arrives but none had a valid qErr since the start.
- Diagnostics, qErr row: `latest TIM-TP: qErr <q> ps, flags 0x<f>`, the values as received whatever the flags (`gnss_ubx_status_t::last_tp_qerr_ps`, `last_tp_flags`). They settle the open question of 4.37: if the qErr values vary by some ns while bit 4 is set, the bit has the other meaning on this firmware; if they stay 0, the receiver gives no qErr (then the correction is not available with this module).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the page against the mock. Not on the board yet.

### 4.54 qErr and MON-RF lines moved to the diagnostics (2026-10-26)

Request of 2026-10-26: the lines `no qErr: ...` (qErr state) and `antenna ..., jamming ..., AGC ... % (MON-RF)` were too detailed for the status. The status page keeps protocol, baud rate, dynamic model and satellites (and the RMC phase with the NMEA protocol). In the diagnostics panel the qErr row starts with the qErr state (learning, active with slope, off, flagged invalid, missing), followed by its details, and the MON-RF line of the GNSS receiver now holds antenna, jamming, AGC (in % and as the count), noise and the CW jamming indicator. The values come from `/api/state` (states) and `/api/diag` (counts) as before; no firmware change except the embedded page. German texts adjusted (`I18N.missing` empty). Verified: build, page against the mock (UBX, English and German).

### 4.55 Board result: the M10 gives no qErr (2026-10-26)

Long run with the u-blox M10 (UBX protocol, firmware `54e308a`): the diagnostics show `no qErr: the receiver flags it invalid (TIM-TP)`, TIM-TP matched 0, missing 0, invalid 117, correction sign none with 0 samples, and `latest TIM-TP: qErr 0 ps, flags 0x1B`.

- Flags `0x1B`: bit 0 time base UTC, bit 1 UTC available, bits 2-3 = 2 RAIM active, bit 4 qErrInvalid. The time pulse is on the UTC grid as configured (4.44).
- qErr stays exactly 0 while bit 4 is set: the receiver does not give a quantization error; bit 4 has its documented meaning. This settles the open question of 4.37 and 4.53: `gnss_ubx_qerr_for_edge()` stays as it is, no change of the bit evaluation.
- Consequence: with this module the PPS keeps its quantization sawtooth; the loopback measurement is not corrected (measured on the board: innovation RMS about 10 ns, ADEV(1 s) 1.3e-8, similar to the GT-U16). The correction with the learned sign (4.37) stays in the code and switches itself on with a receiver that gives a valid qErr; without one it costs nothing but the TIM-TP lookup.
- No firmware change. The diagnostics row already shows this state (4.53, 4.54).

### 4.56 Diagnostics of the basic mode TX delay (2026-10-27)

Board test: the learned TX delay of the hardware path (software T3 to the hardware TX time stamp) was 19.4 us after 5126 values with the M10 and 25.0 us after 1737 values with the GT-U16, varying between about 15 and more than 20 us; the run of 2026-10-19 (other board, firmware before the review series, 7.4) settled at 14 to 15 us. The GNSS module is not the cause. All runs start at 23 to 25 us and fall over hours. The running mean alone cannot tell whether the transmission itself got slower or is delayed now and then.

- `update_tx_delay_estimate()` (hardware NTP task, after the transmission) also keeps, for unauthenticated replies per family: the minimum and maximum sample since the start, and per 100 ms phase of the second of the software T3 a running mean (the same as the estimate: plain mean for 32 samples, then weight 1/32) with its count. The running mean of the estimate moved into `tx_delay_running_mean()`; the compensation is unchanged.
- How to read it: a minimum near 14 us with a varying mean points to occasional delays (interrupts, flash access of the other core); if they belong to a phase of the second (e.g. the GNSS data after the PPS), that bin is higher. A chrony client locked to the server asks at almost the same phase each time, so one or two bins fill and others follow as the phase moves. A minimum above 20 us means the path itself is slower than in 7.4.
- `/api/diag`: `tx_delay_min_ns` and `tx_delay_max_ns` (arrays IPv4, IPv6; 0 = no value), `tx_delay_phase` (per family `ns` and `n`, 10 bins). Diagnostics panel, row *Basic mode TX delay*: the former line, then per family with values `IPv4: minimum … us, maximum … us` and `IPv4 by phase of the second (mean, values): 400-499 ms 21.3 us (9840), …`. German texts in `i18n.js`.
- Cost: 176 B of static RAM; the time path up to the transmission is unchanged. Stack: `update_tx_delay_estimate()` now has a frame of 48 B for the 64-bit division (it is called after the transmission, beside the deeper EMAC and SHA-256 call chains, so the measured stack of the hardware NTP task stays valid); the frames of the hardware NTP handler (400 B), `app_get_diag()` (400 B) and `handle_diag()` (256 B) are unchanged; `web_diag_t` is on the heap.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, stack frames from the disassembly, the page against the mock (English, German, phone width; `I18N.missing` empty). Board result in 4.57.

### 4.57 Board result: the basic mode TX path is not slower (2026-10-28)

Board run with the GT-U16 and the firmware of 4.56, chrony over IPv4 and IPv6 (`xleave`):

| Family | Running mean | Minimum | Maximum | Mean per 100 ms phase of the second |
|---|---|---|---|---|
| IPv4 | 19.5 us (1359 values) | 13.9 us | 75.1 us | 17.0 to 21.1 us, about 85 values per bin, 544 in 400-499 ms (phase of the chrony client, 17.6 us) |
| IPv6 | 21.2 us (827 values) | 15.2 us | 63.9 us | 19.4 to 24.0 us, about 80 values per bin |

- The minimum is the same as the mean of the run in 7.4 (14.3 us, other board, firmware before the review series): the transmission itself is not slower. The higher mean comes from occasional delays of single replies (up to about 60 us more).
- No phase of the second stands out: the differences between the bins are within the scatter of a running mean of about 32 values, and the bin 0-99 ms (GNSS data after the PPS) is not higher. The delays are not caused by the PPS, the PPS task or the GNSS UART. Likely causes, not separated: interrupts on core 1, cache misses in the flash code between the T3 reading and the transmit call (reply, checksums; the EMAC transmit functions are already in IRAM with `CONFIG_ETH_IRAM_OPTIMIZATION`), and a frame of another path on the link.
- IPv6 is about 1.3 us slower than IPv4 (the longer frame and the checksum over the 128-bit addresses).
- Consequence: only for clients without interleaved mode; their T3 carries the scatter of the delay around the compensated mean (a few us). Interleaved clients use the hardware TX time stamp and are not affected. The open point of the review is closed; no firmware change.

### 4.58 Root dispersion growth in the holdover 15 ppm by default (2026-10-28)

Review Z-22. After a PPS loss the advertised root dispersion grows with the time since the last pulse (4.25, setting *Root dispersion growth*). The default was 100 ppm (0.1 ms per second): in the board test the root distance reached about ±29 ms after 5 minutes of holdover, and chrony no longer selected the server, although the measured time error was far smaller (-0.14 us after 3:14 min with a warm board, 4.49; about 1 ppb).

- `ntpHoldoverDispersionPpm` (`ESP32TimeServerSettings.h`, the default of the web setting) is now 15 ppm = 0.015 ms per second, the frequency tolerance PHI that RFC 5905 assumes for a clock. It stays conservative: the learned rate is compensated, and only the change of the crystal during the holdover remains (temperature, typically well below 1 ppm). After 5 minutes the root dispersion has grown by 4.5 ms, after the default stop of 60 minutes by 54 ms; the cap of 1 s is unchanged.
- Saved settings keep their value: a board with saved settings stays at 100 ppm until the setting is changed on the page (0.015) and saved. The layout of the settings is unchanged.
- The base root dispersion `NTP_ROOT_DISPERSION` (1, fixed value) and the stratum 16 replies are unchanged.
- Web page: the hint of the setting names 15 ppm and RFC 5905 (German text in `i18n.js`). The default shown below the field comes from the firmware.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, the page against the mock (English, German, `I18N.missing` empty). No stack change (a constant).

### 4.59 Stack reserve of the tasks and the heap in the diagnostics (2026-10-28)

Review E-14. The stack sizes were set from measurements with `CALCULATE_STACK_SIZES_ENABLED 1`, a debug build; in normal operation nothing showed how close a task came to its limit after a change, a rare path or weeks of run time.

- Each watched task notes its own stack high water mark (`uxTaskGetStackHighWaterMark(nullptr)`: the bytes of its stack it has never used since its start) at the points where the debug build logged its stack use, now `task_stack_checkpoint()`: at most once per 10 s per task (`note_task_stack()`, interval in units of 1.024 ms, 32-bit atomics), and the smallest value is kept. The painted stack is scanned from its end without a lock (a few us for some kB). With `CALCULATE_STACK_SIZES_ENABLED 1` the checkpoint also logs as before.
- Watched: `eth_recovery` (and its worker, same size), `gnss_recovery`, `gnss_uart`, `gnss_time_sync`, `hardware_ntp`, `LED_LCD_Button`, `mqtt_service`, `ntp_cache_purge`, `ntp_server`, `ote_service`, `pps_discipline` (table `Watched_Tasks`, the stack size constants), plus the web admin task `httpd` (10240 B), which reads its own value when it answers `/api/diag`. A task that does not run in this configuration or has not reached a checkpoint yet is left out. Tasks note their own value only: no other task's stack is read, so a task that ends or is deleted cannot leave a dangling handle.
- Internal heap: free size, lowest free size since the start, and the largest free block (`MALLOC_CAP_INTERNAL`; the largest block walks the free list once per request).
- `/api/diag`: `tasks` (`name`, `size`, `free`), `heap_free`, `heap_min_free`, `heap_largest_block`. Diagnostics panel, group *Device*: rows *Heap* (`free … kB, lowest since the start … kB, largest block … kB`) and *Task stacks* (`<name>: at least <free> of <size> B free (<share>)`, the tightest first, in red below 10 %). German texts in `i18n.js`.
- Cost: 88 B of static RAM; `note_task_stack()` has a frame of 32 B at the loop level of each task (far from the deepest call chains); the frames of the tasks (PPS 800 B, GNSS sync 192 B, `ntp_server` 560 B, hardware NTP handler 400 B) and of `app_get_diag()` (400 B) and `handle_diag()` (256 B) are unchanged; the time path is unchanged (the hardware NTP task reaches its checkpoint after the reply).
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`, stack frames from the disassembly, the page against the mock (English, German, phone width; `I18N.missing` empty). Not on the board yet.

### 4.60 MQTT reports after 49.7 days of uptime (2026-10-28)

Found in the code review of the changes since the review (4.62). The FreeRTOS tick count is 32 bits at 1 kHz and wraps after 49.7 days.

- `mqtt_service_task()` compared `xTaskGetTickCount() >= next_report`. Shortly before the wrap `next_report` (tick + period) wraps to a small value while the tick count is still large: the condition held at once, and after each report `next_report += period` stayed small, so the task sent a report every pass (about every `MQTT_QUEUED_PUBLISH_DELAY_MS`) until the tick count itself wrapped, up to one report period long. Now the signed difference `(int32_t)(tick - next_report) >= 0`, correct across the wrap.
- The wait for the MQTT event queue in the start-up health test used `tick < deadline` the same way (only at the start, so it could not hit the wrap); changed to the signed difference as well.
- The other time comparisons were checked: `uptime_ms()` users take differences of 32-bit values (correct across the wrap), the tick waits of the MQTT restart notice take differences, absolute times are `int64_t` from `esp_timer`. No time path change.
- Verified: build (-O2, no warning), compile check with `CALCULATE_STACK_SIZES_ENABLED 1`.

## 5. Security and open ports

### 5.1 Open ports

With the current settings (OTE, MQTT off):

| Protocol | Port | Who | Remark |
|---|---|---|---|
| UDP | 123 | raw NTP path (hardware time stamps) and lwIP sockets (IPv4 `0.0.0.0`, IPv6 `::`, IPv6 link-local) | the sockets get what the raw path leaves to lwIP |
| **TCP** | **443** | **web admin page (HTTPS only, IPv4 and IPv6 on one dual-stack socket); closed with `WEB_ADMIN_ENABLED 0`** | **new on 2026-10-21 (4.23); TLS 1.2 / 1.3, allow-list checked before the handshake, at most 3 connections** |
| UDP | 68 | lwIP DHCP client | only while DHCP is used |
| UDP | 32769 | control socket of `esp_http_server` | bound to 127.0.0.1 only, not reachable from the network; not opened with `WEB_ADMIN_ENABLED 0` |
| ICMP / ICMPv6 | - | lwIP | ping replies, IPv6 neighbour discovery, MLD; ARP |
| TCP | all others | none | a SYN gets a RST |

UDP 5353 (mDNS) is always open (4.24). OTE updates use TCP 443 (no port of their own since 4.24; before: UDP 3232 and a TCP connection from the device to the PC). With `MQTT_ENABLED 1` only an outgoing TCP connection to the broker. Check: `nmap -sU -p 1-65535 <ip>`, `nmap -sT -p 1-65535 <ip>`, the same with `-6`.

### 5.2 Raw NTP path filter (review)

Requests are answered on the raw path only if:

- the Ethernet destination is unicast and the source is unicast,
- IPv4: not a fragment, total length and header checksum correct, destination = the assigned IPv4 address, source not 0.0.0.0/8, 127.0.0.0/8, multicast/reserved/broadcast or the own address,
- IPv6: destination not multicast, source not multicast, unspecified or loopback, UDP checksum present,
- UDP: source port not 0, checksum correct (for IPv4 a checksum of 0 means "not used").

Everything else is handed to lwIP, which applies its own rules (broadcast requests, fragment reassembly, foreign addresses).

### 5.3 Exposure

- Amplification: none worth mentioning (mode 3 → mode 4, same size; no mode 6/7).
- Flooding: global load shedding (8500 or 14500 requests per second depending on the chip revision) and the Ethernet recovery task. A single host flooding above the global threshold still triggers drops for all clients; a flood with spoofed sources pushes real clients out of the interleaved cache (fallback to basic mode) only at about 1000 to 2000 answered requests per poll interval of the client (exchange map, 4.29; before 4.29 one spoofed request with the client's address was enough). Per-client rate limit (4.26, default 20 requests per second per IP address): a single host below the global threshold gets at most its limit answered, the rest is dropped before the reply path. Since 4.30 requests with a T2 cookie (the interleaved requests of the real client) have a bucket of their own, so a host sending with a client's address cannot exhaust it.
- Time spoofing: possible on the path without authentication; NTPv4 symmetric keys are available (SHA-256 truncated to 160 bit, constant time comparison). Unauthenticated requests are still answered when authentication is enabled (by design).
- OTE (if enabled, 4.24): upload over TLS with the login of the page or, for scripts, the web admin password (HTTP Basic, throttled like the login, refused while the factory password is in use, 4.27); the image is checked for project and chip revision and verified, but not signed: whoever knows the web admin password can replace the firmware. Use a long random password, keep OTE off when it is not needed, consider Secure Boot V2 and flash encryption.
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
| `PTP_RELOCK_TIME_CONSTANT_S`, `ptp_relock_max_slew_ppb` | PTP servo | 16 s, 5000 ppb | decay of the mapping residual after a holdover (4.31) |
| `PTP_PPS_LOOPBACK_DELAY_NS` | PTP servo | 0 | delay of the loopback path minus the GNSS PPS input path |
| `NTP_ROOT_DISPERSION` | `main.cpp`, top | 1 | advertised root dispersion, 1/65536 s per unit |
| `PTP_CLOCK_FROM_PLL` | after `PTP_FREE_RUNNING` | 1 | 1 = PTP clock from PLL_F80M (13 ns steps), 0 = 40 MHz crystal (40 ns steps) |
| `PPS_ISR_LATENCY_DEFAULT_US` | compensation block | 2.5 | used until a value is measured |
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
| `CACHE_TIMEOUT_MS_IPV4/IPV6` | `ntp_cache.h` | 1800000 | lifetime of a cache entry |
| `EXCHANGE_MAP_SET_BITS` / `EXCHANGE_MAP_WAYS` | `ntp_cache.c` | 9 / 4 | exchange map of the interleaved mode: 512 sets × 4 entries (32 kB), 4.29 |
| `ntpClientRateLimitPerSecond` | `ESP32TimeServerSettings.h` | 20 | default of the per-client rate limit (4.26), 0 = off |
| `RATE_LIMIT_BURST_US` | `ntp_cache.c` | 1000000 | burst of the per-client rate limit: one second worth of requests |
| `gnssMinimumFixAgeSeconds` | `ESP32TimeServerSettings.h` | 0 | default of the web setting *Minimum GNSS fix age before stratum 1* (4.32), 0 = off, recommended 750 |
| `gnssMinimumSatellites` | `ESP32TimeServerSettings.h` | 0 | default of the web setting *Minimum satellites in use before stratum 1* (4.36), 0 = off; NMEA needs GGA |
| `GNSS_UART_Reader_Task_Stack_Size` | `main.cpp`, top | 3072 | stack of the GNSS UART reader task `gnss_uart` (NMEA and UBX; 4.36, 4.37), to be measured |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `sdkconfig.defaults`, `sdkconfig` | y | -O2 |
| `WEB_ADMIN_ENABLED` | `ESP32TimeServerSettings.h` | 1 | 0 = no web admin page (TCP 443 closed) and compiled settings only (4.23) |
| `WebAdminPassword` | `ESP32TimeServerSettingsSecrets.h` | "password" | password of the web admin page and of `tools/ote_upload.py` (4.27) until one is set on the page; again after a factory reset |
| `DeviceName` | `ESP32TimeServerSettings.h` | "ESP32TimeServer" | default FQDN of the page (certificate name; its first label is the DHCP and mDNS host name) |
| `*_ENABLED` of LCD, LED, MQTT, NTP authentication, the pins, the network values, `timeZoneSpec`, `lcdTopLineTitle`, `display*`, `RGB_LED_Brightness_Percent`, `MQTTServerIPAddress` / `MQTTPort`, `MQTTUsername` / `MQTTPassword`, `symmetric_keys` | settings files | as before | defaults of the web admin page (4.23); saved page settings take precedence |
| `NETWORK_CONFIRM_TIMEOUT_US`, `MAX_SOCKETS`, `MAX_BODY`, `TASK_PRIORITY`, `TASK_CORE` | `main/web/web_server.cpp` | 120 s, 3, 4 kB, 3, 0 | network confirmation, connections, request body, server task |
| `PBKDF2_ITERATIONS`, `MAX_SESSIONS`, `FAILURES_BEFORE_LOCKOUT`, `FIRST_LOCKOUT_US`, `MAX_LOCKOUT_US` | `main/web/web_auth.cpp` | 10000, 4, 5, 30 s, 15 min | password hash and login throttling |
| `CONFIG_MBEDTLS_X509_CREATE_C`, `..._X509_CRT_WRITE_C`, `..._SSL_PROTO_TLS1_3`, `..._DYNAMIC_BUFFER`, `CONFIG_HTTPD_MAX_REQ_HDR_LEN`, `CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM`, `CONFIG_LWIP_IPV6_NUM_ADDRESSES`, `CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS`, `CONFIG_LWIP_DNS_SETSERVER_WITH_NETIF` | `sdkconfig.defaults` | y, y, y, y, 2048, y, 5, 2, y | needed by the web admin (4.23) |

## 9. Data stored in NVS

| Namespace | Key | Content |
|---|---|---|
| `isr_latency` | `ns` (int32), `fp` (uint32) | interrupt latency in ns, fingerprint of the configuration |
| `ptp_latency` | `ns` (int32), `fp` (uint32) | PTP write latency in ns, fingerprint of the configuration (only read and written with `PTP_FREE_RUNNING 0`) |

A value is only used if its fingerprint matches and it is within 0.5 to 20 µs; otherwise it is measured again. Other namespaces (`gnss_state`, `restart_evt`) are unchanged from the original project.

Web admin (4.23):

| Namespace | Key | Content | Factory reset |
|---|---|---|---|
| `webcfg` | `saved` (blob) | settings saved with "Save Settings": magic, layout version, size, flags, all settings incl. MQTT password and NTP keys, CRC-32 | erased |
| `webcfg` | `oneshot` (blob) | settings of "Apply" that need a restart; erased at the start that uses them | erased |
| `webauth` | `pw` (blob) | admin password: PBKDF2-HMAC-SHA256 hash, salt, iterations, CRC-32 | erased |
| `webauth` | `allow` (blob) | allow-list (at most 8 entries), CRC-32 | erased |
| `webtls` | `key`, `crt` (blob), `name` (string) | TLS private key (DER), self-signed certificate (DER), FQDN it was issued for | kept |

## 10. Console messages

Always printed (brief):

- `PPS ISR latency: using the stored value X us` / `no stored value for this configuration, using X us until it is measured`
- `PPS ISR latency measurement started ...`, the statistics line, `central value (mean of the lowest 75 %) = X us`, `X us applied (was Y us) and stored in NVS`
- With a minimum GNSS fix age (4.32): `GNSS fix older than N s and time checked: stratum 1 allowed`
- PPS without valid GNSS time (4.43): `PPS not followed: GNSS time not valid (holdover)`, `PPS followed again: GNSS time valid for 5 s`
- Firmware update (4.47): `New firmware after an update: it is kept once ...`, `Firmware update checked: the new firmware is kept`, `Firmware update check failed (...): starting the previous firmware`, `Firmware update: the new firmware is kept (restart requested on the web admin page)`
- Core dump (4.46): `A core dump of a crash is stored: task ..., PC 0x..., ...`, `No core dump partition: crashes are not recorded (flash the partition table once over USB)`
- U-Blox UBX protocol (4.44): `GNSS time pulse: on, period ..., length ..., rising edge, UTC grid, cable delay ... ns, user delay ... ns` and the warnings for the time pulse configuration
- `PTP clock: 80 MHz PLL source, time stamp steps of 13 ns` or a warning that names the failed step (4.52): `PTP clock: the 80 MHz PLL source could not be switched on (...)`, `... the EMAC refused the 80 MHz PLL source (...)` or `... the 80 MHz PLL source does not run at its rate (n ns in m ns)`, each `using the 40 MHz crystal (40 ns steps)`; firmware before 4.52: `... does not work ...`
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

| Ethernet recovery, ESP-IDF Ethernet driver, OTE over HTTPS (2026-10-21, 4.24) | `idf.py build` with ESP-IDF 6.1 and a new `sdkconfig` (no new warnings compared with the build before; image 49 kB smaller); `main.cpp` also compiled with `CALCULATE_STACK_SIZES_ENABLED 1` (forces `DEBUG_ENABLED`); image header parsing of `tools/ote_upload.py` against a built image | **not yet** |

| Scripted uploads with the web admin password, no OTE password / mDNS host name (2026-10-23, 4.27) | `idf.py build` with ESP-IDF 6.1 (profile v3.1, no warnings in the project code); `tools/ote_upload.py` against a local HTTPS mock of `/api/ote` (IPv4 and IPv6 literal, a name with two addresses of which the first does not answer, wrong password, `.local` / single-label / unknown names, `-4`, a UTF-8 password with space and colon; 12 checks) and its image parsing against the built image; the page (card, Apply body, German texts with `I18N.missing` empty) against a local mock of the JSON API | **not yet** |

| Web admin page (2026-10-21) | `idf.py build` with ESP-IDF 6.1, -O2 and the IDF warning flags, `sdkconfig` regenerated from `sdkconfig.defaults` (no new warnings in the project code); the page (login, status, apply with network confirmation, dark / light mode) in a browser against a local mock of the JSON API | **not yet** (nothing of 4.23 has run on the board yet) |

The transmit delay compensation of basic mode responses was measured on the board (7.4) but not checked with a basic mode client (chrony uses the interleaved mode).

## 12. Known limitations and open points

- The absolute accuracy against UTC is not verified; an independent reference would be needed.
- Systematic uncertainties: interrupt latency (now 62.5 ns resolution; ±0.5 µs before the follow-up), position of the PTP read inside the bracket (affects the software measurement of the PTP clock in both modes; removed by the loopback, 4.21), network path asymmetry (2.7 µs difference between the IPv4 and the IPv6 path of one board in an earlier run; 2.8 µs between the two new boards in 7.5, most likely the longer network path of the test board).
- The measurements are limited by the client: kernel RX time stamps and the OpenVSwitch bridge on the DiskStation add about 18.5 µs noise on the return path (7.5).
- The PTP clock rate against esp_timer: earlier comparisons showed +0.37 / -0.45 ppm, within what the method resolved. With `PTP_FREE_RUNNING 1` the servo measures the rate of the PTP clock itself, so this no longer matters; with `PTP_FREE_RUNNING 0` the error of T2 would grow by up to 0.4 µs over the second.
- `PTP_FREE_RUNNING 0` only: a frame time stamped shortly before a PTP write but converted after it uses the new compensation (error up to the drift of one second, about 33 µs with the -33 ppm oscillator of the test board; probability about 1 in 10000 samples, chrony filters it). Late PTP writes: about 15 per hour with a deviation of about -5.7 µs (7.4).
- The free-running servo and the loopback run on all four boards (loopback on GPIO 23); the board's own error is below what the DiskStation can resolve (about 0.19 µs). The PLL clock source (4.22) is verified on the board: `PTP clock: 80 MHz PLL source, time stamp steps of 13 ns` at every start (several restarts, 2026-10-26/27, after 4.52).
- `esp_timer_impl_get_counter_reg()` is an internal function of esp_timer. If a later IDF version renames it, the build fails at the link step; then set `FINE_LOCAL_TIMEBASE 0`.
- The calibrations assume the board is warm and the PPS locked. A run that does not finish starts again at the next boot.
- After a PPS loss the server stays at stratum 1 for the PPS holdover (default 2 s after the PPS counts as lost, 4.25; root dispersion growth 15 ppm by default since 4.58) and then answers as set (default stratum 16, answers stop 60 min after the loss). The PTP clock keeps the learned rate (simulated: below 1 µs in 5 minutes with a constant crystal). Board test 2026-10-26/27: stratum 1 in a holdover of 10 minutes with a growing root distance, the reach falls after the holdover as set; time error at the re-lock with a warm board -135.6 ns after 3:14 min (M10, 4.49).
- Leap second warnings (LI) from the GNSS are not passed on.
- If the GNSS receiver stops answering at run time, the recovery can end in `halt_with_display()`; the sync task then waits forever (stratum 16, no reboot).
- Ethernet recovery, ESP-IDF Ethernet driver, OTE over HTTPS, mDNS (4.24): built, not tested on the board yet. Points to check first: DHCP host name, IPv6 (SLAAC, neighbour discovery with the multicast setting), `<name>.local`, the hardware NTP path and the PTP loopback after the driver change, an upload on the page and with the script, a refused image (wrong build profile), an Ethernet recovery (unplug the cable for more than 35 s once the server is open for business). Board test 2026-10-28: an upload on the page worked (4.47); the hardware NTP path and the PTP loopback run after the driver change; IPv6 with chrony works (4.34). Not checked: the Ethernet recovery, a refused image, mDNS.
- Web admin (4.23): not tested on the board yet. Points to check first: TLS handshake time and heap with two browsers, the ND6 gateway hook with a static IPv6 address and SLAAC off, the 120 s rollback, the one-shot restart with changed pins, the button recovery. Board use 2026-10-26 to 28: the page with English and German texts, the settings, the diagnostics panel and the firmware upload work in daily use; the points above were not checked one by one.
- The certificate is self-signed: every browser warns, the fingerprint has to be compared by hand. The certificate names the FQDN only (no IP addresses, they can change).
- The IPv6 prefix length is stored and shown, but lwIP decides on-link with the first 64 bits; prefixes shorter than 64 are refused.
- Changing the time zone at run time calls `setenv()` / `tzset()` while other tasks may call `localtime_r()`; newlib guards both, a reading during the switch may use the old zone once.
- The settings blob has a layout version: after a firmware update that changes `web_settings_t` the saved settings are ignored (compiled defaults) until they are saved again.
- PreferIPvX, MACAddress and the MQTT reporting details stay compile-time settings. `UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED` is now the default of the GNSS protocol setting: both GNSS paths are compiled in (`setup_gnss_ublox_receiver()`, `setup_gnss_nmea_receiver()`).
- The settings blob layout version is 10 (2: GNSS protocol and LED colours, 3: OTE settings, 4: up to 8 NTP keys, 5: MAC address, MQTT topic / QoS / retain / period / report contents / TF client limit, TF card pins, 6: PPS holdover and the behaviour after it, 7: root dispersion growth, 8: NTP client rate limit, 9: minimum GNSS fix age, 10: minimum satellites in use; the places of the removed OTE device name and password are kept and zeroed, 4.27): settings saved with version 5 to 9 are read (the new fields get their defaults), older ones are ignored once.
- Scripted uploads with the web admin password (4.27): built, script tested against a mock, not tested on the board yet. Points to check first: an upload with `tools/ote_upload.py` and the web admin password (also a password set on the page), the 403 with the factory password, the 401 and the lock-out after wrong passwords, the log line that erases the former OTE password from the saved settings (once, at the first start), `<host name>.local` after a change of the FQDN.
- Per-client rate limit (4.26): built and simulated, not tested on the board yet. Points to check first: a chrony client with `minpoll -4 maxpoll -4` over hours (status counter stays 0), a flood from one host (e.g. `ntpdate`/`sntp` in a loop) is held to the limit while other clients keep their replies, Apply of 0 and of a new value without a restart. The flood test was not made (2026-10-28: not pursued).
- Own bucket for requests with a T2 cookie (4.30): built and modelled, not tested on the board yet. Check: send 50 requests per second with the address of a chrony client from a test computer (e.g. scapy); `chronyc ntpdata` of the real client keeps showing replies with `Interleaved: Yes`, and the status counter of dropped requests grows. Not tested on the board (2026-10-28: not pursued, S-17).
- Re-lock after a holdover (4.31): built and modelled, not tested on the board yet. Check: disconnect the PPS for 10 minutes (holdover set long enough), then reconnect; chrony's `measurements.log` should show a ramp of the offset instead of a step, and the debug output `PTP servo: re-lock, mapping residual … ns`. Check the stack of the PPS task once with `CALCULATE_STACK_SIZES_ENABLED 1`. Board test 2026-10-26: after a GNSS and PPS loss of 6:50 min the re-lock ramps (after the fix of 4.50); stack of the PPS task measured (`688960e`, `43658df`).
- GNSS validity after a cold start (4.32): built and modelled, the page tested against a mock, not tested on the board yet. Check: with the setting at 750 s, the status line counts the fix age after a start and stratum 1 comes after about 12.5 min with the console line; with a u-blox receiver after a cold start (backup battery removed) the first synchronisation waits for `fullyResolved`. An RMC with mode N or E must not lead to stratum 1.
- One hardware NTP handler (4.34): built, not tested on the board yet. Check IPv4 and IPv6 with chrony (`chronyc ntpdata`: `Interleaved: Yes`, `TX timestamping: Hardware`, similar offsets and delays as before), one authenticated request per family, and the stack of the hardware NTP task with `CALCULATE_STACK_SIZES_ENABLED 1`. Board test 2026-10-26/27: IPv4 and IPv6 with chrony `Interleaved: Yes`, `TX timestamping: Hardware`; stack of the hardware NTP task measured (`43658df`). Not checked: an authenticated request.
- Integer absolute times (4.35): built and modelled, not tested on the board yet. Check: the debug line of the PPS task (`raw_phase`, `resid`, `jitter_excess`, `d_central`) shows the same values as before, the PTP status (offset, innovation, noise) is unchanged, and the stack of the PPS task with `CALCULATE_STACK_SIZES_ENABLED 1`. On the board since 2026-10-26: the PTP status and the diagnostics look as expected (4.42); a line-by-line comparison of the debug output with the earlier firmware was not made; PPS task stack measured.
- Arduino core and SparkFun library removed (4.38): built, not tested on the board yet. Check: the console at 115200 baud from the first line on, the uptime / restart button (hold time), the periodic GNSS refresh every `periodicGNSSRefreshEveryThisNumberOfMinutes`, and the stack of the PPS task with `CALCULATE_STACK_SIZES_ENABLED 1`. Runs on the board since 2026-10-26 (all board tests of 4.41 to 4.57); stacks measured (`79bd538`, `51b48d1`, `688960e`, `43658df`).
- Native UBX protocol (4.37): built and modelled, not tested on the board yet. Check with the M10: first start (baud rate ladder to 921600, identity stored) and a second start (fast path with the stored identity of the SparkFun firmware), the console warnings of the message configuration (none expected), stratum 1 after `validUTC`, the qErr line on the status page (learning, then active with a slope near +1 or -1 after about 2 minutes; if it stays at *no qErr*, check the debug UBX line: `qErr invalid` counting up means flags bit 4 has the opposite meaning), the stack of the PPS task and of `gnss_uart` with `CALCULATE_STACK_SIZES_ENABLED 1`. With the GT-U16 (NMEA protocol): as before. Board test 2026-10-26: first and second start (fast path), stratum 1 after `validUTC`, stacks measured; qErr: the M10 flags every TIM-TP invalid with qErr 0, so no correction with this module (4.55). Open: the qErr correction with a receiver that gives a valid qErr.
- No step after a GNSS loss (4.50): disconnect the antenna (GT-U16: the PPS stops as well) for more than 30 s and less than the holdover time, connect again: the row *Last holdover* ends with a time error (not *set instead of ramped*), and the filter row shows the re-lock residual for some seconds. Board test 2026-10-26: loss of 6:50 min, the row *Last holdover* ends with +7.5 µs (holdover 47 s after the start, rate not settled yet), the re-lock ramps.
- Last holdover (4.49): disconnect the PPS for a few minutes (within the holdover time), reconnect; the row *Last holdover* shows the duration and the error (expected about 1 us or less after 10 min with a warm board). Board test 2026-10-27: 3:14 min after 5:40 h of run time, -135.6 ns (M10); 21 s shortly after a start, +301.7 ns (GT-U16, 4.43).
- Firmware update rollback (4.47): flash the bootloader once over USB; then an OTE update: console `New firmware after an update: ...`, the status line, after about a minute `Firmware update checked: the new firmware is kept`; an upload during the check gets the 409 message. Rollback test (test board only): an update with an image that crashes at the start (e.g. a temporary `abort()` in `app_main`) must come back with the previous firmware after the first crash. Board test 2026-10-28 (bootloader flashed over USB): `Firmware update checked: the new firmware is kept` 96.6 s after the start, two uploads during the check refused (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`, 409). The rollback test with a crashing image was not made.
- Core dump (4.46): flash the partition table over USB once, then trigger a crash on purpose only on a test board (e.g. a temporary `abort()`), check the console line at the next start, the row *Last crash*, the download with `idf.py coredump-info --core coredump.bin --core-format raw`, and *Delete*. Board: the partition is flashed (shown in the boot log); the crash test was not made.
- lwIP and UART interrupt in IRAM (4.45): after deleting `sdkconfig`: build, flash, the UART overflow counter in the diagnostics stays 0 while settings are saved and during an OTE upload. Board test 2026-10-28: the UART overflow counter stayed 0 while settings were saved several times and during an OTE upload.
- u-blox time pulse and antenna (4.44): built, not on the board yet. Check with the M10: the console line `GNSS time pulse: ...` at the start (rising edge, 1 s, UTC or GPS grid; no warning), the status line `antenna OK, jamming none` (unplug the antenna: *open* or *unknown* depending on the antenna supervisor of the board), the time pulse and RF values in the diagnostics. Board test 2026-10-26 with the M10: time pulse 1 s / 100 ms / rising edge / UTC grid / no pulse without lock, cable delay 50 ns, no warning; MON-RF antenna and jamming *unknown* (no antenna supervisor, interference monitor off), AGC 18 %.
- PPS without valid GNSS time (4.43): built, not on the board yet. Check with the M10 (time pulse also without fix, if it can be set) or by unplugging the GNSS antenna while the PPS keeps running: console `PPS not followed`, stratum 1 for the PPS holdover time, then as set; after the antenna is back `PPS followed again` and a ramp instead of a step in chrony. With the GT-U16 nothing changes in normal operation. Board test 2026-10-27 with the GT-U16, the UART RX line disconnected while the PPS kept running: `PPS not followed: GNSS time not valid (holdover)`, after reconnecting `PPS followed again: GNSS time valid for 5 s` 21 s later, *Last holdover* +301.7 ns.
- Diagnostics (4.42): built, formulas checked in Python, page tested against a mock, not on the board yet. Check: open the *Diagnostics* panel; with the loopback the measurement says *PTP PPS loopback*, the innovation RMS after a minute is a few ns, ADEV at 1 s some 1e-9 to 1e-8 and falling with tau, TDEV a few ns at 1 s; the hardware path shows nearly 100 % interleaved for the chrony client; MQTT reports carry `time_quality`. Measure the stacks of the PPS task, `ntp_server` and the MQTT task once with `CALCULATE_STACK_SIZES_ENABLED 1` (frames +16, +16 and +112 B). Board test 2026-10-26: PTP PPS loopback, innovation RMS about 9 ns (GT-U16) and 10 ns (M10), noise 7 ns, ADEV(1 s) 1.4e-8 falling to 1.3e-9 at 16 s and then flat (crystal); stacks measured. MQTT `time_quality` not checked (MQTT off; 2026-10-28: not pursued).
- Native NMEA reader (4.36): built and modelled, page tested against a mock, not tested on the board yet. Check with the GT-U16: synchronisation and stratum 1 as before (RMC only), then with GGA enabled (`$CFGMSG,0,0,1`): satellites and fix quality on the status page, the RMC phase (stable, some 10 to a few 100 ms), the minimum satellites setting (e.g. 30: stratum 16 after a few seconds; back to 0: stratum 1), and the stack of the `gnss_nmea` task with `CALCULATE_STACK_SIZES_ENABLED 1`. Board test 2026-10-26 with the GT-U16: synchronisation and stratum 1, with GGA 23 satellites and fix quality 1, RMC 36 to 67 ms after the PPS; stack of the reader measured (`43658df`).
- Stack reserve and heap (4.59): open the diagnostics after an hour: every running task listed with some hundred bytes or more free (the sizes were measured with a margin of 30 to 70 %), none in red; the heap minimum stays well above 100 kB, also after a TLS login and an OTE upload. Compare with the values of the stack measurement (`43658df`).
- Root dispersion growth 15 ppm (4.58): set 0.015 ms per second on the page (saved settings keep 100 ppm), then a holdover of 5 to 10 minutes: `chronyc sources` / `ntpdata` show a root dispersion growing by about 0.9 ms per minute, and chrony keeps selecting the server during the holdover.
- Diagnostics of the basic mode TX delay (4.56): tested on the board (4.57): minimum 13.9 us (IPv4) and 15.2 us (IPv6), maximum up to 75 us, no phase of the second stands out. Basic mode clients keep a scatter of a few us around the compensated mean.

## 13. Recommendations that are not in the code

NVS encryption (not enabled, decision of 2026-10-21; it burns an eFuse key block permanently):

1. Build once with `CONFIG_NVS_ENCRYPTION=y` and `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y` (key protection with the HMAC peripheral), `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID` = a free key block (0 to 5).
2. At the first start the firmware generates the HMAC key and burns it into that eFuse block (purpose HMAC_UP); this cannot be undone and the block is lost for other uses. Without flash encryption the eFuse key is still readable with JTAG unless read protection is set.
3. The NVS partition is then encrypted (XTS-AES); existing plain NVS data is not readable any more: erase the `nvs` partition before (all calibrations, the TLS key and the web settings are created again).
4. Use it together with flash encryption and Secure Boot V2 for real protection against an attacker with physical access; otherwise the firmware with the compiled secrets stays readable.

- Per-client rate limiting (like chrony's `ratelimit`) to protect the other clients from a single flooding host.
- Holdover at stratum 1 after a PPS loss: was not wanted (decision of 2026-10-20); since 2026-10-22 it is a setting with a default of 2 s (4.25).
- OTE: Secure Boot V2 and flash encryption if it is enabled; MQTT over TLS.

## 14. Files and how to build

Current files of the changes:

| File | Content |
|---|---|
| `main/main.cpp` | the complete current program |
| `main/ESP32TimeServerSettings.h` | follow-up: `PTPPPSLoopbackPin` added (default -1) |
| `main/custom/ntp_auth.cpp` | PSA status handling fixed |
| `main/custom/ntp_cache.c`, `main/custom/ntp_cache.h` | client cache (unchanged since the earlier work) |
| `main/custom/gnss_uart.*`, `gnss_nmea.*`, `gnss_ubx.*` | GNSS receiver on the ESP-IDF UART: reader task, NMEA sentences, UBX protocol (4.36 to 4.38) |
| `main/custom/time_diag.*` | diagnostics of the time keeping: phase series, ADEV / TDEV, innovation RMS, PPS interrupt histogram (4.42) |
| `main/custom/crash_dump.*`, `partitions.csv` | core dump of the last crash: check at the start, summary, download and erase (4.46) |
| `CMakeLists.txt` (root) | -O2 warning handling for third-party components |
| `sdkconfig.defaults`, `sdkconfig` | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` |
| `main/CHANGELOG.md` | this file |
| `main/web/web_settings.*` | web settings: defaults, NVS (saved, one-shot), validation (4.23) |
| `main/web/web_auth.*` | password hash, sessions, CSRF, login throttling, allow-list |
| `main/web/web_tls.*` | TLS key and self-signed certificate |
| `main/web/web_net.*` | network settings at run time, ND6 gateway hook |
| `main/web/web_server.*` | HTTPS server, TLS transport, JSON API |
| `main/web/web_ote.*` | firmware upload (Over The Ethernet update) over the HTTPS server (4.24) |
| `tools/ote_upload.py` | firmware upload script (HTTPS, DNS name (A / AAAA) or address, certificate fingerprint, web admin password) |
| `main/web/www/index.html`, `app.css`, `app.js`, `favicon.ico` | the page and its icon (embedded) |
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
