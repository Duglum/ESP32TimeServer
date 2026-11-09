# Host tests

Unit tests and fuzz targets for the parts of the firmware that read outside input, built for the PC with
AddressSanitizer and UndefinedBehaviorSanitizer. The firmware sources are compiled unchanged; `stubs/` replaces
the few ESP-IDF and FreeRTOS functions they call (single-threaded, with a simulated `esp_timer` clock).

| Test | Firmware code | What is checked |
|---|---|---|
| `gnss_test` | `main/custom/gnss_uart.cpp` (byte parser of the reader task), `gnss_nmea.cpp`, `gnss_ubx.cpp` | NMEA checksum and RMC fields (status, mode, build year, ranges), the reader with noise, cut sentences, flushes and UBX frames in between, GGA fix quality and minimum satellites, GSV groups, RMC phase after the PPS, NAV-PVT (fields, leap seconds, invalid reasons), TIM-TP and the qErr lookup by time of week, the commands MON-VER, SEC-UNIQID, MON-RF, CFG-VALGET and CFG-VALSET with answers, NAKs and time-outs |
| `gnss_console_test` | `main/custom/gnss_console.cpp`, the recording of `gnss_uart.cpp`, `gnss_ubx_send_raw()` | GNSS console of the web page: text commands (checksum and CR LF each on or off, refusals), UBX commands in hex (with and without `B5 62`, length and checksum check, limits), the summaries of received UBX frames (ACK/NAK, MON-VER, hex cut within the buffer for every buffer size), periodic messages, the ring (overwriting, cut texts), the reader recording text lines, UBX frames and lines cut by frames, the raw UBX send |
| `ntp_cache_test` | `main/custom/ntp_cache.c` | interleaved T3 from the client record and the exchange map, spoofed and stale records, slot reuse, per-client rate limit (burst, sustained rate, own bucket for requests with the T2 cookie), IPv6 limit per /64 and per address, expiry |
| `ptp_test` | `main/custom/ptp_l2_protocol.h`, `ptp_followers.h`, `ptp_message.h`, `ptp_l3_udp.h` | upstream's PTP parser and follower tests (release 4.0, adapted: the correctionField and flagField of a Delay_Req are accepted), the message builder (byte layout of Sync, Follow_Up, Delay_Resp and Announce, flags, timestamps, limits), Delay_Resp from a parsed Delay_Req, UDP/IPv4 and UDP/IPv6 encapsulation and decoding (checksums, group, ports, TTL, fragments, MACs), a deterministic fuzz of both decoders |
| `mqtt_queue_test` | `main/custom/mqtt_report_queue.h` | upstream's test of the byte ring of the MQTT RAM queue (order, wrap marker, drop oldest, oversize, random against a model) plus the fork's `push_text()` (NUL-terminated records, mixed with drops); no fuzz target |
| `ntp_auth_test` | `main/custom/ntp_auth.cpp` | NTP symmetric-key authentication: the MAC (SHA-256 of key and packet, 160 bit) against a value of Python `hashlib`, the results of `ntp_auth_verify_request()` in the mixed and the authenticated-only policy (NTPv3 and NTPv4 without MAC, unknown key ID, changed bytes, malformed lengths), the policy without authentication and without a key (fail closed), invalid keys of the settings, a failing hash. `stubs/psa/crypto.h` gives the PSA hash calls with a plain SHA-256 (checked against FIPS 180-4 examples); no fuzz target |
| `ptp_auth_test` | `main/custom/ptp_auth.cpp` (with `ptp_message.h`) | PTP AUTHENTICATION TLV: layout (type 0x8009, length 22, spp, key ID) and the HMAC-SHA256-128 ICV against values of Python `hmac` (with the correctionField covered and, linuxptp `allow_mutable`, not covered), HMAC itself (RFC 4231 case 2), signing of Sync, Follow_Up, Delay_Resp and Announce from the message builder, every changed byte of a signed Delay_Req, key ring, key ID 0, other spp, lengths, the mixed / required policy, fail closed without the active key, invalid keys, a failing hash; no fuzz target |
| `ptp_selection_test` | `main/custom/ptp_l2_master_selection.h` | upstream's master selection test (ranking, start-up wait, qualification by two Announces, sequence wrap and replay, expiry, capacity, legacy mode); no fuzz target |
| `ip_text_test` | `main/net/ip_text.cpp` | IPv4 dotted decimal (strict: no leading zeros, exactly 4 parts), IPv6 text forms (`::`, embedded IPv4, refused forms such as a second `::`, 9 groups, zones, brackets), the canonical forms of RFC 5952 (longest zero run, the first of equal runs, no single-group `::`, IPv4-mapped), round trips of 20000 random addresses |
| `radius_test` | `main/auth/radius_proto.cpp`, `main/auth/radius_client.cpp` | RADIUS packets of the client: the User-Password and the Access-Accept of RFC 2865 7.1; Access-Requests byte by byte against an independent Python reference (`gen_radius_vectors.py`: Message-Authenticator first, every attribute, a password of three blocks) and their limits (user name, password, shared secret, texts, the buffer one byte too small); answers: Access-Accept, -Reject, the privilege level from `Cisco-AVPair` (every form, several pairs: the lowest, other vendors ignored) or from Service-Type, the required attribute, a missing or wrong Message-Authenticator, a wrong Response Authenticator, shared secret, identifier, code or length, attributes that do not fit, two Message-Authenticators, the largest packet, the summary cut to its buffer; the timing of a login: tries and timeouts per server, failover to the second server, the dead time (skipped servers, a login that gives up at once while every server is dead, dead time 0), the round trip time, a packet that could not be sent; the 256 identifiers of a socket |
| `totp_test` | `main/auth/totp.cpp`, `main/third_party/qrcodegen` | the second factor: base32 (RFC 4648 vectors, lower case, padding, refused characters, buffers one byte too small), HOTP (RFC 4226 appendix D), TOTP with SHA-1 (RFC 6238 appendix B), the check of a code (the step of now, one before and one after, not two; each code once; spaces; wrong lengths and characters; no time yet), the otpauth URI byte by byte (percent-encoding, too small a buffer), the QR code of the longest URI (version 15 at most, error correction M, the finder patterns), the recovery codes (text of 50 bits, normalising, the salted hashes, a used code, another salt) |
| `auth_config_test` | `main/auth/auth_config.cpp` (with `radius_proto.cpp`, `ip_text.cpp`, cJSON) | the login settings: defaults (local password only, RADIUS timeout 3 s, 2 retries, dead time 2 min, privilege level 15, Message-Authenticator required), every refused field of `radius` and `web_login` with its message (servers, hosts, ports, secrets, timing, NAS attributes, Service-Type, NAS-Port-Type, privilege level, required attribute, types), the longest DNS name, missing fields keep their value; the shared secrets: never in the API form, an empty or missing one keeps the stored one, a server without a host loses it, the stored form reads back unchanged |

