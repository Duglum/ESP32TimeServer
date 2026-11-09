# Web admin API

The web admin page of the ESP32 Time Server talks to the device through a small JSON API under `https://<FQDN or IP address>/api/`. This file lists every endpoint, how to call it and how the authentication works. Source: `main/web/web_server.cpp` (route table in `register_handlers()`), `main/web/web_auth.cpp` and `main/web/web_auth.h`.

## Authentication

1. **Allow-list.** Each new TCP connection is checked against the allow-list first. A source that is not allowed is closed before the TLS handshake. The allow-list applies to every path, the static files included.
2. **TLS.** Everything runs over HTTPS with the device's own certificate, TLS 1.3 only: a client that only knows TLS 1.2 (e.g. `curl.exe` of Windows 10, which uses Schannel) is refused; use a browser, Python or a curl built with OpenSSL. By default it is self-signed and created at the first start; an own certificate (e.g. of a public or company CA) can be set with `/api/tls`. Compare the SHA-256 fingerprint of the certificate in use with the one in the *Status* section or on the serial console.
3. **Login.** `POST /api/login` with `{"user": "...", "password": "..."}` returns:
   - a session cookie `__Host-sid` (a 256-bit token, `Secure; HttpOnly; SameSite=Strict`);
   - in the body the CSRF token, the user name and the method: `{"csrf": "...", "user": "admin", "radius": false}`. `GET /api/state` returns the CSRF token again (field `csrf`), so the page has it after a reload.

   `user` `""` or missing = the local user `admin` (the password of the page). Other user names are checked by the RADIUS servers while `settings.web_login.radius` is on, see *Logins with RADIUS* below.

   Session limits:
   - at most 4 sessions;
   - a session ends after 15 min without use, and after 12 h at the latest.
4. **Read requests (GET)** need a valid session cookie.
5. **State-changing requests (POST)** need the session cookie and all of the following. Otherwise the device answers 403.
   - `Origin: https://<Host>`, exactly matching the `Host` header;
   - `Sec-Fetch-Site: same-origin`, if the client sends this header;
   - `Content-Type: application/json`;
   - `X-CSRF-Token: <csrf>`.
6. **Throttling.** One counter per source address covers failed logins, failed password checks and failed scripted uploads. After 5 failures:
   - the device answers `429 Too Many Requests` with `Retry-After`;
   - the lock-out starts at 30 s and doubles with every further failure up to 1 h (30 s, 1, 2, 4, 8, 16, 32 min, then 1 h per failure: at most one guess per hour);
   - the failures of a source are forgotten after 24 h without a failure (the counter lives in RAM: a restart clears it).

   More than 20 failures per minute from all sources together block every login for 1 min (doubling up to 1 h, forgotten after 24 h without such a block).
7. **Password storage.** The password is stored as a salted PBKDF2 hash (10000 iterations) in NVS. The default password `password` is used until one is set on the page (there is no settings file: every setting is made on the page). While the default password is in use:
   - the page shows a red warning;
   - scripted uploads are refused.

Request bodies are limited to 4096 bytes; `/api/apply` and `/api/save` take up to 14336 bytes (the MQTT CA certificate may have 4096 characters, the RADIUS servers DNS names of 253), `/api/tls` up to 13312 bytes, `/api/config` (import) up to 27648 bytes (a complete settings set with the own TLS certificate and its key), the firmware upload more. A JSON body may nest at most 8 levels deep and have at most 512 separators (`,` and `:` outside strings); otherwise, or when it is no JSON object, the answer is `400`. Errors come back as `{"error": "..."}` with a matching HTTP status.

## Endpoints

