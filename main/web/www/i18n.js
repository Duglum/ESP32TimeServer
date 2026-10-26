// ESP32 Time Server - web admin page: German texts
// English is the source text (index.html, app.js, messages of the firmware) and the default language; German is
// used once it was chosen with the language button (cookie "lang=de"). Keys are the English texts with runs of
// white space reduced to one space; texts with inline markup are keyed by their HTML (the German HTML keeps the
// same tags and ids). Technical terms, network terms, abbreviations and acronyms stay English.
'use strict';

(function () {
  const match = /(?:^|;\s*)lang=(de|en)(?:;|$)/.exec(document.cookie);
  const lang = match ? match[1] : 'en';
  document.documentElement.lang = lang;
  if (lang === 'de') {
    // Hidden until app.js has translated the page; shown after 3 s in any case
    document.documentElement.classList.add('i18n-pending');
    setTimeout(() => document.documentElement.classList.remove('i18n-pending'), 3000);
  }

  const de = {
    // ---- Header, banners, login ----
    'Language: English (click for German)': 'Sprache: Deutsch (klicken für Englisch)',
    'Switching the language reloads the page: changes that were not applied are lost. Continue?':
      'Der Sprachwechsel lädt die Seite neu: nicht übernommene Änderungen gehen verloren. Fortfahren?',
    'Dark mode (click for light mode)': 'Dunkler Modus (klicken für hellen Modus)',
    'Light mode (click for dark mode)': 'Heller Modus (klicken für dunklen Modus)',
    'Log out': 'Abmelden',
    'The factory default password is in use. Anyone on the network can change the settings of this server. Change the password in the Security section.':
      'Das Standardpasswort ab Werk ist aktiv. Jeder im Netzwerk kann die Einstellungen dieses Servers ändern. Ändern Sie das Passwort im Abschnitt Sicherheit.',
    'Network change pending.': 'Netzwerkänderung ausstehend.',
    'Confirm within': 'Innerhalb von',
    's or the previous settings come back.': 's bestätigen, sonst werden die vorherigen Einstellungen wiederhergestellt.',
    'Open the page over the new address, log in again and confirm there:': 'Öffnen Sie die Seite über die neue Adresse, melden Sie sich erneut an und bestätigen Sie dort:',
    'Keep these network settings': 'Diese Netzwerkeinstellungen behalten',
    'This start uses one-time settings (from <em>Apply</em> or <em>Restart with updates enabled</em>): the next restart returns to the saved settings. Use <em>Save Settings</em> to keep them.':
      'Dieser Start verwendet einmalige Einstellungen (von <em>Übernehmen</em> oder <em>Neustart mit aktivierten Updates</em>): Der nächste Neustart kehrt zu den gespeicherten Einstellungen zurück. Mit <em>Einstellungen speichern</em> bleiben sie erhalten.',
    'The settings in use differ from the saved settings. They are lost at the next restart unless you use <em>Save Settings</em>.':
      'Die aktiven Einstellungen weichen von den gespeicherten ab. Sie gehen beim nächsten Neustart verloren, wenn Sie nicht <em>Einstellungen speichern</em> verwenden.',
    'Log in': 'Anmelden',
    'Password': 'Passwort',
    'Factory default password: <code>password</code> (change it after the first login).':
      'Standardpasswort ab Werk: <code>password</code> (nach der ersten Anmeldung ändern).',
    'Check the certificate fingerprint in the browser against the one printed on the serial console at every start.':
      'Vergleichen Sie den Fingerprint des Zertifikats im Browser mit dem, der bei jedem Start auf der seriellen Konsole ausgegeben wird.',
    '<a href="https://github.com/Duglum/ESP32TimeServer" target="_blank" rel="noopener noreferrer">ESP32TimeServer WebInterface fork</a> · based on <a href="https://github.com/roblatour/ESP32TimeServer" target="_blank" rel="noopener noreferrer">ESP32TimeServer</a> by Rob Latour · MIT license':
      '<a href="https://github.com/Duglum/ESP32TimeServer" target="_blank" rel="noopener noreferrer">ESP32TimeServer WebInterface Fork</a> · basierend auf <a href="https://github.com/roblatour/ESP32TimeServer" target="_blank" rel="noopener noreferrer">ESP32TimeServer</a> von Rob Latour · MIT-Lizenz',

    // ---- Status ----
    'Status': 'Status',
    'Time': 'Zeit',
    'Time service': 'Zeitdienst',
    'GNSS receiver': 'GNSS-Empfänger',
    'NTP authentication': 'NTP-Authentifizierung',
    'Device': 'Gerät',
    'Chip revision': 'Chip-Revision',
    'Uptime': 'Laufzeit',
    'Free heap': 'Freier Heap',
    'Certificate SHA-256': 'Zertifikat SHA-256',
    'Network': 'Netzwerk',
    'DNS servers in use': 'Verwendete DNS-Server',
    'DNS received (DHCPv4)': 'DNS empfangen (DHCPv4)',
    'DNS received (IPv6 RA)': 'DNS empfangen (IPv6 RA)',
    'Your address': 'Ihre Adresse',
    'NTP rate limit': 'NTP-Rate-Limit',
    '{limit} per second and client, {n} requests dropped since the start': '{limit} pro Sekunde und Client, {n} Anfragen seit dem Start verworfen',
    'off, {n} requests dropped since the start': 'aus, {n} Anfragen seit dem Start verworfen',
    'stratum 1, PPS holdover (PPS lost)': 'Stratum 1, PPS-Holdover (PPS verloren)',
    '{n} satellites in use, fix quality {q} (GGA)': '{n} Satelliten in Verwendung, Fix-Qualität {q} (GGA)',
    'RMC {ms} ms after the PPS ({min} to {max} ms since the start)': 'RMC {ms} ms nach dem PPS ({min} bis {max} ms seit dem Start)',
    '1 value differs from the default': '1 Wert weicht vom Standard ab',
    '{n} values differ from the default': '{n} Werte weichen vom Standard ab',
    '{n} satellites in use (NAV-PVT)': '{n} Satelliten in Verwendung (NAV-PVT)',
    'qErr correction: learning the sign': 'qErr-Korrektur: Vorzeichen wird gelernt',
    'qErr correction active (slope {slope})': 'qErr-Korrektur aktiv (Steigung {slope})',
    'qErr correction off: no correlation (slope {slope})': 'qErr-Korrektur aus: keine Korrelation (Steigung {slope})',
    'no qErr (TIM-TP missing or PTP loopback off)': 'kein qErr (TIM-TP fehlt oder PTP-Loopback aus)',
    'stratum 1, GNSS and PPS locked': 'Stratum 1, GNSS und PPS synchronisiert',
    'not stratum 1 (PPS {pps}, GNSS {gnss})': 'nicht Stratum 1 (PPS {pps}, GNSS {gnss})',
    'not stratum 1 yet: GNSS fix {age} s old, {min} s required after the start':
      'noch nicht Stratum 1: GNSS-Fix {age} s alt, {min} s nach dem Start nötig',
    'active': 'aktiv',
    'missing': 'fehlt',
    'locked': 'synchronisiert',
    'not locked': 'nicht synchronisiert',
    'waiting for the GNSS time': 'warte auf die GNSS-Zeit',
    'not set yet': 'noch nicht gesetzt',
    'U-Blox UBX protocol': 'U-Blox-UBX-Protokoll',
    'starting': 'startet',
    'unknown': 'unbekannt',
    'OK': 'OK',
    'short circuit': 'Kurzschluss',
    'open': 'offen',
    'warning': 'Warnung',
    'critical': 'kritisch',
    'antenna {antenna}, jamming {jamming}, AGC {agc} % (MON-RF)': 'Antenne {antenna}, Störungen: {jamming}, AGC {agc} % (MON-RF)',
    // ---- Diagnostics panel ----
    'Diagnostics': 'Diagnose',
    'Measured by the device itself, without a reference clock. Updated every 10 s while this panel is open.':
      'Vom Gerät selbst gemessen, ohne Referenzuhr. Wird alle 10 s aktualisiert, solange dieser Abschnitt offen ist.',
    'PTP clock': 'PTP-Uhr',
    'Measurement': 'Messung',
    'Offset': 'Versatz',
    'Innovation RMS': 'Innovation (RMS)',
    'Rate': 'Gang',
    'Filter': 'Filter',
    'PPS loopback': 'PPS-Loopback',
    'PPS interrupt': 'PPS-Interrupt',
    'Replies': 'Antworten',
    'Interleaved lookup': 'Interleaved-Zuordnung',
    'Basic mode TX delay': 'TX-Verzögerung Basic Mode',
    'Load shedding': 'Lastabwurf',
    'Data': 'Daten',
    'Stability of the GNSS PPS': 'Stabilität des GNSS-PPS',
    'not available (the PTP clock is set once per second)': 'nicht verfügbar (die PTP-Uhr wird einmal pro Sekunde gesetzt)',
    'no estimate (waiting for the PPS)': 'keine Schätzung (warte auf den PPS)',
    'PTP PPS loopback (hardware capture)': 'PTP-PPS-Loopback (Hardware-Capture)',
    'software readings (loopback pulse missing)': 'Software-Ablesungen (Loopback-Puls fehlt)',
    'software readings': 'Software-Ablesungen',
    'PTP PPS loopback': 'PTP-PPS-Loopback',
    '{offset} ns (raw PTP clock minus true time), noise {noise} ns': '{offset} ns (rohe PTP-Uhr minus wahre Zeit), Rauschen {noise} ns',
    'no complete minute yet': 'noch keine vollständige Minute',
    'no data in the last 2 minutes': 'keine Daten in den letzten 2 Minuten',
    '{minute} ns in the last minute, {hour} ns in the last hour': '{minute} ns in der letzten Minute, {hour} ns in der letzten Stunde',
    '{free} ppb free-running, command {cmd} ppb': '{free} ppb frei laufend, Stellwert {cmd} ppb',
    'outliers {o}, steps {s}, command errors {e}': 'Ausreißer {o}, Sprünge {s}, Stellfehler {e}',
    're-lock residual {r} ns': 'Rest nach Wiedereinrasten {r} ns',
    'pulses used {used}, missing {missing}, implausible {rejected}; software minus loopback {bias} ns':
      'Pulse genutzt {used}, fehlend {missing}, unplausibel {rejected}; Software minus Loopback {bias} ns',
    'correction sign {sign}, samples {n}, standard deviation {sd} ns; TIM-TP matched {m}, missing {u}, invalid {i}':
      'Korrekturvorzeichen {sign}, Stichproben {n}, σ {sd} ns; TIM-TP zugeordnet {m}, fehlend {u}, ungültig {i}',
    'latency {lat} \u00b5s; excess over the central value: {bins}; max {max} \u00b5s':
      'Latenz {lat} \u00b5s; Überschuss über dem Zentralwert: {bins}; max. {max} \u00b5s',
    'latency {lat} \u00b5s': 'Latenz {lat} \u00b5s',
    'Not enough data yet.': 'Noch nicht genug Daten.',
    '{n} s of continuous data ({source}). ADEV: overlapping Allan deviation, TDEV: time deviation. The frequency commands of the servo are taken out, outliers are replaced by the prediction.':
      '{n} s zusammenhängende Daten ({source}). ADEV: überlappende Allan-Abweichung, TDEV: Zeitabweichung. Die Frequenz-Stellwerte des Servos sind herausgerechnet, Ausreißer durch die Vorhersage ersetzt.',
    'hardware path {n}: interleaved {i}, without hardware TX time stamp {x}':
      'Hardware-Pfad {n}: interleaved {i}, ohne Hardware-TX-Zeitstempel {x}',
    'socket path {n}: interleaved {i}': 'Socket-Pfad {n}: interleaved {i}',
    'latest exchange of the address {r}, exchange map {m}, not found (basic mode) {x}':
      'letzter Austausch der Adresse {r}, Austausch-Tabelle {m}, nicht gefunden (Basic Mode) {x}',
    'IPv4 {v4} ns ({n4} values), IPv6 {v6} ns ({n6} values)': 'IPv4 {v4} ns ({n4} Werte), IPv6 {v6} ns ({n6} Werte)',
    '{n} requests dropped since the start': '{n} Anfragen seit dem Start verworfen',
    'UBX frames {f}, NAK {n}, UART overflows {o}': 'UBX-Frames {f}, NAK {n}, UART-Überläufe {o}',
    'fullyResolved: not yet': 'fullyResolved: noch nicht',
    'fullyResolved first at uptime {t}, lost since: {n}': 'fullyResolved erstmals bei Laufzeit {t}, seitdem verloren: {n}',
    'validUTC: not yet': 'validUTC: noch nicht',
    'validUTC first at uptime {t}, lost since: {n}': 'validUTC erstmals bei Laufzeit {t}, seitdem verloren: {n}',
    'sentences {n}, checksum errors {e}, UART overflows {o}': 'Sätze {n}, Prüfsummenfehler {e}, UART-Überläufe {o}',
    'RMC after the PPS: {bins}': 'RMC nach dem PPS: {bins}',
    'time pulse: configuration not read': 'Zeitpuls: Konfiguration nicht gelesen',
    'time pulse: off': 'Zeitpuls: aus',
    'ratio': 'Verhältnis',
    'time pulse: period {p}, length {l}; {unlocked}': 'Zeitpuls: Periode {p}, Länge {l}; {unlocked}',
    'no pulse without lock': 'kein Puls ohne Lock',
    'without lock: period {p}, length {l}': 'ohne Lock: Periode {p}, Länge {l}',
    'time pulse: {edge} edge, {grid} grid{aligned}, cable delay {c} ns, user delay {u} ns':
      'Zeitpuls: {edge} Flanke, {grid}-Raster{aligned}, Kabelverzögerung {c} ns, Benutzerverzögerung {u} ns',
    'rising': 'steigende',
    'falling': 'fallende',
    'not aligned to the second': 'nicht auf die Sekunde ausgerichtet',
    'RF: noise {n} per ms, AGC {agc}, CW jamming indicator {j}': 'HF: Rauschen {n} pro ms, AGC {agc}, CW-Störindikator {j}',
    'NMEA standard protocol': 'NMEA-Standardprotokoll',
    '{baud} baud': '{baud} Baud',
    '{model} mode': 'Modus {model}',
    'dynamic model not known': 'Dynamikmodell unbekannt',
    'on, {n} key of the web settings': 'an, {n} Schlüssel aus den Web-Einstellungen',
    'on, {n} keys of the web settings': 'an, {n} Schlüssel aus den Web-Einstellungen',
    'on, {n} key compiled in': 'an, {n} einkompilierter Schlüssel',
    'on, {n} keys compiled in': 'an, {n} einkompilierte Schlüssel',
    'on, but no usable key': 'an, aber kein nutzbarer Schlüssel',
    'connected': 'verbunden',
    'not connected': 'nicht verbunden',
    '{state}, reports queued on the TF card': '{state}, Berichte in der Warteschlange auf der TF-Karte',
    '{mac} (hardware MAC)': '{mac} (Hardware-MAC)',
    '{mac} (set, hardware MAC {hardware})': '{mac} (eingestellt, Hardware-MAC {hardware})',
    'via {gateway}, {source}': 'über {gateway}, {source}',
    'static': 'statisch',
    'waiting for DHCP': 'warte auf DHCP',
    'none': 'keine',

    // ---- Settings form: general ----
    'Fields marked <span class="badge restart">restart</span> take effect after a restart: <em>Apply</em> restarts the device once with them. A <span class="def-sample">coloured border</span> marks a value that differs from the compiled default.':
      'Mit <span class="badge restart">Neustart</span> markierte Felder wirken erst nach einem Neustart: <em>Übernehmen</em> startet das Gerät einmal damit neu. Ein <span class="def-sample">farbiger Rahmen</span> markiert einen Wert, der vom einkompilierten Standard abweicht.',
    'restart': 'Neustart',
    'GPIO pins': 'GPIO-Pins',
    'Default: {value}': 'Standard: {value}',
    '(empty)': '(leer)',
    'on': 'an',
    'off': 'aus',
    'no limit': 'unbegrenzt',
    '1 pin differs from the default': '1 Pin weicht vom Standard ab',
    '{n} pins differ from the default': '{n} Pins weichen vom Standard ab',
    'see the warning': 'siehe Warnung',
    'unchanged': 'unverändert',
    'A password is set.': 'Ein Passwort ist gesetzt.',
    'No password is set.': 'Kein Passwort gesetzt.',
    'Hide the text': 'Text verbergen',
    'Show the text': 'Text anzeigen',
    'Show or hide the text': 'Text anzeigen oder verbergen',
    '{label}: GPIO number expected': '{label}: GPIO-Nummer erwartet',

    // ---- Network ----
    'Host name and domain (FQDN) <span class="badge restart">restart</span>': 'Hostname und Domain (FQDN) <span class="badge restart">Neustart</span>',
    'E.g. ESP32TimeServer.example.com. Without a domain, the domain of the DHCP server applies if DHCP is used. The first part is the host name (DHCP), the full name is used for the certificate.':
      'Z. B. ESP32TimeServer.example.com. Ohne Domain gilt bei DHCP die Domain des DHCP-Servers. Der erste Teil ist der Hostname (DHCP), der vollständige Name wird für das Zertifikat verwendet.',
    'IPv4 address by DHCP': 'IPv4-Adresse per DHCP',
    'IPv4 address / prefix length': 'IPv4-Adresse / Prefix-Länge',
    'IPv4 gateway': 'IPv4-Gateway',
    'IPv6 addresses by SLAAC (router advertisements)': 'IPv6-Adressen per SLAAC (Router Advertisements)',
    'Additional static IPv6 address / prefix length': 'Zusätzliche statische IPv6-Adresse / Prefix-Länge',
    '2001:db8::10/64 (empty = none)': '2001:db8::10/64 (leer = keine)',
    'IPv6 gateway': 'IPv6-Gateway',
    'fe80::1 (empty = from router advertisements)': 'fe80::1 (leer = aus Router Advertisements)',
    'DNS servers (IPv4 or IPv6, one per line, at most 3)': 'DNS-Server (IPv4 oder IPv6, einer pro Zeile, höchstens 3)',
    'Empty = the servers received by DHCP (IPv4) and router advertisements (IPv6)': 'Leer = die per DHCP (IPv4) und Router Advertisements (IPv6) empfangenen Server',
    'Empty: the servers received by DHCP (IPv4) and by IPv6 router advertisements (RDNSS) are used, both families mixed. Configured servers replace them.':
      'Leer: Die per DHCP (IPv4) und per IPv6 Router Advertisements (RDNSS) empfangenen Server werden verwendet, beide Adressfamilien gemischt. Eingetragene Server ersetzen sie.',
    '(empty: received servers)': '(leer: empfangene Server)',
    'MAC address <span class="badge restart">restart</span>': 'MAC-Adresse <span class="badge restart">Neustart</span>',
    'Hardware MAC of this board: <span id="mac-hardware" class="mono"></span>':
      'Hardware-MAC dieses Boards: <span id="mac-hardware" class="mono"></span>',
    'Allowed Formats (any case): <code>02:1a:2b:3c:4d:5e</code>, <code>02-1A-2B-3C-4D-5E</code>, <code>021a.2b3c.4d5e</code>, <code>021A2B3C4D5E</code>':
      'Erlaubte Formate (Groß- oder Kleinschreibung): <code>02:1a:2b:3c:4d:5e</code>, <code>02-1A-2B-3C-4D-5E</code>, <code>021a.2b3c.4d5e</code>, <code>021A2B3C4D5E</code>',
    'MAC address changes: The lowest bit of the first byte must be 0; locally administered addresses have the second bit set, e.g. 02:…':
      'Änderungen der MAC-Adresse: Das niedrigste Bit des ersten Bytes muss 0 sein; lokal verwaltete Adressen haben das zweite Bit gesetzt, z. B. 02:…',
    'the hardware MAC': 'die Hardware-MAC',
    'Network changes take effect at once (the MAC address after a restart) and have to be confirmed within 120 s, otherwise the previous settings come back. Save them after the confirmation.':
      'Netzwerkänderungen wirken sofort (die MAC-Adresse nach einem Neustart) und müssen innerhalb von 120 s bestätigt werden, sonst werden die vorherigen Einstellungen wiederhergestellt. Speichern Sie sie nach der Bestätigung.',
    'NTP clients': 'NTP-Clients',
    'Rate limit per client (requests per second, 0 = off)': 'Rate-Limit pro Client (Anfragen pro Sekunde, 0 = aus)',
    '0 to 10000. A client is identified by its IP address; its requests beyond this rate are dropped without an answer. Takes effect at once with Apply (no confirmation needed). Default covers a polling rate of up to -4 (16 packets/s) per client plus reserve. Raise it only if a very large number of clients is expected behind a single NAT address: they all count as one client.':
      '0 bis 10000. Ein Client wird an seiner IP-Adresse erkannt; seine Anfragen über dieser Rate werden ohne Antwort verworfen. Wirkt sofort mit Übernehmen (keine Bestätigung nötig). Der Standardwert deckt pro Client eine Abfragerate bis -4 (16 Pakete/s) plus Reserve ab. Erhöhen Sie ihn nur, wenn sehr viele Clients hinter einer einzigen NAT-Adresse erwartet werden: Sie zählen alle als ein Client.',
    '{n} per second': '{n} pro Sekunde',
    'Rate limit per client: 0 to 10000 requests per second': 'Rate-Limit pro Client: 0 bis 10000 Anfragen pro Sekunde',

    // ---- GNSS and PPS ----
    'GNSS and PPS': 'GNSS und PPS',
    'Protocol <span class="badge restart">restart</span>': 'Protokoll <span class="badge restart">Neustart</span>',
    'NMEA Standard Protocol': 'NMEA-Standardprotokoll',
    'U-Blox UBX Protocol': 'U-Blox-UBX-Protokoll',
    'NMEA Standard Protocol (needs RMC sentence)': 'NMEA-Standardprotokoll (benötigt RMC-Sentence)',
    'GNSS baud rate <span class="badge restart">restart</span>': 'GNSS-Baudrate <span class="badge restart">Neustart</span>',
    'With the U-Blox UBX protocol the baud rate is found and configured automatically.': 'Mit dem U-Blox-UBX-Protokoll wird die Baudrate automatisch erkannt und eingestellt.',
    'Validity of the GNSS time': 'Gültigkeit der GNSS-Zeit',
    'Minimum satellites in use before stratum 1 (0 = off)': 'Mindestzahl verwendeter Satelliten vor Stratum 1 (0 = aus)',
    '0 to 50. The GNSS time only counts while the receiver\'s position solution uses at least this many satellites; with fewer the server answers at stratum 16 after a few seconds (a 3D fix needs 4; a higher value such as 6 excludes weak fixes). NMEA protocol: needs GGA sentences in addition to RMC, the receiver has to be set to send them; while this is on and no GGA arrives, the server stays at stratum 16. U-Blox UBX protocol: the satellites of the navigation solution. Takes effect at once with Apply.':
      '0 bis 50. Die GNSS-Zeit zählt nur, solange die Positionslösung des Empfängers mindestens so viele Satelliten verwendet; mit weniger antwortet der Server nach wenigen Sekunden mit Stratum 16 (ein 3D-Fix braucht 4; ein höherer Wert wie 6 schließt schwache Fixes aus). NMEA-Protokoll: braucht GGA-Sätze zusätzlich zu RMC, der Empfänger muss sie senden; solange dies eingeschaltet ist und kein GGA ankommt, bleibt der Server bei Stratum 16. U-Blox-UBX-Protokoll: die Satelliten der Navigationslösung. Wirkt sofort mit Übernehmen.',
    'Minimum GNSS fix age before stratum 1 (s, 0 = off)': 'Mindestalter des GNSS-Fix vor Stratum 1 (s, 0 = aus)',
    '0 to 3600 s. After a cold start (no backup battery, or switched off for a long time) a GNSS receiver may report UTC off by whole seconds until it has received the leap seconds, which the satellites broadcast every 12.5 minutes. With a value the server answers at stratum 16 after each start until the GNSS time has been valid for this long without a gap of more than 30 s; then the time is checked against the GNSS once more (a wrong second is corrected) and stratum 1 is allowed until the next start. Recommended: 750 s (12.5 min) for a receiver without backup battery. Takes effect at once with Apply while the server waits; once stratum 1 was reached, a change applies from the next start. A u-blox receiver also reports when its UTC time is fully resolved, which is checked in any case.':
      '0 bis 3600 s. Nach einem Kaltstart (keine Pufferbatterie oder lange ausgeschaltet) kann ein GNSS-Empfänger UTC um ganze Sekunden falsch melden, bis er die Schaltsekunden empfangen hat, die die Satelliten alle 12,5 Minuten aussenden. Mit einem Wert antwortet der Server nach jedem Start mit Stratum 16, bis die GNSS-Zeit so lange ohne eine Lücke von mehr als 30 s gültig war; dann wird die Zeit noch einmal mit dem GNSS verglichen (eine falsche Sekunde wird korrigiert), und Stratum 1 ist bis zum nächsten Start erlaubt. Empfohlen: 750 s (12,5 min) für einen Empfänger ohne Pufferbatterie. Wirkt sofort mit Übernehmen, solange der Server wartet; nachdem Stratum 1 erreicht wurde, gilt eine Änderung ab dem nächsten Start. Ein u-blox-Empfänger meldet zusätzlich, wann seine UTC-Zeit vollständig aufgelöst ist; das wird in jedem Fall geprüft.',
    'When the PPS is lost': 'Wenn der PPS ausfällt',
    'The PPS counts as lost after 3.5 s without a pulse. The settings below take effect at once with Apply. When the PPS and the GNSS are back, the server returns to stratum 1 by itself.':
      'Der PPS gilt nach 3,5 s ohne Impuls als ausgefallen. Die folgenden Einstellungen wirken sofort mit Übernehmen. Sobald PPS und GNSS wieder da sind, kehrt der Server von selbst zu Stratum 1 zurück.',
    'PPS holdover (s, 0 = off)': 'PPS-Holdover (s, 0 = aus)',
    '0 to 600 s. For this long after the PPS is lost the server keeps answering at stratum 1, coasting on the learned rate of its oscillator; the advertised root dispersion grows with the time since the last pulse (see below), so the clients see the growing uncertainty. After the holdover the server answers as set below.':
      '0 bis 600 s. So lange nach dem PPS-Ausfall antwortet der Server weiter mit Stratum 1 und läuft mit der gelernten Rate seines Oszillators weiter; die angegebene Root Dispersion wächst mit der Zeit seit dem letzten Impuls (siehe unten), sodass die Clients die wachsende Unsicherheit sehen. Nach dem Holdover antwortet der Server wie unten eingestellt.',
    'Root dispersion growth (ms per second)': 'Zunahme der Root Dispersion (ms pro Sekunde)',
    '0.001 to 10 ms per second since the last pulse (0.1 ms per second = 100 ppm), up to at most 1 s. The advertised uncertainty grows this fast while the server coasts after the PPS loss: in the holdover and with a stratum of 2 to 15 below.':
      '0,001 bis 10 ms pro Sekunde seit dem letzten Impuls (0,1 ms pro Sekunde = 100 ppm), höchstens bis 1 s. So schnell wächst die angegebene Unsicherheit, während der Server nach dem PPS-Ausfall frei weiterläuft: im Holdover und mit einem Stratum von 2 bis 15 unten.',
    'Root dispersion growth: 0.001 to 10 ms per second': 'Zunahme der Root Dispersion: 0,001 bis 10 ms pro Sekunde',
    'Keep answering NTP requests after the holdover': 'NTP-Anfragen nach dem Holdover weiter beantworten',
    'Off: after the holdover the server does not answer at all, so the clients switch to their other servers at once instead of getting stratum 16 answers.':
      'Aus: Nach dem Holdover antwortet der Server gar nicht mehr, sodass die Clients sofort zu ihren anderen Servern wechseln, statt Antworten mit Stratum 16 zu erhalten.',
    'Keep answering for (min, 0 = no limit)': 'Weiter antworten für (min, 0 = unbegrenzt)',
    '0 to 1440 min, counted from the PPS loss (the holdover included). Then the server stops answering.':
      '0 bis 1440 min, gezählt ab dem PPS-Ausfall (einschließlich Holdover). Danach antwortet der Server nicht mehr.',
    'Stratum of these answers': 'Stratum dieser Antworten',
    '16 = "not synchronised" (leap indicator alarm): the clients stop using the server. 2 to 15: the answers stay "synchronised" at that stratum, coasting with a growing root dispersion for at most one hour after the last pulse, then 16.':
      '16 = „nicht synchronisiert“ (Leap Indicator Alarm): Die Clients verwenden den Server nicht mehr. 2 bis 15: Die Antworten bleiben mit diesem Stratum „synchronisiert“ und laufen mit wachsender Root Dispersion höchstens eine Stunde nach dem letzten Impuls weiter, danach 16.',
    'Relying on time that is no longer synchronised to the GNSS is probably not a good idea: its error grows by an amount nobody measures. Use a stratum below 16 and a long answering time only when the clients absolutely need a time source and have no other one.':
      'Sich auf eine Zeit zu verlassen, die nicht mehr mit dem GNSS synchronisiert ist, ist vermutlich keine gute Idee: Ihr Fehler wächst um einen Betrag, den niemand misst. Verwenden Sie ein Stratum unter 16 und eine lange Antwortzeit nur, wenn die Clients unbedingt eine Zeitquelle brauchen und keine andere haben.',
    'GNSS TX, RX, PPS, PTP PPS loopback': 'GNSS TX, RX, PPS, PTP PPS Loopback',
    'GNSS TX pin (GPIO)': 'GNSS-TX-Pin (GPIO)',
    'GNSS RX pin (GPIO)': 'GNSS-RX-Pin (GPIO)',
    'PPS pin (GPIO)': 'PPS-Pin (GPIO)',
    'PTP PPS loopback pin (GPIO, empty = off)': 'PTP-PPS-Loopback-Pin (GPIO, leer = aus)',
    'The pin has to be empty: a free GPIO with nothing connected; it is driven as an output. Needs an ESP32-P4 with chip revision v3.0 or higher (this board: <span id="ptp-chip"></span>).':
      'Der Pin muss frei sein: ein unbenutzter GPIO, an dem nichts angeschlossen ist; er wird als Ausgang betrieben. Benötigt einen ESP32-P4 mit Chip-Revision v3.0 oder höher (dieses Board: <span id="ptp-chip"></span>).',
    'This board\'s chip revision is below v3.0: the loopback does not work here, leave the field empty.':
      'Die Chip-Revision dieses Boards ist unter v3.0: Der Loopback funktioniert hier nicht, lassen Sie das Feld leer.',
    'GNSS TX pin': 'GNSS-TX-Pin',
    'GNSS RX pin': 'GNSS-RX-Pin',
    'PPS pin': 'PPS-Pin',
    'PTP PPS loopback pin': 'PTP-PPS-Loopback-Pin',
    'Minimum GNSS fix age: 0 to 3600 s': 'Mindestalter des GNSS-Fix: 0 bis 3600 s',
    'Minimum satellites in use: 0 to 50': 'Mindestzahl verwendeter Satelliten: 0 bis 50',
    'PPS holdover: 0 to 600 s': 'PPS-Holdover: 0 bis 600 s',
    'Keep answering after the PPS loss: 0 to 1440 min': 'Weiter antworten nach dem PPS-Ausfall: 0 bis 1440 min',
    'Stratum after the holdover: 2 to 16': 'Stratum nach dem Holdover: 2 bis 16',

    // ---- LCD and time zone ----
    'LCD and time zone': 'LCD und Zeitzone',
    'Enable LCD Display': 'LCD-Display aktivieren',
    '20 × 4 character LCD (HD44780) with a PCF8574 I2C backpack: SDA on GPIO 8, SCL on GPIO 7, I2C address 0x27 or 0x3F (found automatically).':
      '20 × 4 Zeichen LCD (HD44780) mit PCF8574-I2C-Backpack: SDA an GPIO 8, SCL an GPIO 7, I2C-Adresse 0x27 oder 0x3F (wird automatisch erkannt).',
    'Top line title (at most 17 characters)': 'Titel der obersten Zeile (höchstens 17 Zeichen)',
    'Time zone (POSIX TZ string)': 'Zeitzone (POSIX-TZ-String)',
    '24-hour clock': '24-Stunden-Anzeige',
    'Hour with leading zero': 'Stunde mit führender Null',
    'Show the time zone': 'Zeitzone anzeigen',

    // ---- RGB LED ----
    'Module <span class="badge restart">restart</span>': 'Modul <span class="badge restart">Neustart</span>',
    'None': 'Keins',
    'KY-016 (3 pins, PWM)': 'KY-016 (3 Pins, PWM)',
    'WS2812 (1 data pin)': 'WS2812 (1 Daten-Pin)',
    'Brightness <output id="brightness-out"></output>': 'Helligkeit <output id="brightness-out"></output>',
    'Colours': 'Farben',
    'A preset or any colour as <code>#RRGGBB</code>. Takes effect at once.': 'Eine Vorgabe oder eine beliebige Farbe als <code>#RRGGBB</code>. Wirkt sofort.',
    'KY-016 red, green, blue / WS2812 data': 'KY-016 Rot, Grün, Blau / WS2812 Daten',
    'Red pin (GPIO)': 'Pin Rot (GPIO)',
    'Green pin (GPIO)': 'Pin Grün (GPIO)',
    'Blue pin (GPIO)': 'Pin Blau (GPIO)',
    'Data pin (GPIO)': 'Daten-Pin (GPIO)',
    'No LED module is selected: no pins are used.': 'Kein LED-Modul ausgewählt: Es werden keine Pins verwendet.',
    'Start-up': 'Startphase',
    'until the server is ready': 'bis der Server bereit ist',
    'Normal': 'Normal',
    'operating normally': 'normaler Betrieb',
    'Time sync': 'Zeitsynchronisation',
    'the time is being set': 'die Zeit wird gesetzt',
    'Warning': 'Warnung',
    'GNSS / PPS problem, queued MQTT reports': 'GNSS-/PPS-Problem, MQTT-Berichte in der Warteschlange',
    'Alert': 'Alarm',
    'no IP address, load shedding': 'keine IP-Adresse, Load Shedding',
    'Critical': 'Kritisch',
    'Ethernet, GNSS link or MQTT failure': 'Ausfall von Ethernet, GNSS-Verbindung oder MQTT',
    'Button pressed': 'Taste gedrückt',
    'uptime button': 'Laufzeit-Taste',
    'Off': 'Aus',
    'Red': 'Rot',
    'Green': 'Grün',
    'Blue': 'Blau',
    'Yellow': 'Gelb',
    'White': 'Weiß',
    'Orange': 'Orange',
    'Pink': 'Rosa',
    'Custom': 'Eigene',
    '{name} colour': 'Farbe {name}',
    '{name} colour as #RRGGBB': 'Farbe {name} als #RRGGBB',
    'LED red pin': 'LED-Pin Rot',
    'LED green pin': 'LED-Pin Grün',
    'LED blue pin': 'LED-Pin Blau',
    'WS2812 data pin': 'WS2812-Daten-Pin',
    'LED colour "{name}": #RRGGBB expected': 'LED-Farbe „{name}“: #RRGGBB erwartet',

    // ---- MQTT ----
    'MQTT <span class="badge restart">restart</span>': 'MQTT <span class="badge restart">Neustart</span>',
    'MQTT reporting': 'MQTT-Berichte',
    'mqtt://192.168.1.15:1883 or mqtts://broker.example:8883': 'mqtt://192.168.1.15:1883 oder mqtts://broker.example:8883',
    'User name': 'Benutzername',
    'Clear the MQTT password': 'MQTT-Passwort löschen',
    'An empty password field keeps the stored password. Secrets are never sent back to the browser.':
      'Ein leeres Passwortfeld behält das gespeicherte Passwort. Geheimnisse werden nie an den Browser zurückgesendet.',
    'Reports are published to <topic>/report, online / offline to <topic>/status.':
      'Berichte werden an <topic>/report veröffentlicht, online / offline an <topic>/status.',
    'Quality of service (QoS)': 'Quality of Service (QoS)',
    '0: at most once, nothing is queued': '0: höchstens einmal, nichts wird zwischengespeichert',
    '1: at least once, queued while disconnected': '1: mindestens einmal, zwischengespeichert ohne Verbindung',
    '2: exactly once, queued while disconnected': '2: genau einmal, zwischengespeichert ohne Verbindung',
    '0 (at most once)': '0 (höchstens einmal)',
    '1 (at least once)': '1 (mindestens einmal)',
    '2 (exactly once)': '2 (genau einmal)',
    'Reporting period (s)': 'Berichtsintervall (s)',
    '30 to 86400 s, e.g. 900 = 15 min.': '30 bis 86400 s, z. B. 900 = 15 min.',
    'Broker retains the latest report (retain flag)': 'Broker behält den letzten Bericht (Retain-Flag)',
    'Report contents': 'Inhalt der Berichte',
    'Every report has the status, uptime, satellites, the time quality of the PTP clock and NTP request counts. Additionally:':
      'Jeder Bericht enthält Status, Laufzeit, Satelliten, die Zeitqualität der PTP-Uhr und die Anzahl der NTP-Anfragen. Zusätzlich:',
    'Client activity: requests per client address': 'Client-Aktivität: Anfragen pro Client-Adresse',
    'Memory usage: free heap, minimum free heap, largest free block': 'Speicherbelegung: freier Heap, minimaler freier Heap, größter freier Block',
    'Historical information: last time synchronised and disciplined, last GNSS and PPS faults':
      'Verlauf: zuletzt synchronisiert und diszipliniert, letzte GNSS- und PPS-Fehler',
    'Separate counts for authenticated and unauthenticated NTP requests': 'Getrennte Zählung authentifizierter und nicht authentifizierter NTP-Anfragen',
    'Report queue on a TF card (QoS 1 and 2)': 'Berichts-Warteschlange auf einer TF-Karte (QoS 1 und 2)',
    'Without a usable TF card (FAT32) only the last 4 reports are queued, in RAM, with at most 50 clients each: they are lost at a power failure or crash, and the oldest report is dropped first. Flash is not used, so it does not wear out. With a TF card the reports are queued on the card (an empty 16 GB card can hold more than 36,000 reports) with up to the client limit below; when the card is full, the oldest reports are dropped first.':
      'Ohne nutzbare TF-Karte (FAT32) werden nur die letzten 4 Berichte im RAM zwischengespeichert, mit jeweils höchstens 50 Clients: Sie gehen bei Stromausfall oder Absturz verloren, und der älteste Bericht wird zuerst verworfen. Der Flash wird nicht verwendet und nutzt sich daher nicht ab. Mit TF-Karte werden die Berichte auf der Karte zwischengespeichert (eine leere 16-GB-Karte fasst mehr als 36.000 Berichte), mit bis zu dem Client-Limit unten; ist die Karte voll, werden die ältesten Berichte zuerst verworfen.',
    'Client limit per report with the TF card': 'Client-Limit pro Bericht mit TF-Karte',
    'Report buffer: {kb} kB of RAM (1426 bytes plus 76 bytes per client). Without the TF card at most 50 clients are listed.':
      'Berichtspuffer: {kb} kB RAM (1426 Bytes plus 76 Bytes pro Client). Ohne TF-Karte werden höchstens 50 Clients aufgeführt.',
    '50 to 1000.': '50 bis 1000.',
    'Only used with client activity reporting.': 'Nur mit Berichten zur Client-Aktivität verwendet.',
    'TF card CMD, CLK, D0 to D3': 'TF-Karte CMD, CLK, D0 bis D3',
    'Waveshare ESP32-P4-ETH, ESP32-P4-POE-ETH, ESP32-P4-WIFI-ETH and ESP32-P4-WIFI-POE-ETH: the TF card slot is wired to these pins. If you use one of these boards, do not change them.':
      'Waveshare ESP32-P4-ETH, ESP32-P4-POE-ETH, ESP32-P4-WIFI-ETH und ESP32-P4-WIFI-POE-ETH: Der TF-Kartenslot ist mit diesen Pins verbunden. Wenn Sie eines dieser Boards verwenden, ändern Sie sie nicht.',
    '4-bit SD mode. The pins are always reserved for the card, also while MQTT or QoS 1 / 2 are off.':
      '4-Bit-SD-Modus. Die Pins sind immer für die Karte reserviert, auch wenn MQTT oder QoS 1 / 2 aus sind.',
    'TF card CMD pin': 'TF-Karte CMD-Pin',
    'TF card CLK pin': 'TF-Karte CLK-Pin',
    'TF card D0 pin': 'TF-Karte D0-Pin',
    'TF card D1 pin': 'TF-Karte D1-Pin',
    'TF card D2 pin': 'TF-Karte D2-Pin',
    'TF card D3 pin': 'TF-Karte D3-Pin',
    'MQTT reporting period: 30 to 86400 s': 'MQTT-Berichtsintervall: 30 bis 86400 s',
    'MQTT client limit with the TF card: 50 to 1000': 'MQTT-Client-Limit mit TF-Karte: 50 bis 1000',

    // ---- NTP authentication ----
    'NTP authentication (symmetric key, SHA-256)': 'NTP-Authentifizierung (symmetrischer Schlüssel, SHA-256)',
    'Answer authenticated NTPv4 requests': 'Authentifizierte NTPv4-Anfragen beantworten',
    'Keys': 'Schlüssel',
    'Key ID': 'Key-ID',
    'Key': 'Schlüssel',
    'Key (32 letters or digits)': 'Schlüssel (32 Buchstaben oder Ziffern)',
    'Add key': 'Schlüssel hinzufügen',
    'Generate': 'Erzeugen',
    'New random key (32 letters or digits)': 'Neuer Zufallsschlüssel (32 Buchstaben oder Ziffern)',
    'Remove': 'Entfernen',
    'Use the keys compiled into the firmware (ESP32TimeServerSettingsSecrets.h)': 'Die in die Firmware einkompilierten Schlüssel verwenden (ESP32TimeServerSettingsSecrets.h)',
    'Up to 8 keys (SHA-256, truncated to 160 bit). Enter the same key ID and key on the clients, e.g. <code>1 SHA256 ASCII:&lt;key&gt;</code> in chrony.keys. Generate uses the random generator of the browser. Changed keys take effect at once with Apply; Save Settings keeps them.':
      'Bis zu 8 Schlüssel (SHA-256, auf 160 Bit gekürzt). Tragen Sie dieselbe Key-ID und denselben Schlüssel auf den Clients ein, z. B. <code>1 SHA256 ASCII:&lt;key&gt;</code> in chrony.keys. Erzeugen verwendet den Zufallsgenerator des Browsers. Geänderte Schlüssel wirken sofort mit Übernehmen; Einstellungen speichern behält sie.',
    'No keys are set on this page: these are the keys compiled into the firmware. Changing them here stores them as keys of the web settings.':
      'Auf dieser Seite sind keine Schlüssel gesetzt: Dies sind die in die Firmware einkompilierten Schlüssel. Wenn Sie sie hier ändern, werden sie als Schlüssel der Web-Einstellungen gespeichert.',
    'Keys set on this page are in use.': 'Die auf dieser Seite gesetzten Schlüssel sind aktiv.',
    'NTP key {n}: key ID 1 to 65535': 'NTP-Schlüssel {n}: Key-ID 1 bis 65535',
    'NTP key {n}: exactly 32 letters or digits': 'NTP-Schlüssel {n}: genau 32 Buchstaben oder Ziffern',

    // ---- Buttons and their explanation ----
    'In effect now, not stored': 'Sofort wirksam, nicht gespeichert',
    'Apply': 'Übernehmen',
    'Stored in NVS (flash), loaded at every boot': 'In NVS (Flash) gespeichert, bei jedem Start geladen',
    'Save Settings': 'Einstellungen speichern',
    'Restart the device': 'Gerät neu starten',
    'Reboot': 'Neu starten',
    'Back to the compiled settings': 'Zurück zu den einkompilierten Einstellungen',
    'Factory reset': 'Werkseinstellungen',
    '<strong>Apply:</strong> in effect now, not stored. <strong>Save Settings:</strong> the settings are stored in NVS (flash) and loaded at every boot. <strong>Reboot:</strong> restarts with the saved settings. <strong>Factory reset:</strong> back to the compiled settings and the compiled password (the TLS certificate is kept).':
      '<strong>Übernehmen:</strong> sofort wirksam, nicht gespeichert. <strong>Einstellungen speichern:</strong> Die Einstellungen werden in NVS (Flash) gespeichert und bei jedem Start geladen. <strong>Neu starten:</strong> startet mit den gespeicherten Einstellungen neu. <strong>Werkseinstellungen:</strong> zurück zu den einkompilierten Einstellungen und dem einkompilierten Passwort (das TLS-Zertifikat bleibt erhalten).',
    'This erases the saved settings, the web admin password and the allow-list, then restarts the device.':
      'Dies löscht die gespeicherten Einstellungen, das Web-Admin-Passwort und die Allow-List und startet das Gerät dann neu.',
    'Current password': 'Aktuelles Passwort',
    'Erase and restart': 'Löschen und neu starten',
    'Cancel': 'Abbrechen',

    // ---- Firmware update ----
    'Firmware Update': 'Firmware-Update',
    'A new firmware can be installed over the network (Over The Ethernet, OTE). First the device has to accept updates:':
      'Eine neue Firmware kann über das Netzwerk installiert werden (Over The Ethernet, OTE). Zuerst muss das Gerät Updates annehmen:',
    '<strong>Once:</strong> press <em>Restart with updates enabled</em> under <em>Upload</em>. The device restarts and accepts updates until its next restart (the one after an upload included); after that they are off again. Log in again after about a minute.':
      '<strong>Einmalig:</strong> Drücken Sie <em>Neustart mit aktivierten Updates</em> unter <em>Upload</em>. Das Gerät startet neu und nimmt Updates bis zu seinem nächsten Neustart an (auch dem nach einem Upload); danach sind sie wieder aus. Melden Sie sich nach etwa einer Minute erneut an.',
    '<strong>Permanently:</strong> tick <em>Accept firmware updates over the network</em>, press <em>Save Settings</em> and then <em>Reboot</em>.':
      '<strong>Dauerhaft:</strong> Haken bei <em>Firmware-Updates über das Netzwerk annehmen</em> setzen, <em>Einstellungen speichern</em> und dann <em>Neu starten</em> drücken.',
    'Then the image can be sent over HTTPS (TCP 443) in two ways:': 'Dann kann das Image auf zwei Wegen über HTTPS (TCP 443) gesendet werden:',
    '<strong>With this page:</strong> choose the image under <em>Upload</em> and press <em>Upload and restart</em>. Your login to this page authorises the upload.':
      '<strong>Mit dieser Seite:</strong> Wählen Sie das Image unter <em>Upload</em> und drücken Sie <em>Hochladen und neu starten</em>. Ihre Anmeldung an dieser Seite berechtigt den Upload.',
    '<strong>With the script on a PC:</strong> <code>python tools/ote_upload.py -i &lt;FQDN or IP address&gt; -f build/ESP32TimeServer.bin --fingerprint &lt;certificate SHA-256&gt;</code> (the fingerprint is shown under <em>Status</em>). The script looks the name up in the DNS (A and AAAA records, no mDNS) and authorises the upload with the web admin password, which it takes from the environment variable <code>WEB_ADMIN_PASSWORD</code> or asks for; no login is needed. While the factory default password is in use, the device refuses the script.':
      '<strong>Mit dem Skript auf einem PC:</strong> <code>python tools/ote_upload.py -i &lt;FQDN or IP address&gt; -f build/ESP32TimeServer.bin --fingerprint &lt;certificate SHA-256&gt;</code> (der Fingerprint steht unter <em>Status</em>). Das Skript löst den Namen über DNS auf (A- und AAAA-Records, kein mDNS) und berechtigt den Upload mit dem Web-Admin-Passwort, das es aus der Umgebungsvariable <code>WEB_ADMIN_PASSWORD</code> nimmt oder abfragt; eine Anmeldung ist nicht nötig. Solange das Werkspasswort verwendet wird, lehnt das Gerät das Skript ab.',
    '<strong>Which file:</strong> <code>ESP32TimeServer.bin</code> from the build folder, built with the profile for this board\'s chip revision (<span id="ote-chip"></span>), not the merged flash image. The device refuses images of another project or chip revision.':
      '<strong>Welche Datei:</strong> <code>ESP32TimeServer.bin</code> aus dem Build-Ordner, gebaut mit dem Profil für die Chip-Revision dieses Boards (<span id="ote-chip"></span>), nicht das zusammengeführte Flash-Image. Das Gerät lehnt Images eines anderen Projekts oder einer anderen Chip-Revision ab.',
    '<strong>What happens:</strong> the device writes the image into its spare firmware slot, checks the complete image and only then selects it for the next start; until then the running firmware stays untouched. It restarts 5 s after the upload: reload this page after about a minute. During the upload (at most 5 min) this page pauses its other requests.':
      '<strong>Was passiert:</strong> Das Gerät schreibt das Image in seinen freien Firmware-Slot, prüft das vollständige Image und wählt es erst dann für den nächsten Start aus; bis dahin bleibt die laufende Firmware unberührt. Es startet 5 s nach dem Upload neu: Laden Sie diese Seite nach etwa einer Minute neu. Während des Uploads (höchstens 5 min) pausiert diese Seite ihre anderen Anfragen.',
    '<strong>Setting:</strong> <em>Accept firmware updates over the network</em> takes effect with <em>Apply</em>, which restarts the device once, and is kept with <em>Save Settings</em>.':
      '<strong>Einstellung:</strong> <em>Firmware-Updates über das Netzwerk annehmen</em> wirkt mit <em>Übernehmen</em>, das das Gerät einmal neu startet, und bleibt mit <em>Einstellungen speichern</em> erhalten.',
    '<strong>Security:</strong> the image is not signed, so anyone who knows the web admin password can install any firmware, with this page or with the script. Use a long random password and switch updates off when you do not need them. Wrong passwords of the script count towards the login lock-out, and the allow-list under <em>Security</em> applies to both ways.':
      '<strong>Sicherheit:</strong> Das Image ist nicht signiert, daher kann jeder, der das Web-Admin-Passwort kennt, eine beliebige Firmware installieren, mit dieser Seite oder mit dem Skript. Verwenden Sie ein langes zufälliges Passwort und schalten Sie Updates aus, wenn Sie sie nicht brauchen. Falsche Passwörter des Skripts zählen für die Anmeldesperre, und die Allow-List unter <em>Sicherheit</em> gilt für beide Wege.',
    'Settings <span class="badge restart">restart</span>': 'Einstellungen <span class="badge restart">Neustart</span>',
    'Accept firmware updates over the network': 'Firmware-Updates über das Netzwerk annehmen',
    'Upload': 'Upload',
    'Firmware image (.bin)': 'Firmware-Image (.bin)',
    'Updates are off on the device: uploads are not possible.': 'Updates sind auf dem Gerät aus: Uploads sind nicht möglich.',
    'Restart with updates enabled': 'Neustart mit aktivierten Updates',
    'Only until the next restart: the restart after the upload switches updates off again.':
      'Nur bis zum nächsten Neustart: Der Neustart nach dem Upload schaltet Updates wieder aus.',
    'Updates are on until the next restart (enabled once).': 'Updates sind bis zum nächsten Neustart an (einmalig aktiviert).',
    'Upload and restart': 'Hochladen und neu starten',
    'Restart the device now with firmware updates enabled until its next restart?':
      'Das Gerät jetzt neu starten, mit aktivierten Firmware-Updates bis zu seinem nächsten Neustart?',
    'Choose the firmware image (.bin) first.': 'Wählen Sie zuerst das Firmware-Image (.bin).',
    'This is not an application image: use ESP32TimeServer.bin of the build folder (not the merged flash image).':
      'Dies ist kein Application-Image: Verwenden Sie ESP32TimeServer.bin aus dem Build-Ordner (nicht das zusammengeführte Flash-Image).',
    'Upload {project} {version} ({file}, {kb} kB) and restart the device with it? Running: {running}.':
      '{project} {version} ({file}, {kb} kB) hochladen und das Gerät damit neu starten? Aktuell: {running}.',
    'Uploading …': 'Upload läuft …',
    'Uploading … {p} %': 'Upload läuft … {p} %',
    'Checking and writing the image …': 'Image wird geprüft und geschrieben …',
    'Firmware accepted.': 'Firmware angenommen.',
    'Upload failed ({status})': 'Upload fehlgeschlagen ({status})',
    'The upload was interrupted. The device may have refused the image (the reason is on the LCD and the serial console), or it does not answer.':
      'Der Upload wurde unterbrochen. Das Gerät hat das Image vielleicht abgelehnt (der Grund steht auf dem LCD und der seriellen Konsole), oder es antwortet nicht.',

    // ---- Security ----
    'Security': 'Sicherheit',
    'Change the password': 'Passwort ändern',
    'New password (12 to 128 characters)': 'Neues Passwort (12 bis 128 Zeichen)',
    'Repeat the new password': 'Neues Passwort wiederholen',
    'Change password': 'Passwort ändern',
    'Takes effect and is stored at once. Other sessions are logged out.': 'Wirkt sofort und wird sofort gespeichert. Andere Sitzungen werden abgemeldet.',
    'Allowed source networks': 'Erlaubte Quellnetze',
    'One address or CIDR network per line, at most 8 (empty = all)': 'Eine Adresse oder ein CIDR-Netz pro Zeile, höchstens 8 (leer = alle)',
    'Store allow-list': 'Allow-List speichern',
    'Takes effect and is stored at once. Connections from other addresses are closed before the TLS handshake. A list that does not contain your own address is refused.':
      'Wirkt sofort und wird sofort gespeichert. Verbindungen von anderen Adressen werden vor dem TLS-Handshake geschlossen. Eine Liste ohne Ihre eigene Adresse wird abgelehnt.',

    // ---- Messages of the page ----
    'The device does not answer (restarting or a different address?)': 'Das Gerät antwortet nicht (Neustart oder andere Adresse?)',
    'IPv4 address: a.b.c.d/prefix length expected': 'IPv4-Adresse: a.b.c.d/Prefix-Länge erwartet',
    'IPv6 address: address/prefix length expected': 'IPv6-Adresse: Adresse/Prefix-Länge erwartet',
    'MAC address: xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx or xxxxxxxxxxxx (hexadecimal) expected':
      'MAC-Adresse: xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx oder xxxxxxxxxxxx (hexadezimal) erwartet',
    'MAC address: a unicast address is required (the lowest bit of the first byte must be 0)':
      'MAC-Adresse: Eine Unicast-Adresse ist erforderlich (das niedrigste Bit des ersten Bytes muss 0 sein)',
    'At most 3 DNS servers': 'Höchstens 3 DNS-Server',
    'Too many failed logins. Try again in {s} s.': 'Zu viele fehlgeschlagene Anmeldungen. Versuchen Sie es in {s} s erneut.',
    'Login failed': 'Anmeldung fehlgeschlagen',
    'Logged out.': 'Abgemeldet.',
    'Request failed ({status})': 'Anfrage fehlgeschlagen ({status})',
    'Applied. The device restarts once with these settings; confirm the network settings within 120 s after the start. Reload this page in about a minute.':
      'Übernommen. Das Gerät startet einmal mit diesen Einstellungen neu; bestätigen Sie die Netzwerkeinstellungen innerhalb von 120 s nach dem Start. Laden Sie diese Seite in etwa einer Minute neu.',
    'Applied. The device restarts once with these settings. Reload this page in about a minute.':
      'Übernommen. Das Gerät startet einmal mit diesen Einstellungen neu. Laden Sie diese Seite in etwa einer Minute neu.',
    'Applied. Confirm the network settings within 120 s.': 'Übernommen. Bestätigen Sie die Netzwerkeinstellungen innerhalb von 120 s.',
    'Applied (not saved).': 'Übernommen (nicht gespeichert).',
    'Saved.': 'Gespeichert.',
    '{message} Reload this page and log in again in about a minute.': '{message} Laden Sie diese Seite in etwa einer Minute neu und melden Sie sich erneut an.',
    '{message} Reload this page in about a minute.': '{message} Laden Sie diese Seite in etwa einer Minute neu.',
    '{message} Log in with the compiled password after the restart.': '{message} Melden Sie sich nach dem Neustart mit dem einkompilierten Passwort an.',
    'Restart failed': 'Neustart fehlgeschlagen',
    'Restart the device now? Applied settings that were not saved are discarded.':
      'Das Gerät jetzt neu starten? Übernommene, aber nicht gespeicherte Einstellungen werden verworfen.',
    'Confirmation failed': 'Bestätigung fehlgeschlagen',
    'Erase all saved settings, the password and the allow-list, and restart?':
      'Alle gespeicherten Einstellungen, das Passwort und die Allow-List löschen und neu starten?',
    'Factory reset failed': 'Zurücksetzen auf Werkseinstellungen fehlgeschlagen',
    'The two new passwords differ.': 'Die beiden neuen Passwörter stimmen nicht überein.',
    'The new password needs 12 to 128 characters.': 'Das neue Passwort braucht 12 bis 128 Zeichen.',
    'Password change failed': 'Passwortänderung fehlgeschlagen',
    'Allow-list could not be stored': 'Allow-List konnte nicht gespeichert werden',

    // ---- Fixed messages of the firmware ----
    'Not logged in': 'Nicht angemeldet',
    'Wrong password': 'Falsches Passwort',
    'Wrong current password': 'Falsches aktuelles Passwort',
    'Too many failed logins, please wait': 'Zu viele fehlgeschlagene Anmeldungen, bitte warten',
    'Too many failed password checks, please wait': 'Zu viele fehlgeschlagene Passwortprüfungen, bitte warten',
    'Invalid JSON': 'Ungültiges JSON',
    'Request body missing or too large': 'Anfrage leer oder zu groß',
    'Request rejected (origin)': 'Anfrage abgelehnt (Origin)',
    'Request rejected (origin or CSRF token)': 'Anfrage abgelehnt (Origin oder CSRF-Token)',
    'Not found': 'Nicht gefunden',
    'Out of memory': 'Zu wenig Speicher',
    'Session could not be created': 'Sitzung konnte nicht angelegt werden',
    'Settings could not be written to NVS': 'Einstellungen konnten nicht in NVS geschrieben werden',
    'One-shot settings could not be written': 'Einmalige Einstellungen konnten nicht geschrieben werden',
    'NVS could not be erased': 'NVS konnte nicht gelöscht werden',
    'Password could not be stored': 'Passwort konnte nicht gespeichert werden',
    'Apply and confirm the network settings before saving them': 'Übernehmen und bestätigen Sie die Netzwerkeinstellungen, bevor Sie sie speichern',
    'Confirm or wait for the pending network change first': 'Bestätigen Sie zuerst die ausstehende Netzwerkänderung oder warten Sie sie ab',
    'Confirm the pending network change first': 'Bestätigen Sie zuerst die ausstehende Netzwerkänderung',
    'No network change to confirm': 'Keine Netzwerkänderung zu bestätigen',
    'Open the page over the new static address and confirm there': 'Öffnen Sie die Seite über die neue statische Adresse und bestätigen Sie dort',
    'Refused: your own address would not be allowed any more': 'Abgelehnt: Ihre eigene Adresse wäre nicht mehr erlaubt',
    'Allow-list cleared: every source address may connect.': 'Allow-List geleert: Jede Quelladresse darf sich verbinden.',
    'Allow-list stored and active.': 'Allow-List gespeichert und aktiv.',
    'Factory reset done. The device restarts with the compiled settings.': 'Auf Werkseinstellungen zurückgesetzt. Das Gerät startet mit den einkompilierten Einstellungen neu.',
    'Network settings confirmed. Use Save to keep them after a restart.': 'Netzwerkeinstellungen bestätigt. Mit Einstellungen speichern bleiben sie nach einem Neustart erhalten.',
    'Password changed and stored. All other sessions were closed.': 'Passwort geändert und gespeichert. Alle anderen Sitzungen wurden beendet.',
    'The device restarts now and accepts firmware updates until its next restart.': 'Das Gerät startet jetzt neu und nimmt Firmware-Updates bis zu seinem nächsten Neustart an.',
    'The device restarts now. Applied settings that were not saved are discarded.': 'Das Gerät startet jetzt neu. Übernommene, aber nicht gespeicherte Einstellungen werden verworfen.',
    'Settings saved. They are loaded at every start.': 'Einstellungen gespeichert. Sie werden bei jedem Start geladen.',
    'Settings saved. Some saved values differ from the values in use: they take effect after Apply or a restart.':
      'Einstellungen gespeichert. Einige gespeicherte Werte weichen von den aktiven ab: Sie wirken nach Übernehmen oder einem Neustart.',
    'Over The Ethernet updates are switched off in the settings': 'Over-The-Ethernet-Updates sind in den Einstellungen ausgeschaltet',
    'Send the application image (.bin) as application/octet-stream': 'Senden Sie das Application-Image (.bin) als application/octet-stream',
    'Send the application image with its Content-Length': 'Senden Sie das Application-Image mit Content-Length',
    'A firmware image was already accepted; wait for the restart': 'Ein Firmware-Image wurde bereits angenommen; warten Sie auf den Neustart',
    'Firmware accepted. The device restarts in 5 seconds.': 'Firmware angenommen. Das Gerät startet in 5 Sekunden neu.',
    'No inactive OTA partition': 'Keine inaktive OTA-Partition',
    'The image is larger than the OTA partition': 'Das Image ist größer als die OTA-Partition',
    'The image belongs to another project': 'Das Image gehört zu einem anderen Projekt',
    'The image was built for another chip or chip revision (see the build profiles in README.md)':
      'Das Image wurde für einen anderen Chip oder eine andere Chip-Revision gebaut (siehe die Build-Profile in README.md)',
    'Not an application image (use the .bin of the build folder, not a merged flash image)':
      'Kein Application-Image (verwenden Sie die .bin aus dem Build-Ordner, kein zusammengeführtes Flash-Image)',
    'The OTA partition could not be prepared': 'Die OTA-Partition konnte nicht vorbereitet werden',
    'Writing the OTA partition failed': 'Schreiben der OTA-Partition fehlgeschlagen',
    'Upload interrupted or truncated': 'Upload unterbrochen oder abgeschnitten',
    'Upload interrupted': 'Upload unterbrochen',
    'Upload timeout': 'Zeitüberschreitung beim Upload',
    'The image did not pass the verification': 'Das Image hat die Prüfung nicht bestanden',
    'The new image could not be selected for the next boot': 'Das neue Image konnte nicht für den nächsten Start ausgewählt werden',
    'The new password must differ from the current one': 'Das neue Passwort muss sich vom aktuellen unterscheiden',
    'New password: 12 to 128 characters, no control characters': 'Neues Passwort: 12 bis 128 Zeichen, keine Steuerzeichen',
    'Field gnss_protocol: ubx or nmea expected': 'Feld gnss_protocol: ubx oder nmea erwartet',
    'Field led_colors: object expected': 'Feld led_colors: Objekt erwartet',
    'Field led_type: none, ky016 or ws2812 expected': 'Feld led_type: none, ky016 oder ws2812 erwartet',
    'FQDN: 1 to 253 characters': 'FQDN: 1 bis 253 Zeichen',
    'FQDN: a label must not start with \'-\'': 'FQDN: Ein Label darf nicht mit \'-\' beginnen',
    'FQDN: every label needs 1 to 63 characters and must not end with \'-\'': 'FQDN: Jedes Label braucht 1 bis 63 Zeichen und darf nicht mit \'-\' enden',
    'FQDN: must not be an IP address': 'FQDN: darf keine IP-Adresse sein',
    'FQDN: only letters, digits, \'-\' and \'.\' are allowed': 'FQDN: Nur Buchstaben, Ziffern, \'-\' und \'.\' sind erlaubt',
    'FQDN: the top-level domain must not be numeric': 'FQDN: Die Top-Level-Domain darf nicht numerisch sein',
    'GNSS baud rate is not one of the supported values': 'Die GNSS-Baudrate ist keiner der unterstützten Werte',
    'IPv4 address is not valid': 'Die IPv4-Adresse ist ungültig',
    'IPv4 address must not be the network or broadcast address': 'Die IPv4-Adresse darf nicht die Netz- oder Broadcast-Adresse sein',
    'IPv4 address: not a usable unicast address': 'IPv4-Adresse: keine nutzbare Unicast-Adresse',
    'IPv4 gateway is not valid': 'Das IPv4-Gateway ist ungültig',
    'IPv4 gateway must be another host address of the subnet': 'Das IPv4-Gateway muss eine andere Host-Adresse des Subnetzes sein',
    'IPv4 gateway must be in the subnet of the address': 'Das IPv4-Gateway muss im Subnetz der Adresse liegen',
    'IPv4 prefix length: 8 to 30': 'IPv4-Prefix-Länge: 8 bis 30',
    'IPv6 address is not valid': 'Die IPv6-Adresse ist ungültig',
    'IPv6 address: a global or unique local unicast address is required': 'IPv6-Adresse: Eine globale oder Unique-Local-Unicast-Adresse ist erforderlich',
    'IPv6 gateway is not valid': 'Das IPv6-Gateway ist ungültig',
    'IPv6 gateway must be a link-local address or in the /64 of the address': 'Das IPv6-Gateway muss eine Link-Local-Adresse sein oder im /64 der Adresse liegen',
    'IPv6 gateway requires a static IPv6 address': 'Ein IPv6-Gateway erfordert eine statische IPv6-Adresse',
    'IPv6 prefix length: 64 to 128': 'IPv6-Prefix-Länge: 64 bis 128',
    'KY-016 LED pins: one of the GPIOs cannot be used': 'KY-016-LED-Pins: Einer der GPIOs kann nicht verwendet werden',
    'LCD title: 1 to 17 printable ASCII characters': 'LCD-Titel: 1 bis 17 druckbare ASCII-Zeichen',
    'LED brightness: 0 to 100 %': 'LED-Helligkeit: 0 bis 100 %',
    'LED type is not valid': 'Der LED-Typ ist ungültig',
    'MAC address: 00:00:00:00:00:00 cannot be used': 'MAC-Adresse: 00:00:00:00:00:00 kann nicht verwendet werden',
    'MAC address: xx:xx:xx:xx:xx:xx (hexadecimal) expected': 'MAC-Adresse: xx:xx:xx:xx:xx:xx (hexadezimal) erwartet',
    'MQTT QoS: 0, 1 or 2': 'MQTT QoS: 0, 1 oder 2',
    'MQTT broker URI is required when MQTT is on': 'Die MQTT-Broker-URI ist erforderlich, wenn MQTT an ist',
    'MQTT broker URI is too long': 'Die MQTT-Broker-URI ist zu lang',
    'MQTT broker URI must start with mqtt:// or mqtts://': 'Die MQTT-Broker-URI muss mit mqtt:// oder mqtts:// beginnen',
    'MQTT broker URI: IPv6 address must be in brackets': 'MQTT-Broker-URI: Die IPv6-Adresse muss in eckigen Klammern stehen',
    'MQTT broker URI: port 1 to 65535': 'MQTT-Broker-URI: Port 1 bis 65535',
    'MQTT broker URI: scheme://host[:port] without user, path or spaces': 'MQTT-Broker-URI: scheme://host[:port] ohne Benutzer, Pfad oder Leerzeichen',
    'MQTT password: at most 64 printable ASCII characters': 'MQTT-Passwort: höchstens 64 druckbare ASCII-Zeichen',
    'MQTT topic: 1 to 100 printable ASCII characters without spaces, \'+\' and \'#\'': 'MQTT-Topic: 1 bis 100 druckbare ASCII-Zeichen ohne Leerzeichen, \'+\' und \'#\'',
    'MQTT user name: at most 64 printable ASCII characters': 'MQTT-Benutzername: höchstens 64 druckbare ASCII-Zeichen',
    'NTP authentication needs at least one key': 'NTP-Authentifizierung braucht mindestens einen Schlüssel',
    'TF card pins: one of the GPIOs cannot be used': 'TF-Karten-Pins: Einer der GPIOs kann nicht verwendet werden',
    'Time zone is not a valid POSIX TZ string': 'Die Zeitzone ist kein gültiger POSIX-TZ-String'
  };

  // Messages of the firmware with values (printf formats as in the C++ source; {1}, {2} = the values in order)
  const server = [
    ['At most %u DNS servers', 'Höchstens {1} DNS-Server'],
    ['At most %u NTP keys', 'Höchstens {1} NTP-Schlüssel'],
    ['At most %u entries', 'Höchstens {1} Einträge'],
    ['DNS server %u is not valid', 'DNS-Server {1} ist ungültig'],
    ['DNS server %u is not an IPv4 or IPv6 address', 'DNS-Server {1} ist keine IPv4- oder IPv6-Adresse'],
    ['DNS server %u: not a unicast address', 'DNS-Server {1}: keine Unicast-Adresse'],
    ['Entry %u is not an address or CIDR network', 'Eintrag {1} ist keine Adresse und kein CIDR-Netz'],
    ['Field %s: text of at most %u characters expected', 'Feld {1}: Text mit höchstens {2} Zeichen erwartet'],
    ['Field %s: true or false expected', 'Feld {1}: true oder false erwartet'],
    ['Field %s: whole number from %ld to %ld expected', 'Feld {1}: ganze Zahl von {2} bis {3} erwartet'],
    ['FQDN: the host name (first label) may have at most %u characters', 'FQDN: Der Hostname (erstes Label) darf höchstens {1} Zeichen haben'],
    ['GNSS RX pin: GPIO %d cannot be used', 'GNSS-RX-Pin: GPIO {1} kann nicht verwendet werden'],
    ['GNSS TX pin: GPIO %d cannot be used', 'GNSS-TX-Pin: GPIO {1} kann nicht verwendet werden'],
    ['PPS pin: GPIO %d cannot be used', 'PPS-Pin: GPIO {1} kann nicht verwendet werden'],
    ['PTP PPS loopback pin: GPIO %d cannot be used', 'PTP-PPS-Loopback-Pin: GPIO {1} kann nicht verwendet werden'],
    ['WS2812 data pin: GPIO %d cannot be used', 'WS2812-Daten-Pin: GPIO {1} kann nicht verwendet werden'],
    ['GPIO %d is used twice (%s, %s)', 'GPIO {1} ist doppelt belegt ({2}, {3})'],
    ['LED colour %u is not a 24-bit RGB value', 'LED-Farbe {1} ist kein 24-Bit-RGB-Wert'],
    ['LED colour %s: #RRGGBB expected', 'LED-Farbe {1}: #RRGGBB erwartet'],
    ['MQTT client limit with TF card: %u to %u', 'MQTT-Client-Limit mit TF-Karte: {1} bis {2}'],
    ['NTP key %u: exactly 32 characters', 'NTP-Schlüssel {1}: genau 32 Zeichen'],
    ['NTP key %u: key ID 1 to 65535', 'NTP-Schlüssel {1}: Key-ID 1 bis 65535'],
    ['NTP key %u: letters and digits only', 'NTP-Schlüssel {1}: nur Buchstaben und Ziffern'],
    ['NTP key %u: key ID 1 to 65535 and 32 letters or digits expected', 'NTP-Schlüssel {1}: Key-ID 1 bis 65535 und 32 Buchstaben oder Ziffern erwartet'],
    ['NTP key ID %lu is used twice', 'NTP-Key-ID {1} ist doppelt vergeben'],
    ['NTP keys: at most %u', 'NTP-Schlüssel: höchstens {1}'],
    ['Rate limit per client: 0 (no limit) to %u requests per second', 'Rate-Limit pro Client: 0 (unbegrenzt) bis {1} Anfragen pro Sekunde'],
    ['Minimum GNSS fix age: 0 (off) to %u seconds', 'Mindestalter des GNSS-Fix: 0 (aus) bis {1} Sekunden'],
    ['Minimum satellites in use: 0 (off) to %u', 'Mindestzahl verwendeter Satelliten: 0 (aus) bis {1}'],
    ['PPS holdover: 0 to %u seconds', 'PPS-Holdover: 0 bis {1} Sekunden'],
    ['Serving time after the PPS loss: 0 (no limit) to %u minutes', 'Antwortzeit nach dem PPS-Ausfall: 0 (unbegrenzt) bis {1} Minuten'],
    ['Stratum after the holdover: 2 to 16', 'Stratum nach dem Holdover: 2 bis 16'],
    ['Root dispersion growth: 1 to %u ppm (0.001 to 10 ms per second)', 'Zunahme der Root Dispersion: 1 bis {1} ppm (0,001 bis 10 ms pro Sekunde)'],
    ['MQTT reporting period: 30 to 86400 s', 'MQTT-Berichtsintervall: 30 bis 86400 s']
  ];

  // Texts that stay English on purpose (names, technical terms, examples): listed so that app.js does not report
  // them as missing
  for (const text of ['ESP32 Time Server', 'UTC', 'Firmware', 'MQTT', 'MAC', 'IPv4', 'IPv6', 'RGB LED', 'Broker URI', 'NTP',
    'qErr', 'ADEV', 'TDEV',
    'Topic', 'CMD (GPIO)', 'CLK (GPIO)', 'D0 (GPIO)', 'D1 (GPIO)', 'D2 (GPIO)', 'D3 (GPIO)', 'KY-016', 'WS2812',
    'CET-1CEST,M3.5.0,M10.5.0/3', '192.168.1.0/24 fd00::/8']) {
    if (!Object.prototype.hasOwnProperty.call(de, text)) de[text] = text;
  }

  window.I18N = { lang: lang, de: de, server: server };
})();
