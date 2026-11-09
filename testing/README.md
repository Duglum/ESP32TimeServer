# Ways to test an NTP Time Server

## Functional testing

### Internal functional health checks

ESP32TimeServer has a built in self health check logic which is disabled by default.  Enabling it allows the program to basically run tests on itself at startup - for example it will send itself an NTP request, receive and process it as it would any other external request, once done it will verify ther  results - including metrics accumulated for MQTT reporting.  In all there are 37 such tests.  

To enable a self health check set `STARTUP_HEALTH_TEST_ENABLED` to `1` near the top
of the `main.cpp` file. Once done, at startup the program will run its health tests and report the results in the console log.

This feature is disable by default as it is primary purpose is to help ensure, pre-release, code changes development do not introduce  unexpect regression issues.  Also it adds time to startup time delaying the point at which the server can come online (be open for business).

Of note: results may be misleading depending setup. For example, health checks may 'Pass' all
IPv4 tests yet 'Fail' all IPv6 tests, not because of a problem with the program but because
the network it is running on doesn't support IPv6.

### External functional testing

[ESP32TimeServerTester](https://github.com/roblatour/ESP32TimeServerTester) is an independent functional NTP server testing tool.  It tests much of the same functionality as the Health check tests do, but from an external standpoint.

### RFC 9769 compliance testing
RFC 9769 compliance testing ensures network time synchronization implements the RFC 9769 NTP interleaved modes specification for NTPv4 replies. While this testing is covered off by two testing methods above, a python testing tool is also available to specifically test RFC 9769 compliance, for more information please see: [/testing/RFC_9769_compliance/README.md](/testing/RFC_9769_compliance/README.md)


### Own TLS certificate of the web admin
[/testing/web_certs/web_certs.py](/testing/web_certs/web_certs.py) writes test certificates for *Security*, *TLS certificate of the web admin* into `web-certs/` (in `.gitignore`: it holds private keys, never commit one): a test CA, ECDSA P-256 and RSA 2048 chains with their keys, an expired chain, a P-521 pair and a foreign and an encrypted key (refused). It needs OpenSSL 3.4 or newer (the one of Git for Windows will do):

```
python -I testing/web_certs/web_certs.py --ip <address> --dns <FQDN>
```

Copy a file to the clipboard with `Get-Content web-certs\chain-ecdsa.pem -Raw | Set-Clipboard` (PowerShell) and paste the chain into *Certificate chain* and the key into *Private key*.

### RADIUS and TOTP logins of the website
[/testing/board/radius_logins.py](/testing/board/radius_logins.py) tests the logins of the website on the board against the RADIUS test server [/testing/board/radius_server.py](/testing/board/radius_server.py), which it runs on the PC (UDP 18120 and 18121, other ports with `--port-base`; the Windows firewall asks once for Python). Both come from the ESP32-ConsoleServer project (its web tests 1 to 9 and 16 to 19, without SSH). The settings are changed with *Apply* only and set back at the end; TOTP tokens it makes are deleted again. Test 20 (this fork: a right password does not reset the lock-out before the TOTP code) is added. Tests 9 and 20 each lock the PC out of the website for 30 s (skip them with `--no-throttle`). The TOTP tests need the time from GNSS.

```
python -I testing/board/radius_logins.py esp32timeserver.test [--pc-address 192.168.8.100] [--only 1,3] [--no-throttle] [--port-base 18120]
```

The web password comes from the environment variable `WEB_ADMIN_PASSWORD` or is asked for. `radius_server.py` alone is a RADIUS server for own tries (`--secret testing123 [--port 1812] [--mode normal|silent|bad_auth|bad_ma|no_ma|wrong_id|challenge|accept_all]`; users and modes in its header).

Host tests of the login modules (RADIUS packets, timing, TOTP, QR code, the login settings) with fuzzing: [/testing/host/README.md](/testing/host/README.md).

## Memory Leak Testing
A memory leak occurs when a program allocates memory but fails to release it when no longer needed. Over time, leaked memory can degrade performance and potentially cause application failures.  For more information please see: [/testing/memory_leak_testing/README.md](/testing/memory_leak_testing/README.md)

## Stress Testing
Stress testing evaluates how a program performs under extreme workloads or resource constraints beyond normal operating conditions. It helps identify stability issues, bottlenecks, and failure points. For more information please see this open source stress testing tool: [Time Server Stress Test](https://github.com/roblatour/TimeServerStressTest)

### Packet floods
[/testing/flood/flood.py](/testing/flood/flood.py) sends UDP datagrams at a fixed rate (default 15000 per second) to one port, or opens and closes TCP connections to the web admin. To UDP port 123 it sends 64 zero bytes by default (no NTP request: lwIP hands them to the NTP socket task); with `--ntp` valid NTPv4 requests (the hardware NTP path). Keep the serial console open; afterwards *Diagnostics* shows the load shedding and the receive rests of the NTP tasks. Only use it against your own device in your own network:

```
python -I testing/flood/flood.py udp <address> --port 123 --seconds 300
python -I testing/flood/flood.py udp <address> --port 123 --seconds 300 --ntp
```

## Quality of Service

### Jitter
Jitter is the variation in network delay between NTP messages exchanged with a time server. High jitter can reduce the accuracy and stability of time synchronization. For more information please see: [/testing/jitter/README.md](/testing/jitter/README.md)

### Drift
Drift is the gradual deviation of a computer's clock from the correct reference time due to hardware clock inaccuracies. NTP compensates for drift by regularly synchronizing the system clock with a time source. For more information and a python based drift testing tool please see: [/testing/drift/README.md](/testing/drift/README.md)

### NTP Server Alignment
The multiple internal NTP servers alignment can be verified to help ensure quality of service. Useful in quality assuring new builds and/or identifying potential network issues: [/testing/server_alignment/README.md](/testing/server_alignment/README.md)