| Endpoint | Method | Authentication | What it does |
|---|---|---|---|
| `/api/info` | GET | **none** | Host name and FQDN, `login_user` (RADIUS logins are on: the login page asks for a user name) |
| `/api/login` | POST | same origin, user name and password in the body | `{"user": "...", "password": "..."}`. Starts a session: cookie and the reply `{"csrf", "user", "radius"}`. `401` wrong user name or password (the same for both) or `No RADIUS server answered`, `429` with `Retry-After` while locked out (the body has the seconds as `retry_after`), `503` a login from the same address is still being checked by RADIUS |
| `/api/login/totp` | POST | same origin, the pending login | The second step of a login that needs a TOTP code: `{"pending": "...", "code": "123456"}` (`code`: the 6 digits of the authenticator app or a recovery code `abcde-fghij`). Answers as `/api/login` (session, `429`); a token set up at the first login adds `recovery_codes` (10, shown once). `401` `Wrong code` (counts for the lock-out; after 3 the password again: `Wrong code: log in again`) or `The login has expired: log in again` (2 minutes; 10 for a new token) |
| `/api/totp/enroll` | POST | cookie + CSRF | *Set up a token* for `{"user": "..."}` (1 to 64 characters; `admin` or any RADIUS user): a new key made on the device, kept for 10 minutes until it is confirmed: `{"user", "secret": "ABCD EFGH ...", "uri": "otpauth://totp/...", "qr": {"size": n, "rows": ["<hex>", ...]}, "replaces": bool}` (`rows`: one per line of the QR code, the left module the highest bit of the first hex digit, 1 = dark). `409` all 9 tokens in use |
| `/api/totp/confirm` | POST | cookie + CSRF | `{"user": "...", "code": "123456"}`: stores the token set up in this session with its first right code and answers `{"ok", "user", "recovery_codes": [10 codes]}` (shown only now). `400` wrong code, `409` expired or the clock not set (GNSS), `507` all tokens in use or NVS full |
| `/api/totp/delete` | POST | cookie + CSRF | `{"user": "..."}`: deletes the token. `409` for `admin` while `web_login.totp_local` is on, and for the own token while the second factor is required for the own login; `404` no token |
| `/api/logout` | POST | cookie + CSRF (without them only the cookie is cleared) | Ends the session |
| `/api/state` | GET | cookie | Settings in use (`settings`), `defaults`, `saved`, allow-list, `status` (chip: `chip_model` (`ESP32-P4` or `ESP32-S31`), `chip_revision`, `gpio_max` (highest GPIO number: 54 or 61; the pin fields of the settings accept -1 or 0 to this value, ESP32-S31: GPIO 29 and 41 are refused), `ptp_loopback_supported`; network: link state, speed (10, 100 or, ESP32-S31, 1000), duplex and driver restarts in `link_up`, `link_speed_mbps`, `link_full_duplex`, `eth_recoveries`; MQTT: `mqtt_active`, `mqtt_connected`, `mqtt_broker`, `mqtt_port`, `mqtt_tls`, `mqtt_error` with `mqtt_error_age_s`, `mqtt_connected_s`, `mqtt_published`, `mqtt_publish_age_s`, `mqtt_tf_queue`; NTP: `ntp_auth_available`, `ntp_auth_keys_in_use`, `ntp_rate_limited` (requests dropped by the per-client rate limit since the start), `ntp_unsigned_refused` (unsigned requests dropped while `ntp_auth_required` is on, since the start); time, GNSS, PPS, ...), certificate fingerprint, `tls` (see *TLS certificate* below), `radius_status` (see *RADIUS* below), `session` (the login of this session: `user`, `radius`, `totp`), `totp` (`time_ok`: the clock is set from GNSS, TOTP codes can be checked; `max` 9; `tokens`: `[{"user", "created", "last_used", "recovery_left"}]`, unix times, 0 = unknown / never), CSRF token. Secrets (passwords, keys, the shared secrets of the RADIUS servers) are not sent back |
| `/api/diag` | GET | cookie | Diagnostics: servo, ADEV/TDEV, ISR histogram, NTP paths, basic mode TX delay, load shedding, receive rests of the NTP tasks under a flood (`receive_rests`), RMC phase, UBX details, task stacks (`radius` once it was started), heap, `nvs` (entries of 32 bytes: `used`, `available` for new data, `total`, and `settings`, the entries of one stored settings set) |
| `/api/ptp` | GET | cookie | PTP master: state per transport, counters since the start and the current followers. See *PTP* below |
| `/api/coredump` | GET | cookie | Downloads the stored core dump (`404` if there is none) |
| `/api/apply` | POST | cookie + CSRF | Applies settings now, without storing them. The body is a settings object with the field names of `settings` in `/api/state`; missing fields keep their current value. See the notes below the table. Errors: `400` invalid value, `409` while a network change waits for its confirmation |
| `/api/network/confirm` | POST | cookie + CSRF | Confirms the pending network change (body `{}`). A new static address must be confirmed over that address (`409` otherwise) |
| `/api/save` | POST | cookie + CSRF | Stores the settings (same body as `/api/apply`) in NVS; they are loaded at every start. `409` while network changes are not applied and confirmed yet |
| `/api/config` | GET | cookie + CSRF header | *Export configuration*: the configuration file (JSON, `Content-Disposition: attachment; filename="timeserver-config-<host name>.json"`), see *Configuration file* below. Contains secrets, so besides the cookie it needs the `X-CSRF-Token` header, and a `Sec-Fetch-Site` header (sent by browsers) must be `same-origin` (`403` otherwise) |
| `/api/config` | POST | cookie + CSRF | *Import configuration*: the body is a configuration file. Replaces every setting and the allow-list, see *Configuration file* below. Errors: `400` not a configuration file or an invalid value, `409` while a network change waits for its confirmation or when the allow-list of the file does not allow your own address |
| `/api/reboot` | POST | cookie + CSRF | Restarts with the saved settings; applied but unsaved settings are discarded |
| `/api/password` | POST | cookie + CSRF + current password | `{"current": "...", "new": "..."}`; the new password has 12 to 128 characters |
| `/api/allow-list` | POST | cookie + CSRF | `{"entries": ["192.168.8.0/24", "2001:db8::/64", ...]}`: source addresses or networks (CIDR) allowed to connect. `409` if your own address would no longer be allowed |
| `/api/tls` | GET | cookie | The certificate in use and the stored own one: `{"tls": {...}}`, see *TLS certificate* below |
| `/api/tls` | POST | cookie + CSRF | `{"mode": "own", "certificate": "<PEM chain>", "key": "<PEM>"}` or `{"mode": "self"}`: see *TLS certificate* below. `400` with the reason if the certificate or key is refused |
| `/api/factory-reset` | POST | cookie + CSRF + current password | *Reset to defaults* (`{"password": "..."}`): erases the saved settings, the password, the allow-list, the own TLS certificate with its key and the TOTP tokens, then restarts. The self-signed key and certificate are kept. The button held at the start does the same |
| `/api/ote/once` | POST | cookie + CSRF | Restarts once with firmware updates (OTE) switched on, until the next restart |
| `/api/ote` | POST | cookie + CSRF (page) **or** HTTP Basic (script) | Firmware upload. The body is the raw application image (`ESP32TimeServer.bin`, `application/octet-stream`). `403` while OTE updates are switched off. See below |
| `/api/coredump/erase` | POST | cookie + CSRF | Erases the stored core dump |
| `/api/gnss/command` | POST | cookie + CSRF | GNSS console: sends one command to the GNSS receiver. See below |
| `/api/gnss/console` | GET | cookie | GNSS console: the recorded traffic after `?after=<seq>`. See below |
| `/api/radius/test` | POST | cookie + CSRF | *Test login* of *Security*: `{"user": "...", "password": "...", "radius": {...}}` with the RADIUS fields of the form (as in `settings.radius`; an empty `secret` keeps the one in use; missing fields: the settings in use). Asks the servers like a login and answers when it is done (up to servers × (1 + retries) × timeout): `{"result": "accept" \| "reject" \| "low_privilege" \| "missing_attribute" \| "no_answer" \| "busy", "text": "...", "server": 1, "rtt_ms": 14, "challenge": false, "privilege": 15, "summary": "Service-Type = Administrative-User, Cisco-AVPair = \"shell:priv-lvl=15\""}` (`server`, `rtt_ms`, `challenge`, `summary` with an answer, `privilege` with an Access-Accept). Not counted for the login throttling, does not mark a server dead. `400` invalid fields, `503` too many RADIUS logins at once |

