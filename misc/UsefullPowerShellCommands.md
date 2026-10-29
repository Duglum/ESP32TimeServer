# Useful Command Prompt Commands

Below, in more detail, are various Command Prompt commands that will allow you
to:

- Provide for Over The Ethernet (OTE) updates
- Test for NTP responses
- Reset the Meinberg drift file (to recreate a new baseline for the use of a
  different time server)

## Notes

In the examples below:

- The device name, `ESP32TimeServer`, is set as `DeviceName` in
  `ESP32TimeServerSettings.h` (default FQDN of the device; its first label is
  also the DHCP and mDNS host name).
- `timeserver1.example.net` is the DNS name (A and / or AAAA record) of the
  ESP32-P4 running the ESP32 time server software.
- `x.x.x.x` is the IP address of the ESP32-P4 running the ESP32 time server
  software.

## 1. Over The Ethernet (OTE) Updates

The firmware image is uploaded over the HTTPS web admin server (TCP 443), either on
the web admin page (section *Firmware update*) or with `tools\ote_upload.py`.
Over The Ethernet updates have to be switched on (web admin page, section
*Over The Ethernet updates*, or `OTE_UPDATES_ENABLED` in
`ESP32TimeServerSettings.h`) and the web admin server has to be enabled
(`WEB_ADMIN_ENABLED`).

Upload the application image of the build folder (`ESP32TimeServer.bin`), not the
merged flash image. Use the build profile that matches the chip revision of the
board (see `README.md`, `Setup Step 3 - Build`): the device refuses an image of
another project or for another chip revision before it writes anything.

The script checks the self-signed certificate of the device by its SHA-256
fingerprint (shown on the web admin page under *Status* and on the serial console
at every start) and sends the web admin password (`WebAdminPassword` in
`ESP32TimeServerSettingsSecrets.h`, or the one set on the web admin page) with
HTTP Basic authentication inside the TLS connection. It reads the password from
the environment variable `WEB_ADMIN_PASSWORD`, otherwise it asks for it. The
device refuses the script while its web admin password is the default
(`password`).

The device is given as an IP address or as a fully qualified DNS name; the
script looks the name up in the DNS (A and AAAA records, `-4` / `-6` for one
address family) and connects by unicast. `.local` (mDNS) and single-label names
are refused.

```cmd
python tools\ote_upload.py -i timeserver1.example.net -f build\ESP32TimeServer.bin --fingerprint AB:CD:...:EF
```

or

```cmd
python tools\ote_upload.py -i x.x.x.x -f build\ESP32TimeServer.bin --fingerprint AB:CD:...:EF
```

Without `--fingerprint` the script stops and prints the fingerprint the device
presented; `--insecure` skips the check (trusted LAN only). The device restarts 5
seconds after it accepted the image.

Several devices with the same web admin password (PowerShell 7; every device has
its own certificate, so each needs its own fingerprint):

```powershell
$env:WEB_ADMIN_PASSWORD = Read-Host -MaskInput "Web admin password"
$devices = @{
    "timeserver1.example.net" = "AB:CD:...:EF"
    "timeserver2.example.net" = "12:34:...:56"
}
foreach ($device in $devices.GetEnumerator()) {
    python tools\ote_upload.py -i $device.Key -f build\ESP32TimeServer.bin --fingerprint $device.Value
}
Remove-Item Env:WEB_ADMIN_PASSWORD
```

A device that still runs the firmware with the separate OTE password is
updated once with the script of that firmware (environment variable
`OTE_PASSWORD`) or on the web admin page.

A device that still runs a firmware with ArduinoOTA (UDP 3232) is updated once
with `tools\espota.py` (or by USB); after that the upload above is used:

```cmd
python tools\espota.py -i x.x.x.x -p 3232 -P 3232 -a ESP32TimeServerpw -f build\ESP32TimeServer.bin -r -d
```

## 2. NTP Test Request (ntpdate) - Linux or Windows (with Meinberg)

Here is a command which may be used for sending a NTP test request on Linux or
Windows (with the Meinberg Network Time Protocol Daemon service for Windows
installed and running):

```cmd
ntpdate -q ESP32TimeServer.local
```

or

```cmd
ntpdate -q x.x.x.x
```

With the output showing, for example:



```text
28 Aug 10:46:21 ntpdate[51120]: Raised to realtime priority class
server 192.168.7.24, stratum 1, offset +0.006410, delay 0.04124
28 Aug 10:46:24 ntpdate[51120]: adjust time server 192.168.7.24 offset +0.006410 sec
```



The `stratum 1` value indicates that the server is a Stratum 1 time source
(directly connected to a reference clock, in this case GNSS). The `offset` value
is the clock offset between the machine issuing the command and the
`ESP32TimeServer.local` device (in the case above approximately 6/1000's of a
second). The `delay` value is the round-trip network delay.

## 3. NTP Test Request (w32tm) - Windows only

If you are using Windows and running the default Windows Time service, here is a
command which may be used for sending NTP test requests (will not work if you
are not using Windows or using the Meinberg Network Time Protocol Daemon service
for Windows):

```cmd
w32tm /stripchart /computer:ESP32TimeServer.local /samples:5 /dataonly
```

or

```cmd
w32tm /stripchart /computer:x.x.x.x /samples:5 /dataonly
```



With the output showing, for example:

```text
Tracking 192.168.7.24 [192.168.7.24:123].
Collecting 5 samples.
The current time is 2026-08-24 8:23:44 AM.
08:23:44, +00.0022650s
08:23:46, +00.0016820s
08:23:48, +00.0016135s
08:23:50, +00.0017035s
08:23:52, +00.0018610s
```

The first number above is the current time. The second number above is the clock
offset between the machine issuing the command and the `ESP32TimeServer.local`
device (in the case above approximately 2/1000's of a second).

## 4. NTP Test Request (sntp) - MacOS

If you are using MacOS, here is a command which may be used for sending an NTP
test request (the `sntp` utility is included with MacOS by default):

```cmd
sntp -d ESP32TimeServer.local
```

or

```cmd
sntp -d x.x.x.x
```

With the output showing, for example:



```text
sntp 4.2.8p15@1.37-o
request 2026-08-24T08:24:01
rec:1715006401.000000 frac:0.000000000
refid: .GNSS.
leap: 0 stratum: 1 rootdelay: 0.000000
response from ESP32TimeServer.local: offset 0.002154, delay 0.040680
```



The offset value shown above is the clock offset between the machine issuing the
command and the `ESP32TimeServer.local` device (in the case above approximately
2/1000's of a second).

## 5. Resetting the Meinberg Drift File

If you are using the
[Meinberg NTP Time Server](https://www.meinbergglobal.com/english/sw/ntp.htm)
and you want to create a new baseline as you are now using a new time server,
then from the Windows Command Prompt with Administrative Privileges you can use
these commands:

```cmd
net stop ntp
del "C:\Program Files (x86)\NTP\etc\ntp.drift"
net start ntp
```

Between 15 minutes to an hour later a new `ntp.drift` file will be created by
the service.

If you don't do this the Meinberg software will automatically recalibrate the
drift file within a few hours.

## 6. NTP Stress Testing Tool

Here is an open source Windows app, written by me, which can be used to stress
test your NTP server.



[TimeServerStressTest](https://github.com/roblatour/TimeServerStressTest)

![Screenshot](https://raw.githubusercontent.com/roblatour/TimeServerStressTest/main/Misc/screenshot.jpg)


