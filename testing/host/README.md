# Host tests

Unit tests and fuzz targets for the parts of the firmware that read outside input, built for the PC with
AddressSanitizer and UndefinedBehaviorSanitizer. The firmware sources are compiled unchanged; `stubs/` replaces
the few ESP-IDF and FreeRTOS functions they call (single-threaded, with a simulated `esp_timer` clock).

| Test | Firmware code | What is checked |
|---|---|---|
| `gnss_test` | `main/custom/gnss_uart.cpp` (byte parser of the reader task), `gnss_nmea.cpp`, `gnss_ubx.cpp` | NMEA checksum and RMC fields (status, mode, build year, ranges), the reader with noise, cut sentences, flushes and UBX frames in between, GGA fix quality and minimum satellites, GSV groups, RMC phase after the PPS, NAV-PVT (fields, leap seconds, invalid reasons), TIM-TP and the qErr lookup by time of week, the commands MON-VER, SEC-UNIQID, MON-RF, CFG-VALGET and CFG-VALSET with answers, NAKs and time-outs |
| `gnss_console_test` | `main/custom/gnss_console.cpp`, the recording of `gnss_uart.cpp`, `gnss_ubx_send_raw()` | GNSS console of the web page: text commands (checksum and CR LF each on or off, refusals), UBX commands in hex (with and without `B5 62`, length and checksum check, limits), the summaries of received UBX frames (ACK/NAK, MON-VER, hex cut within the buffer for every buffer size), periodic messages, the ring (overwriting, cut texts), the reader recording text lines, UBX frames and lines cut by frames, the raw UBX send |
| `ntp_cache_test` | `main/custom/ntp_cache.c` | interleaved T3 from the client record and the exchange map, spoofed and stale records, slot reuse, per-client rate limit (burst, sustained rate, own bucket for requests with the T2 cookie), IPv6 limit per /64 and per address, expiry |

Fuzz targets (`-DHOST_FUZZ`, libFuzzer):

- `gnss_fuzz`: the input is a byte stream from the receiver (a byte 0xFE starts a new flush generation) and the answer of the receiver to the commands. Checks the sanitizers and that a valid RMC has plausible fields and the version and ID texts stay within their buffers.
- `gnss_console_fuzz`: the input is a typed command and, after a zero byte, a byte stream from the receiver recorded by the reader. Text and UBX commands must build only printable bytes (and CR LF) within the buffer or name a reason, summaries and recorded entries must stay terminated within their buffers, sequence numbers must rise.
- `ntp_cache_fuzz`: a sequence of requests, stored exchanges, time steps, expiries and rate limit changes on 8 IPv4 and 8 IPv6 addresses (4 of them in one /64) and many others. Every interleaved T3 the cache hands out must have been stored for this address and this T2 (a model of the stored exchanges); a planted defect (exchange map lookup without the address) is found within two minutes.

## Running

Needs clang with the sanitizer and libFuzzer runtimes for Windows: [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`). No ESP-IDF needed.

```powershell
.\run_tests.ps1                    # unit tests
.\run_tests.ps1 -FuzzSeconds 300   # also each fuzz target for 300 s (corpus and crash files in build\)
```

`gnss_test.exe --seeds <folder>` writes seed inputs (valid sentences and frames) for the GNSS fuzzer; the script
does this before fuzzing. A crash is saved as `build\crash-*` and can be replayed with
`build\gnss_fuzz.exe build\crash-...`.

## Results (2026-10-29)

With the GNSS console (CHANGELOG 4.83): all checks pass (gnss_test 108, gnss_console_test 363, ntp_cache_test 734). Fuzzing for 3 minutes each (about 1.9 million GNSS, 13.1 million console and 4.6 million cache inputs) found nothing.

## Results (2026-10-28)

All checks pass. Fuzzing for 5 minutes each (about 3.7 million GNSS and 5 million cache inputs) found nothing. Building
with clang found one unused variable in `gnss_ubx.cpp` (removed, CHANGELOG 4.75).