Notes on `/api/apply`:
- A network change must be confirmed within 120 s, otherwise the previous network settings are restored.
- A change of a restart field (FQDN, pins, LED module, OTE, ...) writes a one-shot set and restarts once with it. The MQTT settings (except the TF card pins) take effect at once: the client stops and connects again with the new settings.
- MQTT broker: `mqtt_broker` (IPv4 or IPv6 address or DNS name, no `.local`), `mqtt_port` (0 = 8883 with TLS, 1883 without), `mqtt_tls`, `mqtt_ca_pem` (CA certificate of the broker in PEM, at most 4096 characters, required with TLS; parsed with mbedTLS, `400` if it cannot be read). With TLS the broker certificate is checked against this CA and the broker name or IP address (SAN). The former field `mqtt_uri` (`mqtt://host[:port]` or `mqtts://host[:port]`) is still accepted and sets broker, port and TLS; fields sent with it replace these values.

- NTP authentication (takes effect at once): `ntp_auth_enabled`, `ntp_keys` (up to 8 `{"id": 1 to 65535, "key": "<32 letters or digits>"}`), `ntp_auth_required` (default false = mixed: unsigned NTPv3 and NTPv4 requests are answered; true: only requests with a valid MAC are answered, unsigned ones, including every NTPv3 request, are dropped without an answer and counted in `ntp_unsigned_refused`). Requests with an unknown key ID, a wrong MAC or a malformed trailer are always dropped (no crypto-NAK). `ntp_auth_required` needs `ntp_auth_enabled` and at least one key (`400` otherwise).