Fuzz targets (`-DHOST_FUZZ`, libFuzzer):

- `gnss_fuzz`: the input is a byte stream from the receiver (a byte 0xFE starts a new flush generation) and the answer of the receiver to the commands. Checks the sanitizers and that a valid RMC has plausible fields and the version and ID texts stay within their buffers.
- `gnss_console_fuzz`: the input is a typed command and, after a zero byte, a byte stream from the receiver recorded by the reader. Text and UBX commands must build only printable bytes (and CR LF) within the buffer or name a reason, summaries and recorded entries must stay terminated within their buffers, sequence numbers must rise.
- `ntp_cache_fuzz`: a sequence of requests, stored exchanges, time steps, expiries and rate limit changes on 8 IPv4 and 8 IPv6 addresses (4 of them in one /64) and many others. Every interleaved T3 the cache hands out must have been stored for this address and this T2 (a model of the stored exchanges); a planted defect (exchange map lookup without the address) is found within two minutes.

- `ptp_fuzz`: the input is a received Ethernet frame. The L2 parser and the UDP decoder must stay within it, and a Delay_Req they accept must give a Delay_Resp with its sequence, correctionField and port identity.
- `ip_text_fuzz`: the input as text: an accepted IPv4 address is in canonical form, an accepted IPv6 address formats and parses back to the same bytes; the first 16 bytes as an address round-trip as well.
- `radius_fuzz`: the input as an answer datagram (both settings of the Message-Authenticator) and as the attributes of a correctly signed Access-Accept, so that the attribute checks are reached: an accepted answer has a terminated summary and a privilege level of 0 to 15.
- `totp_fuzz`: the input as base32 (a decoded text encodes and decodes to the same bytes), as a TOTP code (an accepted one matched a step of the window), as a recovery code and as the user name of an otpauth URI (an accepted URI fits its buffer).
- `auth_config_fuzz`: the input as the JSON of a request and of a configuration file: texts stay terminated, an accepted and valid set has no secret in its API form and reads back unchanged from its stored form.

