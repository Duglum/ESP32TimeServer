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

**RADIUS servers** (*Security*, *RADIUS servers*): per server a host (an IPv4 or IPv6 address or a DNS name), the UDP port (1812) and the shared secret (1 to 64 printable characters). Server 2 is asked when server 1 does not answer. Timeout per try 3 s, 2 more tries per server, a server that did not answer is skipped for 2 minutes (*Dead time*, 0 = never skipped). The requests carry User-Name, User-Password (PAP), Message-Authenticator, NAS-Identifier (the host name unless set), NAS-IP-Address or NAS-IPv6-Address (the address the request is sent from unless set), NAS-Port 0, NAS-Port-Id `https`, NAS-Port-Type Virtual, Service-Type Login and Calling-Station-Id (the address of the client); Service-Type and NAS-Port-Type can be changed or left out.

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

**Stored where.** The RADIUS settings and the check boxes are part of the settings (*Apply*, *Save Settings*); the configuration file (export) carries the RADIUS shared secrets, the website never gets them back. The tokens are stored in NVS at once (up to 9, about 300 bytes each), are not in the configuration file and are erased by *Reset to defaults*. The NVS partition has 24 kB and the settings take about 7 kB of it: a new token is only stored while NVS keeps room for writing the settings again (*Diagnostics*, *NVS*); on the test board that is about 4 tokens.

Third-party code: `THIRD_PARTY_NOTICES` (QR Code generator library of Project Nayuki, MIT).

## Disclaimer

**99.9% of this fork is AI slop!**  
Almost no code was written or really reviewed by me.  
I just threw it at Claude and told it to improve stuff and build a settings website/server. For fun.
In general it works really good, but i didn't test all features.

Do with it whatever you want within the original MIT license used by Rob, but don't complain to me if anything breaks.

## Screenshot of the Website:

<img width="1230" height="5953" alt="settings_website_20261021_01" src="https://github.com/user-attachments/assets/bf447f55-7757-43f9-99a0-c453763e0a40" />
