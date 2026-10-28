# Host tests

Unit tests and fuzz targets for the parts of the firmware that read outside input, built for the PC with
AddressSanitizer and UndefinedBehaviorSanitizer. The firmware sources are compiled unchanged; `stubs/` replaces
the few ESP-IDF and FreeRTOS functions they call (single-threaded, with a simulated `esp_timer` clock).

| Test | Firmware code | What is checked |
|---|---|---|
| `gnss_test` | `main/custom/gnss_uart.cpp` (byte parser of the reader task), `gnss_nmea.cpp`, `gnss_ubx.cpp` | NMEA checksum and RMC fields (status, mode, build year, ranges), the reader with noise, cut sentences, flushes and UBX frames in between, GGA fix quality and minimum satellites, GSV groups, RMC phase after the PPS, NAV-PVT (fields, leap seconds, invalid reasons), TIM-TP and the qErr lookup by time of week, the commands MON-VER, SEC-UNIQID, MON-RF, CFG-VALGET and CFG-VALSET with answers, NAKs and time-outs |
| `ntp_cache_test` | `main/custom/ntp_cache.c` | interleaved T3 from the client record and the exchange map, spoofed and stale records, slot reuse, per-client rate limit (burst, sustained rate, own bucket for requests with the T2 cookie), IPv6 limit per /64 and per address, expiry |

Fuzz targets (`-DHOST_FUZZ`, libFuzzer):

- `gnss_fuzz`: the input is a byte stream from the receiver (a byte 0xFE starts a new flush generation) and the answer of the receiver to the commands. Checks the sanitizers and that a valid RMC has plausible fields and the version and ID texts stay within their buffers.
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

## Results (2026-10-28)

All checks pass. Fuzzing for 5 minutes each (about 3.7 million GNSS and 5 million cache inputs) found nothing. Building
with clang found one unused variable in `gnss_ubx.cpp` (removed, CHANGELOG 4.75).