- PTP master (all fields take effect at once): `ptp_l2_enabled`, `ptp_udp4_enabled`, `ptp_udp6_enabled` (UDP/IPv4 224.0.1.129 and UDP/IPv6 ff02::181, ports 319 and 320; the IP version must be in use, see `ip_mode`), `ptp_domain` (0 to 127), `ptp_priority1`, `ptp_priority2` (0 to 255), `ptp_log_sync_interval` (-3 to 4, 2^n s), `ptp_log_announce_interval` (0 to 4), `ptp_log_min_delay_req_interval` (-3 to 6), `ptp_master_selection` (true: stand aside for a better master; false: always master), `ptp_clock_accuracy` (clockAccuracy of the Announce: 32 to 49 = 0x20 to 0x31, or 254 = unknown), `ptp_follower_timeout` (5 to 3600 s), `ptp_delay_req_rate_limit` (0 = no limit, up to 1000 per follower and second), `ptp_utc_offset` (TAI - UTC, 0 to 100 s; used while the GNSS receiver does not tell it), `ptp_ingress_latency_ns`, `ptp_egress_latency_ns` (-100000 to 100000).

- PTP authentication (takes effect at once; AUTHENTICATION TLV of IEEE 1588-2019 16.14 with HMAC-SHA256-128, as linuxptp `SHA256-128`, on every PTP transport): `ptp_auth_enabled` (sign every sent message, check signed Delay_Req / Announce), `ptp_auth_required` (also drop unsigned Delay_Req / Announce; needs `ptp_auth_enabled`), `ptp_auth_mutable` (the correctionField is not covered by the ICV, linuxptp `allow_mutable 1`), `ptp_auth_spp` (0 to 255), `ptp_auth_active_key_id` (1 to 65535, must be one of the keys while authentication is on), `ptp_keys` (up to 4 `{"id": 1 to 65535, "key": "<32 letters or digits>"}`, sent like `ntp_keys`: the list replaces the keys, a missing list keeps them; the key is used as it is as the HMAC key, linuxptp `ASCII:<key>`). `400` when authentication is on without a key or with an active key ID that is not one of the keys.

- Logins of the web page (take effect at once; *Security*): the objects `radius` and `web_login` (a missing field keeps its value).

Fields of `radius` (defaults in brackets). The API never returns a shared secret: a server has `secret_set` (bool) instead, and an empty or missing `secret` keeps the stored one. The configuration file (export) carries the secrets:

| Field | Type | Meaning |
|---|---|---|
| `servers` | list of up to 2 objects | `host` (an IPv4 or IPv6 address or a DNS name, `""` = not used; a server without a host loses its secret), `port` (1 to 65535, 1812), `secret` (the shared secret, 1 to 64 printable ASCII characters; required with a host). Server 2 is asked when server 1 does not answer. A login method with RADIUS needs at least one server |
| `timeout_s` | 1 to 30 | Seconds per try (3) |
| `retries` | 0 to 5 | More tries per server (2) |
| `dead_time_min` | 0 to 60 | A server that did not answer is skipped this long; 0 = never skipped (2) |
| `nas_identifier` | text, at most 64 characters | NAS-Identifier of the requests, `""` = the host name (`""`) |
| `nas_ip`, `nas_ipv6` | text | NAS-IP-Address / NAS-IPv6-Address, `""` = the address the request is sent from (`""`) |
| `service_type` | 0, 1, 6, 7 | Service-Type of the requests: Login, Administrative, NAS-Prompt; 0 = not sent (1) |
| `nas_port_type` | -1, 0, 5 | NAS-Port-Type: Async, Virtual; -1 = not sent (5) |
| `min_privilege` | 1 to 15 | An Access-Accept is accepted from this privilege level: `Cisco-AVPair` `shell:priv-lvl=N`, without it Service-Type Administrative-User = 15, anything else = 1 (15) |
| `required` | text | `""` or an attribute the Access-Accept must also carry, compared exactly: `Cisco-AVPair=<value>`, `Class=<value>` or `Filter-Id=<value>` (`""`) |
| `require_message_authenticator` | bool | An answer without a valid Message-Authenticator is dropped (BlastRADIUS, CVE-2024-3596); requests always carry one (true) |

