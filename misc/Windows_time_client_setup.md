# Windows Time (w32time) as a client: `tools/configTimeSettings.ps1`

`tools/configTimeSettings.ps1` (from upstream release 4.0) sets up the time service built into Windows (W32Time) as an
NTP or a PTP client of this time server. It changes only this one computer. `.\tools\configTimeSettings.ps1 -help`
shows every option and the exit codes.

Windows Time is less accurate than chrony or the Meinberg NTP daemon (see [Setup.md](Setup.md)); use the script when
the built-in service is wanted, or to try PTP on Windows.

## Requirements

- Windows 11 23H2 or newer, or Windows Server 2022 or newer; 64-bit **Windows PowerShell 5.1** (`powershell.exe`, not
  PowerShell 7).
- A physical, standalone computer: the script refuses virtual machines, domain members, domain controllers, a managed
  W32Time policy and other running time services (chrony, ntpd, Meinberg, PTP services).
- `-set`, `-restore` and `-backup` need an elevated (administrator) PowerShell; `-inventory`, `-readiness` and `-help` do
  not.

### Settings on the time server's web page

| Mode | Settings |
|---|---|
| NTP | *NTP authentication required* off (panel *NTP authentication*): Windows sends unsigned requests. |
| PTP | Panel *PTP*: *PTP over UDP/IPv4* on, domain 0, *PTP authentication* off (Windows sends unsigned Delay_Req and expects unsigned PTPv2). IPv4 must be among the address families (panel *Network*). |

The time server sends PTP only while it is at stratum 1 with GNSS and PPS. It has no PTP holdover, and Windows has no
automatic NTP fallback: without PTP, Windows gets no time updates until PTP comes back or NTP is set again. The time
server answers Delay_Req sent to the PTP multicast group (the script sets `EnableMulticastTx` 1, so Windows sends them
there).

## Commands

```powershell
.\tools\configTimeSettings.ps1 -inventory 192.168.8.123      # adapters, time stamp support, reachability (no changes)
.\tools\configTimeSettings.ps1 -readiness 192.168.8.123      # which adapter Windows PTP is expected to use (no changes)
.\tools\configTimeSettings.ps1 -set NTP -serveraddress1 192.168.8.123
.\tools\configTimeSettings.ps1 -set NTP -serveraddress1 192.168.8.123 -serveraddress2 2001:db8::123
.\tools\configTimeSettings.ps1 -set PTP 192.168.8.123
.\tools\configTimeSettings.ps1 -backup
.\tools\configTimeSettings.ps1 -restore .\configtimebackup_2026_11_09_10_00_00.bak
```

Addresses are literal IP addresses (no DNS names). PTP, `-inventory` and `-readiness` take an IPv4 address; NTP takes up
to four IPv4 or IPv6 peers (a link-local IPv6 address needs its scope, e.g. `fe80::1%12`).

## What the script changes

Run `-backup` first; every `-set` also writes a safety backup before it changes anything, and rolls back if a readback
fails. The backups (`configtimebackup_*.bak`, JSON with a SHA-256 digest, readable only by the current user,
administrators and SYSTEM) are written into the current directory; keep them, they contain network details.

- Registry values under `HKLM\SYSTEM\CurrentControlSet\Services\W32Time` (only these are saved and restored):
  `Parameters\Type`, `Parameters\NtpServer`, `TimeProviders\NtpClient\Enabled` and `InputProvider`,
  `TimeProviders\VMICTimeProvider\Enabled` (saved and restored, never set), the `TimeProviders\PtpClient` values (`PtpMasters`, `DllName`,
  `Enabled`, `InputProvider`, `DelayPollInterval`, `AnnounceInterval`, `EnableMulticastRx`, `EnableMulticastTx`) and
  `Config\MinPollInterval`, `MaxPollInterval`, `UpdateInterval`.
  - NTP: `w32tm /config /manualpeerlist:... /syncfromflags:manual`, NtpClient on, PtpClient off, the Windows defaults
    for a standalone computer (poll 2^10 to 2^15 s).
  - PTP: PtpClient on with this server as master (multicast receive and transmit), NtpClient off (its peer list is
    kept), Microsoft's high-accuracy profile (poll 64 s, update every second).
- The Windows Time service is set to start automatically (not delayed).
- PTP only: two firewall rules owned by the script (`ESP32TimeServer-configTimeSettings-PTP-In` / `-Out`: UDP 319 and
  320, Private profile, the server and 224.0.1.129, the wired adapters on the server's subnet). The network profile of
  that adapter must already be *Private*; the script does not change it. `-set NTP` removes these rules again.
- After a `Y` to *Restart the Windows Time service now?* it restarts the service and watches for up to 3 minutes for a
  synchronisation from the expected source; for PTP it also reports the adapter Windows chose (event 513 of the
  PTP-Operational log) and watches 2 minutes for rejected samples (event 34).

The wall clock, the time zone and other network settings are not changed. `-restore <file>` puts the saved values, the
service start mode and state and the script's firewall rules back (only on the computer that made the backup).

## PTP samples rejected

Windows Time event 34 (a rejected, implausibly large correction) right after `-set PTP` means Windows used a zero PTP
time stamp. This firmware sends an originTimestamp in every Sync; check that the time server is the only PTP master in
domain 0 on that network (another master without it could be the source), that its PTP panel shows *serving*, and
restore the backup if it does not go away.

## Tests

`testing/configTimeSettings/run_tests.ps1` (upstream's suite, run in Windows PowerShell 5.1 from the repository root)
checks the script with mocked Windows functions; it changes nothing on the computer except files in a temporary folder.
The backup tests read the firewall rules, which Windows allows only in an elevated PowerShell: run the suite as
administrator to get through all of them.
