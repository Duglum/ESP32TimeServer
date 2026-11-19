# ESP32 NTP Stratum 1 Time Server (version 4.0.0-WebUI)

An ESP32 NTP Stratum 1 Time Server for your home network

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## Overview

This is a fork of [Rob Latour's neat ESP32TimeServer Project](https://github.com/roblatour/ESP32TimeServer).

At the moment it features the functions of his release v4.0 plus some changes.
The big one is a Website to change settings and update firmware images.

## What's new in 4.0.0-WebUI

Taken over from Rob's release v4.0 (adapted to this fork, everything set on the website):

- PTP master (IEEE 1588, end-to-end, two-step) over Ethernet, UDP/IPv4 and UDP/IPv6, off by default. It serves only while the server is stratum 1 with GNSS and PPS, and announces itself as a GNSS-locked grandmaster (clockClass 6). Delay_Req rate limit per follower, follower list on the website.
- PTP authentication (AUTHENTICATION TLV, HMAC-SHA256-128, compatible with linuxptp).
- NTP "authentication required": unsigned NTP requests can be refused.
- MQTT / Home Assistant: PTP report and a larger MQTT queue.
- `tools/configTimeSettings.ps1`: sets up Windows Time as NTP or PTP client of the server.
- ESP32-S31 build (experimental, not tested on hardware).

Also new in this fork: RADIUS and TOTP logins of the website (see below).

Not taken over: the change of the internal time scale to TAI (this fork keeps UTC inside and adds TAI - UTC only for PTP).
The fork's version name changed from "WebIf" to "WebUI".

## RADIUS and TOTP logins of the website

The website can check logins against up to two RADIUS servers and ask for a TOTP code as a second factor (taken over from the ESP32-ConsoleServer project). Everything is off by default: the website has the local user `admin` with its password.

**RADIUS servers** (*Security*, *RADIUS servers*): per server a host (an IPv4 or IPv6 address or a DNS name; a name ending in `.local` is looked up with mDNS), the UDP port (1812) and the shared secret (1 to 64 printable characters). Server 2 is asked when server 1 does not answer. Timeout per try 3 s, 2 more tries per server, a server that did not answer is skipped for 2 minutes (*Dead time*, 0 = never skipped). The requests carry User-Name, User-Password (PAP), Message-Authenticator, NAS-Identifier (the host name unless set), NAS-IP-Address or NAS-IPv6-Address (the address the request is sent from unless set), NAS-Port 0, NAS-Port-Id `https`, NAS-Port-Type Virtual, Service-Type Login and Calling-Station-Id (the address of the client); Service-Type and NAS-Port-Type can be changed or left out.

**Who is let in.** An Access-Accept counts from privilege level 15 (*Privilege level at least*, 1 to 15): the level is taken from `Cisco-AVPair` `shell:priv-lvl=N`, without it Service-Type Administrative-User counts as 15 and anything else as 1, as on a Cisco device. *Attribute that must also be in the answer* (optional) asks for one more attribute, compared exactly: `Cisco-AVPair=<value>`, `Class=<value>` or `Filter-Id=<value>`. An Access-Challenge is not supported and refused. *Answers must carry a valid Message-Authenticator* (on by default) drops every answer without a valid one (BlastRADIUS, CVE-2024-3596); the requests always carry one.

**The login methods** (*Security*, *Login to this page*): *Local password* and *RADIUS* can be on together; *Local password only when no RADIUS server answers* decides the order, as `aaa authentication login` on a Cisco device:
- on (default), like `group radius local`: every user name is asked from RADIUS first; only when no server answers, `admin` logs in with the local password. A RADIUS Access-Reject is final.
- off, like `local group radius`: `admin` is checked with the local password only, every other name by RADIUS.
- RADIUS alone: every name, also `admin`, is checked by RADIUS.

With RADIUS on, the login page asks for a user name (empty = `admin`). *Status*, *Login* shows the user and method of your session, the RADIUS servers (last answer, round trip, dead time) and the counters.

**Lock-out guard and throttling.** The local password can only be switched off in a session that RADIUS logged in, so a wrong RADIUS setting cannot lock you out; *Apply* without *Save Settings* is undone by a restart as well. Every refused RADIUS login (also "no answer") counts as a failed login with the same lock-out as a wrong password. At most 2 logins are asked at once and one per source address; a login that waits for RADIUS does not hold up the page. The RADIUS client task and its memory (about 8 kB plus 6 kB stack) are only started once RADIUS is switched on or tested (this board has no PSRAM).

**Test.** *Security*, *Test*: a user name and a password sent to the servers with the values in the form, also before *Apply*. The answer shows the result, the server, the round trip and the attributes. A RADIUS test server of the Python standard library is in the repository:

```
python -I testing/board/radius_server.py --secret testing123
```

**TOTP second factor** (*Security*, *Second factor (TOTP)*): codes of an authenticator app (RFC 6238: SHA-1, 6 digits, 30 s, one step before or after accepted, each code once). *Set up a token* for `admin` or a RADIUS user makes a new key on the device and shows it as a QR code; the first right code of the app stores it and shows 10 recovery codes once (`xxxxx-xxxxx`, each works once instead of a code). *Required for the local user `admin`* (only with a token for `admin`), *Required for RADIUS users*, and *RADIUS users without a token set one up at their first login*. The login page asks for the code after the password (within 2 minutes, three wrong codes and the password is asked again). TOTP needs the time of the device, which comes from GNSS: after a start without a GNSS fix only the recovery codes work. A scripted firmware upload needs the code of `admin` while it is required (`python tools/ote_upload.py --totp ...`) and is refused while the local password is off.

**Stored where.** The RADIUS settings and the check boxes are part of the settings (*Apply*, *Save Settings*); the configuration file (export) carries the RADIUS shared secrets, the website never gets them back. The tokens are stored in NVS at once (up to 9, about 300 bytes each), are not in the configuration file and are erased by *Reset to defaults*. A new token is only stored while NVS keeps room for writing the settings again (*Diagnostics*, *NVS*; a token and one settings set need 237 entries of 32 bytes): with the 256 kB `nvs` of the current partition table all 9 fit. A board that still has the old table (24 kB, see *Partition table and NVS*) has much less room: the test board with an own RSA 2048 certificate stored had 253 free entries.

Third-party code: `THIRD_PARTY_NOTICES` (QR Code generator library of Project Nayuki, MIT).

## Partition table and NVS

`partitions.csv` (since 2026-11-19): `otadata` at `0xF000`, `phy_init`, `factory`, `ota_0` and `ota_1` (4 MB each), `coredump` (128 kB at `0xC20000`) and `nvs` (256 kB at `0xC40000`); `0x9000` to `0xEFFF` is unused, the rest of the flash too. `nvs` holds everything the device stores: the saved settings and the one-shot set of a restart (6996 bytes each), the self-signed web certificate and its key, an own certificate chain and key (up to 8192 and 4096 characters of PEM), the password hash, the allow-list, up to 9 TOTP tokens and a few values of the device (GNSS identity, PPS interrupt and PTP write latency). At the limits that is about 823 entries of 32 bytes, and a changed item is written in full before its old copy is erased. An unchanged settings set (*Save Settings* without a change, the same one-shot set twice) is not written again: ESP-IDF 6.1.1 takes an unchanged blob of more than one chunk (over 400 bytes) as changed and writes it again (`cmpMultiPageBlob()` in `nvs_storage.cpp`), so the firmware compares the stored bytes itself first (`main/custom/nvs_blob.cpp`). The former `nvs` of 24 kB at `0x9000` has 630 usable entries. The firmware finds the partition by its name and runs on both tables.

**Boards updated over the network keep their table.** A firmware upload (web page, `tools/ote_upload.py`) writes only an application slot, never the partition table: such a board keeps the 24 kB `nvs` and its limits until it is flashed over USB with the new table. Measured on the test board with the old table (an own RSA 2048 certificate stored, 253 free entries): up to firmware `2102115` an own certificate chain of 8192 characters was refused with `400` "The certificate could not be stored (NVS: ESP_ERR_NVS_NOT_ENOUGH_SPACE)". A chain of 6000 characters with a key of 4096 was refused the same way, but the new chain was already stored without its key (chain and key were two NVS items, written one after the other): after the next start the stored own certificate was not used ("The private key does not belong to the first certificate of the chain") and the page showed none. In that state *Save Settings* answered `500` "Settings could not be written to NVS", *Restart with updates* `500` "One-shot settings could not be written" and a new TOTP token `507`. Since then chain and key are one NVS item (a power loss or a failed write keeps the old pair or stores the new one) and the room is checked before anything is written: on that board both are refused with `400` "The certificate could not be stored: NVS has 253 free entries, 437 are needed (the certificate and key, and room for writing the settings again)" (508 for the largest pair), NVS stays unchanged and the stored own certificate stays usable. By this check a pair of up to 4286 characters (chain and key together) still fits there; stored and used there: an ECDSA P-256 pair of 2385 characters.

**The new table over USB** (ESP-IDF environment; `COMx` is the port of the board, `build_v3_1` the build folder of its profile). Writing only the partition table keeps the firmware and the boot slot in use; `idf.py flash` writes the new table as well, but also the bootloader and the firmware into `factory`, and resets `otadata` (the device then starts from `factory`). With the new table the device starts with an empty `nvs`: default settings, the password `password`, a new self-signed certificate (new fingerprint), no own certificate, no TOTP tokens. To carry everything over, copy the old `nvs` into the start of the new one (an NVS of 6 pages is a valid NVS at the start of a larger, erased partition; the other pages are then free pages):

```powershell
esptool --chip esp32p4 -p COMx read-flash 0x9000 0x6000 nvs_old.bin   # with the old table; holds the password hash and private keys
esptool --chip esp32p4 -p COMx erase-region 0xC40000 0x40000
esptool --chip esp32p4 -p COMx write-flash 0x8000 build_v3_1\partition_table\partition-table.bin 0xC40000 nvs_old.bin
```

The device then starts with its settings, password, certificates and tokens (the same fingerprints as before). The old region `0x9000` to `0xEFFF` is no longer used but still holds the password hash and the private keys of the web certificates: once the device works with the new table, erase it with `esptool --chip esp32p4 -p COMx erase-region 0x9000 0x6000` and delete `nvs_old.bin`. Tried on an ESP32-P4 (chip v3.2), not on the ESP32-S31.

## Disclaimer

**99.9% of this fork is AI slop!**  
Almost no code was written or really reviewed by me.  
I just threw it at Claude and told it to improve stuff and build a settings website/server. For fun.
In general it works really good, but i didn't test all features.

Do with it whatever you want within the original MIT license used by Rob, but don't complain to me if anything breaks.

## Screenshot of the Website:

<img width="1230" height="5953" alt="settings_website_20261021_01" src="https://github.com/user-attachments/assets/bf447f55-7757-43f9-99a0-c453763e0a40" />