Fields of `web_login` (defaults in brackets):

| Field | Type | Meaning |
|---|---|---|
| `local` | bool | Login of the user `admin` with the password of the page (true) |
| `radius` | bool | Login with user name and password, checked by the RADIUS servers (false). At least one of `local` and `radius` stays on |
| `local_fallback` | bool | With both: every user name is checked by RADIUS first, the local password of `admin` only when no RADIUS server answers. false: `admin` is checked with the local password only, other names by RADIUS (true) |
| `totp_local`, `totp_radius` | bool | A TOTP code is required after the password of the local user / of a RADIUS user (false) |
| `totp_enroll` | bool | A RADIUS user without a TOTP token sets one up at the first login; false: a user without a token is refused while `totp_radius` is on (false) |

- `web_login.local` can only be switched off (also by `/api/save` and the import) in a session that RADIUS logged in (`400` otherwise): a wrong RADIUS setting cannot lock you out.
- `web_login.totp_local` can only be switched on while `admin` has a TOTP token (`400` otherwise).

## Logins with RADIUS

The local user `admin` logs in with the password of the page while `settings.web_login.local` is on; any other user name (and `admin` too while the local password is off or `local_fallback` is on) is checked by the RADIUS servers while `web_login.radius` is on. With both and `local_fallback` (default) every name is asked from RADIUS first and the local password of `admin` is checked only when no RADIUS server answers (like `aaa authentication login default group radius local` of a Cisco switch); without it `admin` is checked with the local password only. An Access-Accept below `radius.min_privilege` (default 15) or without `radius.required` is refused. Every refusal counts for the lock-out of the source, also "No RADIUS server answered"; a user name that no method takes is refused after the same PBKDF2 check as a wrong password. A login waiting for RADIUS (up to servers × (1 + retries) × timeout) does not hold up the other requests; one per source address at a time (`503` for a second one). The requests carry User-Name, User-Password (PAP), Message-Authenticator (first), NAS-Identifier, NAS-IP-Address or NAS-IPv6-Address, NAS-Port 0, NAS-Port-Id `https`, NAS-Port-Type, Service-Type and Calling-Station-Id (the address of the client).

Second factor: while `web_login.totp_local` (the local user) or `web_login.totp_radius` (RADIUS users) is on, a right password is answered with `{"pending": "...", "user": "...", "totp": true}` (no session yet): the page asks for the code and sends it to `/api/login/totp`. A user without a token is refused (`401` `No TOTP token is set up for this user`), unless `web_login.totp_enroll` is on and it is a RADIUS user: then the answer is `{"pending", "user", "totp_enroll": true, "secret", "uri", "qr"}` (as `/api/totp/enroll`) and the first right code stores the token and logs in. The lock-out forgets the failures of the source only after the whole login. TOTP codes need the clock of the device (GNSS); while it is not set only recovery codes work.

TOTP tokens (`/api/totp/*`): one per user name, at most 9, stored in NVS (namespace `totp`, about 300 bytes each), not in the configuration file, erased by *Reset to defaults*. A new token is only stored while NVS keeps room for one more settings set (`507` otherwise; `nvs` in `/api/diag` shows the room): with the 24 kB NVS partition of this device and an own TLS certificate stored that is about 4 tokens.

**Static files** need no login; only the allow-list applies: `/`, `/app.js`, `/app.css`, `/i18n.js`, `/favicon.ico`.

## RADIUS (`radius_status` in `/api/state`)

