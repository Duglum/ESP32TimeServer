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

Not taken over: the change of the internal time scale to TAI (this fork keeps UTC inside and adds TAI - UTC only for PTP).
The fork's version name changed from "WebIf" to "WebUI".

## Disclaimer

**99.9% of this fork is AI slop!**  
Almost no code was written or really reviewed by me.  
I just threw it at Claude and told it to improve stuff and build a settings website/server. For fun.
In general it works really good, but i didn't test all features.

Do with it whatever you want within the original MIT license used by Rob, but don't complain to me if anything breaks.

## Screenshot of the Website:

<img width="1230" height="5953" alt="settings_website_20261021_01" src="https://github.com/user-attachments/assets/bf447f55-7757-43f9-99a0-c453763e0a40" />