The login modules and their tests (`ip_text`, `radius`, `totp`, `check.h`, the seeds in `seeds/`) come from ESP32-ConsoleServer. The RADIUS vectors in `radius_vectors.h` come from `gen_radius_vectors.py` (Python standard library only: `hashlib`, `hmac`); run `python -I gen_radius_vectors.py > radius_vectors.h` only to change them.

## Running

Needs clang with the sanitizer and libFuzzer runtimes for Windows: [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`). `radius_test` and `totp_test` also need ESP-IDF 6.1 (`IDF_PATH`,
default `C:\esp\v6.1\esp-idf`) and its CMake and Ninja: the first run builds TF-PSA-Crypto of ESP-IDF for the PC into
`build\psa` (ESP-IDF's copy includes two headers of its port: `stubs\psa_shim` stands in for them).

```powershell
.\run_tests.ps1                    # unit tests
.\run_tests.ps1 -FuzzSeconds 300   # also each fuzz target for 300 s (corpus and crash files in build\)
.\run_tests.ps1 -FuzzSeconds 180 -Only radius   # only the targets whose name contains "radius"
```

`gnss_test.exe --seeds <folder>` writes seed inputs (valid sentences and frames) for the GNSS fuzzer; the script
does this before fuzzing. A crash is saved as `build\crash-*` and can be replayed with
`build\gnss_fuzz.exe build\crash-...`.

## Results (2026-11-09, PTP authentication)

With CHANGELOG 4.107: all checks pass (gnss_test 108, gnss_console_test 363, ntp_cache_test 734, ptp_test, ptp_selection_test and mqtt_queue_test pass, ntp_auth_test 91, ptp_auth_test 141).

## Results (2026-11-08, NTP authentication required)

With CHANGELOG 4.106: all checks pass (gnss_test 108, gnss_console_test 363, ntp_cache_test 734, ptp_test, ptp_selection_test and mqtt_queue_test pass, ntp_auth_test 91).

## Results (2026-11-08)

With the PTP protocol code (CHANGELOG 4.100): all checks pass (gnss_test 108, gnss_console_test 363, ntp_cache_test 734, ptp_test and ptp_selection_test pass). Fuzzing for 30 s each (about 17 million PTP frames) found nothing.

## Results (2026-10-29)

With the GNSS console (CHANGELOG 4.83): all checks pass (gnss_test 108, gnss_console_test 363, ntp_cache_test 734). Fuzzing for 3 minutes each (about 1.9 million GNSS, 13.1 million console and 4.6 million cache inputs) found nothing.

## Results (2026-10-28)

All checks pass. Fuzzing for 5 minutes each (about 3.7 million GNSS and 5 million cache inputs) found nothing. Building
with clang found one unused variable in `gnss_ubx.cpp` (removed, CHANGELOG 4.75).