`running` (the RADIUS client task runs: it is started once a login method uses RADIUS or a test is made, then it stays), `servers`: per server 1 and 2 `used` (an address is set), `host`, `address` (the address last used, `""` before the first login), `port`, `dead` (skipped because it did not answer) with `dead_for_s`, `last_answer_s` (-1 = none), `rtt_ms` (of the last answer), and the counters since the start or the last change of its address: `requests` (packets sent, retransmissions included), `accepts`, `rejects` (Access-Reject and Access-Challenge), `timeouts` (logins it did not answer), `dropped` (invalid answers: wrong authenticator, Message-Authenticator, identifier or length). Counters of all logins: `accepted`, `rejected`, `refused` (an Access-Accept below the privilege level or without the required attribute), `no_answer`, `busy` (refused because too many logins were asked at once or one from the same address was still asked), `active` (logins being asked now). The test of *Security* is not counted in the servers' rows.

## Configuration file (`/api/config`)

```json
{
  "timeserver_config": 1,
  "settings": { "fqdn": "ESP32TimeServer", "...": "every field of settings in /api/state, with the secrets" },
  "hostname": "ESP32TimeServer",
  "firmware": "4.0.0-WebUI",
  "exported": "2026-11-01T10:15:00Z",
  "allow_list": ["192.168.8.0/24"],
  "tls": { "mode": "own", "certificate": "-----BEGIN CERTIFICATE-----\n...", "key": "-----BEGIN PRIVATE KEY-----\n..." }
}
```

- `timeserver_config`: the format (1). A file without it, or of another format, is refused.
- `settings`: the settings in use, with the field names of `settings` in `/api/state`, but with the MQTT password in clear text (`mqtt_password`) in place of `mqtt_password_set` and the shared secrets of the RADIUS servers (`secret`) in place of `secret_set`; the NTP and PTP keys (`ntp_keys`, `ptp_keys`) are there as on the page. On import a field the file lacks gets its default. A file of an earlier firmware with `mqtt_uri` instead of `mqtt_broker`, `mqtt_port` and `mqtt_tls` is read as with `/api/apply`; with `mqtts://` and MQTT on it needs `mqtt_ca_pem` as well.
- `hostname`, `firmware`, `exported` (UTC, only when the clock is set): for information, ignored on import.
- `allow_list`: at most 8 entries as for `/api/allow-list`. On import a file without it leaves the allow-list unchanged; `[]` clears it.
- `tls`: `mode` (`own` or `self`) and, when an own certificate is stored, `certificate` (PEM chain) and `key` (PEM, in clear text). On import a file without it leaves the certificate unchanged; `{"mode": "self"}` switches to the self-signed one (a stored own one is kept); `own` needs `certificate` and `key` and is checked as for `/api/tls`.
- Not in the file: the password of the web page and the self-signed key and certificate (they never leave the device).

Import (`POST /api/config`): the settings are checked like `/api/apply` and `/api/save`, and the own TLS certificate as for `/api/tls`; then the allow-list is stored (refused with `409` if it would not allow your own address), then the TLS certificate (when it changed and the device does not restart, it closes its connections after the answer, so that the browser connects again with it). Then:
- without a change of the network settings the set is saved in NVS and takes effect; if a restart field changed, the device restarts with it (`"restart": true`);
- with a change of the network settings the set is applied like `/api/apply` and not saved: confirm within 120 s (`network_pending`), then save it with `/api/save`; with a restart field it is written as a one-shot set and the device restarts once.

Answer: `{"ok": true, "message": "...", "restart": ..., "network_changed": ..., "saved": ..., "network_pending": {...}}`.

## TLS certificate (`/api/tls`, `tls` in `/api/state`)

The web admin uses the self-signed certificate of the device (default) or an own one. The own certificate is checked before it is stored:
- `certificate`: one or more PEM blocks `CERTIFICATE`, at most 8192 characters: the server certificate first, then its intermediates (the root may be left out);
- `key`: one PEM block `PRIVATE KEY` (PKCS#8), `EC PRIVATE KEY` (optionally after `EC PARAMETERS`) or `RSA PRIVATE KEY`, at most 4096 characters, not encrypted; ECDSA P-256 or P-384, or RSA with 2048 to 4096 bits; it has to belong to the first certificate (proven by a signature made with the key and verified with the certificate).

Not checked (the page shows warnings instead): the validity dates and whether the names fit the address the page is opened with. `certificate` or `key` missing or empty keeps the stored one, so `{"mode": "own"}` switches back to a stored own certificate. `{"mode": "self"}` uses the self-signed certificate; the own one stays stored. The certificate is stored in NVS at once and used for new connections; after the answer the device closes its connections, so that browsers connect again with it (and ask about it if it is not trusted). At every start the stored own certificate is checked again; one that does not pass is not used (a warning on the console) and the self-signed one serves. The key is never sent back. Answer: `{"ok": true, "message": "...", "tls": {...}}`.

`tls` (in `/api/state`, `/api/tls` and the answer of `POST /api/tls`): `mode` (`own` or `self`, the one in use), `fingerprint` (SHA-256 of the certificate in use), `own` (`null` when none is stored): `subject`, `issuer`, `san` (DNS names and IP addresses, at most 8), `not_before`, `not_after` (UTC), `days_left` (`null` while the clock is not set; negative when expired), `valid_now`, `key_type` (`"ECDSA P-256"`, `"RSA 2048"`, ...), `chain_length`, `fingerprint`.

Example of renewing it from a script (e.g. a certbot deploy hook), with the session of *Example with curl* below:

```sh
python3 -c 'import json,sys; print(json.dumps({"mode": "own", "certificate": open(sys.argv[1]).read(), "key": open(sys.argv[2]).read()}))' \
    fullchain.pem privkey.pem > tls.json
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" -H "X-CSRF-Token: <csrf>" \
     --data-binary @tls.json https://HOST/api/tls
```

`tools/ote_upload.py --fingerprint` takes the fingerprint of the certificate in use, the own one included.

## GNSS console (`/api/gnss/command`, `/api/gnss/console`)

The *GNSS console* sub-panel of *GNSS and PPS* sends commands typed by the user to the GNSS receiver. The firmware itself sends nothing through it.

**`POST /api/gnss/command`**, body `{"mode": "text" | "ubx", "command": "...", "checksum": true, "crlf": true}`:
- `text` sends the command as typed (1 to 200 printable ASCII characters). The two options are independent:
  - `checksum` appends `*hh`: the XOR of the characters after a leading `$` or `!`, or of all characters without one. A command that already contains `*` is refused while this is on.
  - `crlf` appends CR LF.
- `ubx` takes hexadecimal bytes. Spaces, commas, colons, dashes and `0x` are allowed.
  - `<class> <ID> <payload>`: the sync bytes, the length and the checksum are added. The payload has at most 100 bytes.
  - Or a whole frame starting with `B5 62`: it is sent only if its length and checksum are right.
  - It is sent between the firmware's own UBX commands. It is only allowed with the protocol U-Blox UBX (`409` otherwise).
- The answer is `{"ok": true, "sent": "<the command as recorded>"}`.
- Errors:
  - `400` an invalid command (the reason in `error`);
  - `409` UART not running, or UBX without the u-blox protocol;
  - `429` more than 4 commands per second;
  - `503` receiver busy for 2 s, or out of memory.
- Every command is logged on the serial console with the source address.

**`GET /api/gnss/console?after=<seq>`** returns the entries after the sequence number `seq`. Each call keeps the recording on for 10 s; without calls the reader records nothing. The answer:
- `entries`: a list of `{"seq", "kind", "periodic", "text"}`.
  - `kind` is `sent` (a command), `text` (a received text line, any printable line) or `ubx` (a received UBX frame: class-ID, ACK/NAK, the MON-VER strings or the payload in hex).
  - `periodic` marks the per-second messages: the standard NMEA sentences (RMC, GGA, GSV, ...), UBX NAV and TIM-TP.
- `last`: the sequence number for the next call.
- `skipped`: older entries were overwritten (the ring keeps 48).
- `reset`: the number was from before a restart, so the list starts again.
- `running`: the GNSS UART runs.
- `ublox`: the protocol is U-Blox UBX.

## PTP (`/api/ptp`)

Read by the page every 10 s while the PTP panel is open. The PTP master serves only while the server is at stratum 1 with GNSS and PPS (no holdover).

```json
{"started": true, "clock_identity": "80f1b2.fffe.d6581d", "utc_offset": 37, "utc_offset_source": "setting",
 "authentication": {"signing": true, "required": false},
 "ports": [{"transport": "l2", "enabled": true, "state": "serving", "followers": 2, "sync": 3600, "follow_up": 3598,
            "announce": 1800, "delay_req": 7200, "delay_resp": 7190, "rate_limited": 3, "drops": 0, "invalid": 1,
            "ignored": 12, "rx_timestamp_misses": 0, "tx_timestamp_misses": 2, "transmit_failures": 0, "foreign_announces": 0,
            "auth_rejected": 0, "unsigned_refused": 0},
           {"transport": "udp4", "enabled": false, "state": "disabled", ...},
           {"transport": "udp6", "enabled": false, "state": "disabled", ...}],
 "followers": [{"transport": "l2", "mac": "00:11:22:33:44:55", "port_identity": "001122.fffe.334455-1", "last_seen_s": 1}]}
```

- `started`: the PTP task runs (it needs the hardware time stamps of the Ethernet MAC). `clock_identity`: EUI-64 of the Ethernet MAC address. `utc_offset`: currentUtcOffset in use, from the u-blox receiver (`gnss`) or the setting (`setting`).
- `authentication`: `signing` (sent messages carry the AUTHENTICATION TLV), `required` (unsigned Delay_Req / Announce are dropped). Required without a usable key: no port serves (state `authentication required but no usable key`).
- `state`: `serving`, `disabled`, `authentication required but no usable key`, `no Ethernet link`, `no address for this IP version` (UDP), `PTP clock not mapped to true time`, `not synchronized (stratum 1 needs GNSS and PPS)`, `PPS holdover`, `listening for other masters`, `a better master is present`.
- Counters since the start: Sync, Follow_Up, Announce and Delay_Resp sent; Delay_Req received; `rate_limited` (over the per-follower limit, not answered), `drops` (queue full or over the limit of 1000 PTP frames per second), `invalid`, `ignored` (other message types and domains), `rx_timestamp_misses` (Delay_Req without a usable receive time stamp), `tx_timestamp_misses` (Sync without a usable transmit time stamp: no Follow_Up), `transmit_failures` (also a message that could not be signed: it is not sent unsigned), `foreign_announces` (Announces of other masters in the domain), `auth_rejected` (Delay_Req / Announce whose AUTHENTICATION TLV did not verify: unknown key, wrong ICV, other spp, malformed TLV, or authentication off), `unsigned_refused` (unsigned Delay_Req / Announce while authentication is required).
- `followers`: followers that sent a Delay_Req within the follower timeout (at most 96), with the seconds since their last one.

## Firmware upload from a script (`/api/ote`)

`tools/ote_upload.py` uses this path. The request carries `Authorization: Basic base64(<any user name>:<web admin password>)` inside the TLS connection. It needs no cookie, CSRF token or `Origin`.

- The upload is refused with `403` while the default password is in use, and while the local password is switched off (`web_login.local`): use the page then.
- While `web_login.totp_local` is on, a scripted upload also needs the TOTP code (or a recovery code) of `admin` in the header `X-TOTP-Code` (`tools/ote_upload.py --totp` asks for it); a wrong or missing code is a wrong password (`401`, counted). While `web_login.local` is off, scripted uploads are refused (`403`): use the page.
- A wrong password counts for the throttling like a failed login: `401`, and later `429`.
- On a refusal the device answers as soon as it has the headers and the first byte of the body. It then closes the connection without reading the image. The script therefore sends the headers with the first byte and waits up to 2 s for an answer before it sends the rest. Otherwise Windows can lose the answer to a connection reset.

## Example with curl

The certificate is self-signed: `-k` skips the check, so compare the fingerprint yourself. Replace `HOST` with the FQDN or IP address.

```bash
# Log in: stores the session cookie in "jar" and prints the CSRF token
curl -k -c jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -d '{"password":"<password>"}' https://HOST/api/login

# Read the state
curl -k -b jar https://HOST/api/state

# Export the configuration (contains secrets)
curl -k -b jar -H "X-CSRF-Token: <csrf>" -o timeserver-config.json https://HOST/api/config

# Import it again
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -H "X-CSRF-Token: <csrf>" --data-binary @timeserver-config.json https://HOST/api/config

# State-changing request, e.g. a restart
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -H "X-CSRF-Token: <csrf>" -d '{}' https://HOST/api/reboot
```

Keep this file up to date when an endpoint, its authentication or its behaviour changes.
