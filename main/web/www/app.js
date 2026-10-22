// ESP32 Time Server - web admin page
// Values from the server are only written with textContent / .value (never as HTML).
'use strict';

(function () {
  const $ = (id) => document.getElementById(id);
  const form = $('settings-form');
  const f = form.elements;
  let csrf = '';
  let state = null;
  let pendingDeadline = 0;
  let refreshTimer = 0;
  let countdownTimer = 0;

  // ---- Theme: the system setting unless one was chosen with the button (stored in this browser) ----
  const darkQuery = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  function storedTheme() {
    try { return localStorage.getItem('theme'); } catch (e) { return null; }
  }
  function currentTheme() {
    const stored = storedTheme();
    if (stored === 'dark' || stored === 'light') return stored;
    return darkQuery && darkQuery.matches ? 'dark' : 'light';
  }
  function applyTheme() {
    const theme = currentTheme();
    const stored = storedTheme();
    if (stored === 'dark' || stored === 'light') document.documentElement.setAttribute('data-theme', stored);
    else document.documentElement.removeAttribute('data-theme'); // CSS follows the system
    // The button shows the mode in use: U+1F319 (crescent moon) = dark, U+2600 (sun) = light
    const toggle = $('theme-toggle');
    toggle.textContent = theme === 'dark' ? '\u{1F319}' : '☀️';
    const label = theme === 'dark' ? 'Dark mode (click for light mode)' : 'Light mode (click for dark mode)';
    toggle.setAttribute('aria-label', label);
    toggle.title = label;
  }
  applyTheme();
  if (darkQuery && darkQuery.addEventListener) darkQuery.addEventListener('change', applyTheme);
  $('theme-toggle').addEventListener('click', () => {
    const next = currentTheme() === 'dark' ? 'light' : 'dark';
    try { localStorage.setItem('theme', next); } catch (e) { /* storage not available */ }
    applyTheme();
  });

  // ---- Header ----
  function setHost(fqdn) {
    const host = String(fqdn || '').split('.')[0] || 'ESP32 Time Server';
    $('hdr-host').textContent = host;
    $('hdr-sub').textContent = fqdn && fqdn.indexOf('.') > 0 ? fqdn + ' · ESP32 Time Server' : 'ESP32 Time Server';
    document.title = host + ' – ESP32 Time Server';
  }

  // ---- Messages ----
  function show(text, kind) {
    const m = $('message');
    m.textContent = text;
    m.className = 'banner ' + (kind || 'ok');
    m.hidden = !text;
    if (text) m.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }

  // ---- API ----
  async function api(path, body) {
    const options = { method: body === undefined ? 'GET' : 'POST', credentials: 'same-origin', cache: 'no-store', headers: {} };
    if (body !== undefined) {
      options.headers['Content-Type'] = 'application/json';
      if (csrf) options.headers['X-CSRF-Token'] = csrf;
      options.body = JSON.stringify(body);
    }
    let response;
    try {
      response = await fetch(path, options);
    } catch (e) {
      return { status: 0, data: { error: 'The device does not answer (restarting or a different address?)' } };
    }
    let data = {};
    try { data = await response.json(); } catch (e) { data = {}; }
    if (response.status === 401 && path !== '/api/login') showLogin();
    return { status: response.status, data: data };
  }

  function showLogin() {
    csrf = '';
    state = null;
    clearInterval(refreshTimer);
    clearInterval(countdownTimer);
    $('app').hidden = true;
    $('logout').hidden = true;
    for (const id of ['banner-default-pw', 'banner-network', 'banner-oneshot', 'banner-unsaved']) $(id).hidden = true;
    $('login-card').hidden = false;
    $('login-password').focus();
    api('/api/info').then((r) => { if (r.status === 200) setHost(r.data.hostname); });
  }

  // ---- LED colours ----
  const ledStates = [
    ['startup', 'Start-up', 'until the server is ready'],
    ['normal', 'Normal', 'operating normally'],
    ['sync', 'Time sync', 'the time is being set'],
    ['warning', 'Warning', 'GNSS / PPS problem, queued MQTT reports'],
    ['alert', 'Alert', 'no IP address, load shedding'],
    ['critical', 'Critical', 'Ethernet, GNSS link or MQTT failure'],
    ['button', 'Button pressed', 'uptime button']
  ];
  const presets = [
    ['#000000', 'Off'], ['#FF0000', 'Red'], ['#00FF00', 'Green'], ['#0000FF', 'Blue'], ['#FFFF00', 'Yellow'],
    ['#FFFFFF', 'White'], ['#FFA500', 'Orange'], ['#FFC0CB', 'Pink']
  ];
  const colourRows = {};

  function normaliseHex(text) {
    const t = String(text || '').trim().toUpperCase();
    const m = /^#?([0-9A-F]{6})$/.exec(t);
    return m ? '#' + m[1] : null;
  }

  function buildColourTable() {
    const table = $('led-colours');
    for (const [key, name, meaning] of ledStates) {
      const label = document.createElement('div');
      label.className = 'state';
      label.textContent = name;
      const small = document.createElement('small');
      small.textContent = meaning;
      label.appendChild(small);

      const select = document.createElement('select');
      select.setAttribute('aria-label', name + ' colour');
      for (const [hex, presetName] of presets) {
        const o = document.createElement('option');
        o.value = hex;
        o.textContent = presetName;
        select.appendChild(o);
      }
      const custom = document.createElement('option');
      custom.value = 'custom';
      custom.textContent = 'Custom';
      select.appendChild(custom);

      const input = document.createElement('input');
      input.type = 'text';
      input.maxLength = 7;
      input.placeholder = '#RRGGBB';
      input.spellcheck = false;
      input.setAttribute('aria-label', name + ' colour as #RRGGBB');

      const swatch = document.createElement('span');
      swatch.className = 'swatch';

      select.addEventListener('change', () => {
        if (select.value !== 'custom') input.value = select.value;
        else input.focus();
        updateColourRow(key);
        markDefaults();
      });
      input.addEventListener('input', () => { updateColourRow(key, true); markDefaults(); });

      table.append(label, select, input, swatch);
      colourRows[key] = { select, input, swatch };
    }
  }

  function updateColourRow(key, fromInput) {
    const row = colourRows[key];
    const hex = normaliseHex(row.input.value);
    row.input.classList.toggle('bad', hex === null);
    if (fromInput) row.select.value = hex && presets.some((p) => p[0] === hex) ? hex : 'custom';
    // CSSOM (allowed by the CSP, unlike style attributes)
    row.swatch.style.backgroundColor = hex || 'transparent';
  }

  function setColours(colours) {
    for (const [key] of ledStates) {
      colourRows[key].input.value = (colours && colours[key]) || '#000000';
      updateColourRow(key, true);
    }
  }

  // ---- Password fields: eye button for clear text ----
  // Only text typed in this session can be shown: the server never sends a stored secret, and an emptied field is
  // masked again.
  const passwordEyes = [];
  function syncPasswordEye(entry) {
    const empty = entry.input.value === '';
    if (empty || entry.input.disabled) entry.input.type = 'password';
    entry.button.hidden = empty || entry.input.disabled;
    const shown = entry.input.type === 'text';
    entry.button.setAttribute('aria-pressed', shown ? 'true' : 'false');
    entry.button.title = shown ? 'Hide the text' : 'Show the text';
  }
  function syncPasswordEyes() {
    passwordEyes.forEach(syncPasswordEye);
  }
  document.querySelectorAll('input[type="password"]').forEach((input) => {
    const wrap = document.createElement('span');
    wrap.className = 'pw-field';
    input.parentNode.insertBefore(wrap, input);
    wrap.appendChild(input);
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'eye';
    button.setAttribute('aria-label', 'Show or hide the text');
    wrap.appendChild(button);
    const entry = { input: input, button: button };
    button.addEventListener('click', () => {
      if (input.value !== '') input.type = input.type === 'password' ? 'text' : 'password';
      syncPasswordEye(entry);
    });
    input.addEventListener('input', () => syncPasswordEye(entry));
    passwordEyes.push(entry);
    syncPasswordEye(entry);
  });

  // ---- NTP keys ----
  let ntpKeyRows = [];
  let ntpKeysChanged = false;

  // 32 characters [0-9A-Za-z] from the cryptographic random generator of the browser (rejection sampling: no bias)
  function randomKey() {
    const chars = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
    const out = [];
    const buffer = new Uint8Array(64);
    while (out.length < 32) {
      crypto.getRandomValues(buffer);
      for (const b of buffer) {
        if (b < 248 && out.length < 32) out.push(chars[b % 62]); // 248 = 4 * 62
      }
    }
    return out.join('');
  }

  function checkKeyRow(row) {
    row.key.classList.toggle('bad', !/^[0-9A-Za-z]{32}$/.test(row.key.value.trim()));
    const id = Number(row.id.value);
    row.id.classList.toggle('bad', !Number.isInteger(id) || id < 1 || id > 65535 ||
      ntpKeyRows.some((other) => other !== row && Number(other.id.value) === id));
  }

  function keysChanged() {
    ntpKeysChanged = true;
    ntpKeyRows.forEach(checkKeyRow);
    $('ntp-key-add').disabled = ntpKeyRows.length >= 8;
  }

  function addKeyRow(id, key) {
    const table = $('ntp-keys');
    const idInput = document.createElement('input');
    idInput.type = 'number';
    idInput.min = '1';
    idInput.max = '65535';
    idInput.value = String(id);
    idInput.setAttribute('aria-label', 'Key ID');
    const keyInput = document.createElement('input');
    keyInput.type = 'text';
    keyInput.maxLength = 32;
    keyInput.spellcheck = false;
    keyInput.autocomplete = 'off';
    keyInput.value = key;
    keyInput.setAttribute('aria-label', 'Key');
    const generate = document.createElement('button');
    generate.type = 'button';
    generate.textContent = 'Generate';
    generate.title = 'New random key (32 letters or digits)';
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.textContent = 'Remove';
    const row = { id: idInput, key: keyInput, elements: [idInput, keyInput, generate, remove] };
    generate.addEventListener('click', () => { keyInput.value = randomKey(); keysChanged(); });
    remove.addEventListener('click', () => {
      row.elements.forEach((el) => el.remove());
      ntpKeyRows = ntpKeyRows.filter((r) => r !== row);
      keysChanged();
    });
    idInput.addEventListener('input', keysChanged);
    keyInput.addEventListener('input', keysChanged);
    table.append(idInput, keyInput, generate, remove);
    ntpKeyRows.push(row);
    checkKeyRow(row);
  }

  // compiled: the keys shown are the ones of ESP32TimeServerSettingsSecrets.h
  function setNtpKeys(keys, compiled) {
    ntpKeyRows.forEach((row) => row.elements.forEach((el) => el.remove()));
    ntpKeyRows = [];
    for (const k of keys || []) addKeyRow(k.id, k.key);
    ntpKeysChanged = false;
    $('ntp-key-state').textContent = compiled
      ? 'No keys are set on this page: these are the keys compiled into the firmware. Changing them here stores them as keys of the web settings.'
      : 'Keys set on this page are in use.';
    $('ntp-key-add').disabled = ntpKeyRows.length >= 8;
  }

  $('ntp-key-add').addEventListener('click', () => {
    const used = ntpKeyRows.map((r) => Number(r.id.value));
    let id = 1;
    while (used.includes(id)) id++;
    addKeyRow(id, randomKey());
    keysChanged();
  });

  // ---- Settings <-> form ----
  function formValues(s) {
    // the form view of a settings object (also used for the defaults)
    return {
      fqdn: s.fqdn,
      ipv4_dhcp: s.ipv4_dhcp,
      ipv4_cidr: s.ipv4_address ? s.ipv4_address + '/' + s.ipv4_prefix : '',
      ipv4_gateway: s.ipv4_gateway,
      ipv6_slaac: s.ipv6_slaac,
      ipv6_cidr: s.ipv6_address ? s.ipv6_address + '/' + s.ipv6_prefix : '',
      ipv6_gateway: s.ipv6_gateway,
      dns: (s.dns || []).join('\n'),
      // "" = hardware MAC: the field shows the hardware MAC
      mac_address: s.mac_address || (state && state.status ? state.status.mac_hardware : ''),
      gnss_protocol: s.gnss_protocol,
      gnss_tx_pin: String(s.gnss_tx_pin),
      gnss_rx_pin: String(s.gnss_rx_pin),
      gnss_pps_pin: String(s.gnss_pps_pin),
      gnss_baud: String(s.gnss_baud),
      ptp_loopback_pin: s.ptp_loopback_pin < 0 ? '' : String(s.ptp_loopback_pin),
      pps_holdover: String(s.pps_holdover),
      pps_lost_serve: s.pps_lost_serve,
      pps_lost_serve_min: String(s.pps_lost_serve_min),
      pps_lost_stratum: String(s.pps_lost_stratum),
      lcd_enabled: s.lcd_enabled,
      lcd_title: s.lcd_title,
      lcd_24_hour: s.lcd_24_hour,
      lcd_pad_hour: s.lcd_pad_hour,
      lcd_show_time_zone: s.lcd_show_time_zone,
      time_zone: s.time_zone,
      led_type: s.led_type,
      led_brightness: String(s.led_brightness),
      led_red_pin: String(s.led_red_pin),
      led_green_pin: String(s.led_green_pin),
      led_blue_pin: String(s.led_blue_pin),
      led_ws2812_pin: String(s.led_ws2812_pin),
      mqtt_enabled: s.mqtt_enabled,
      mqtt_uri: s.mqtt_uri,
      mqtt_username: s.mqtt_username,
      mqtt_topic: s.mqtt_topic,
      mqtt_qos: String(s.mqtt_qos),
      mqtt_retain: s.mqtt_retain,
      mqtt_report_period: String(s.mqtt_report_period),
      mqtt_client_reporting: s.mqtt_client_reporting,
      mqtt_memory_reporting: s.mqtt_memory_reporting,
      mqtt_historical_reporting: s.mqtt_historical_reporting,
      mqtt_distinguish_auth: s.mqtt_distinguish_auth,
      mqtt_tf_client_limit: String(s.mqtt_tf_client_limit),
      tf_cmd_pin: String(s.tf_cmd_pin),
      tf_clk_pin: String(s.tf_clk_pin),
      tf_d0_pin: String(s.tf_d0_pin),
      tf_d1_pin: String(s.tf_d1_pin),
      tf_d2_pin: String(s.tf_d2_pin),
      tf_d3_pin: String(s.tf_d3_pin),
      ntp_auth_enabled: s.ntp_auth_enabled,
      ote_enabled: s.ote_enabled,
      ote_device_name: s.ote_device_name
    };
  }

  const displayNames = {
    gnss_protocol: { nmea: 'NMEA Standard Protocol (needs RMC sentence)', ubx: 'U-Blox UBX Protocol' },
    led_type: { none: 'None', ky016: 'KY-016', ws2812: 'WS2812' },
    mqtt_qos: { 0: '0 (at most once)', 1: '1 (at least once)', 2: '2 (exactly once)' }
  };

  function fillForm(s) {
    const values = formValues(s);
    for (const name of Object.keys(values)) {
      const el = f[name];
      if (!el) continue;
      if (el.type === 'checkbox') el.checked = !!values[name];
      else el.value = values[name];
    }
    setColours(s.led_colors);
    f.mqtt_password.value = '';
    f.mqtt_password_clear.checked = false;
    f.ntp_key_clear.checked = false;
    setNtpKeys(s.ntp_key_override ? s.ntp_keys : state.ntp_compiled_keys, !s.ntp_key_override);
    f.ote_password.value = '';
    $('ote-password-state').textContent = s.ote_password_set ? 'A password is set.' : 'No password is set.';
    $('mqtt-password-state').textContent = s.mqtt_password_set ? 'A password is set.' : 'No password is set.';
    updateDependent();
    markDefaults();
  }

  function currentValue(name) {
    const el = f[name];
    return el.type === 'checkbox' ? el.checked : el.value.trim();
  }

  function markDefaults() {
    if (!state) return;
    const defaults = formValues(state.defaults);
    document.querySelectorAll('small.def').forEach((small) => {
      const name = small.getAttribute('data-for');
      const value = defaults[name];
      let text = value === '' || value === undefined ? '(empty)' : String(value);
      if (typeof value === 'boolean') text = value ? 'on' : 'off';
      if (name === 'dns') text = value ? value.replace(/\n/g, ', ') : '(empty: received servers)';
      if (name === 'ptp_loopback_pin' && value === '') text = 'off';
      if (name === 'led_brightness') text += ' %';
      if (name === 'mqtt_report_period') text += ' s';
      if (name === 'pps_holdover') text = value === '0' ? 'off' : text + ' s';
      if (name === 'pps_lost_serve_min') text = value === '0' ? 'no limit' : text + ' min';
      if (name === 'mac_address' && !state.defaults.mac_address) text = 'the hardware MAC';
      if (displayNames[name]) text = displayNames[name][value] || text;
      small.textContent = 'Default: ' + text;
      const differs = String(currentValue(name)).replace(/\r/g, '') !== String(value);
      small.classList.toggle('differs', differs);
      f[name].classList.toggle('is-changed', differs);
    });
    for (const [key] of ledStates) {
      const row = colourRows[key];
      const def = state.defaults.led_colors ? state.defaults.led_colors[key] : null;
      const differs = def !== null && normaliseHex(row.input.value) !== def;
      row.input.classList.toggle('is-changed', differs);
      row.input.title = 'Default: ' + (def || '');
    }
  }

  function updateDependent() {
    const dhcp = f.ipv4_dhcp.checked;
    f.ipv4_cidr.disabled = dhcp;
    f.ipv4_gateway.disabled = dhcp;
    const ubx = f.gnss_protocol.value === 'ubx';
    f.gnss_baud.disabled = ubx;
    $('baud-note').hidden = !ubx;
    const type = f.led_type.value;
    document.querySelectorAll('.led-ky016').forEach((el) => { el.hidden = type !== 'ky016'; });
    document.querySelectorAll('.led-ws2812').forEach((el) => { el.hidden = type !== 'ws2812'; });
    $('brightness-out').textContent = f.led_brightness.value + ' %';
    f.mqtt_password.disabled = f.mqtt_password_clear.checked;
    f.pps_lost_serve_min.disabled = !f.pps_lost_serve.checked;
    f.pps_lost_stratum.disabled = !f.pps_lost_serve.checked;
    // The client limit only matters with the client list; the report buffer is sized from it (RAM, no PSRAM)
    f.mqtt_tf_client_limit.disabled = !f.mqtt_client_reporting.checked;
    const limit = Number(f.mqtt_tf_client_limit.value);
    $('mqtt-buffer-note').textContent = f.mqtt_client_reporting.checked && Number.isInteger(limit) && limit >= 50 && limit <= 1000
      ? 'Report buffer: ' + ((1166 + 76 * limit) / 1024).toFixed(1) + ' kB of RAM (1166 bytes plus 76 bytes per client). Without the TF card at most 50 clients are listed.'
      : (f.mqtt_client_reporting.checked ? '50 to 1000.' : 'Only used with client activity reporting.');
    const compiledOnly = f.ntp_key_clear.checked;
    document.querySelectorAll('#ntp-keys input, #ntp-keys button').forEach((el) => { el.disabled = compiledOnly; });
    $('ntp-key-add').disabled = compiledOnly || ntpKeyRows.length >= 8;
    syncPasswordEyes();
    if (state) {
      // Updates switched on while the OTE password is still the published compiled default (and no new one is typed)
      $('ote-default-pw-warning').hidden = !(f.ote_enabled.checked && state.ote_password_default && f.ote_password.value === '');
      $('ptp-chip-warning').hidden = state.status.ptp_loopback_supported || f.ptp_loopback_pin.value.trim() === '';
    }
  }

  // xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx (Cisco) or xxxxxxxxxxxx -> lower case with ':'
  // ('' stays '', null = not valid)
  function normaliseMac(text) {
    const t = text.trim().toLowerCase();
    if (t === '') return '';
    if (!/^([0-9a-f]{2}:){5}[0-9a-f]{2}$|^([0-9a-f]{2}-){5}[0-9a-f]{2}$|^([0-9a-f]{4}\.){2}[0-9a-f]{4}$|^[0-9a-f]{12}$/.test(t)) return null;
    return t.replace(/[^0-9a-f]/g, '').match(/../g).join(':');
  }

  function splitCidr(text, defaultPrefix) {
    const parts = text.split('/');
    if (parts.length > 2) return null;
    const prefix = parts.length === 2 ? Number(parts[1]) : defaultPrefix;
    if (!Number.isInteger(prefix)) return null;
    return { address: parts[0].trim(), prefix: prefix };
  }

  function pin(name, label, allowEmpty) {
    const text = f[name].value.trim();
    if (text === '' && allowEmpty) return -1;
    const value = Number(text);
    if (text === '' || !Number.isInteger(value)) throw new Error(label + ': GPIO number expected');
    return value;
  }

  function collect() {
    const body = {
      fqdn: f.fqdn.value.trim(),
      ipv4_dhcp: f.ipv4_dhcp.checked,
      ipv6_slaac: f.ipv6_slaac.checked,
      ipv6_gateway: f.ipv6_gateway.value.trim(),
      gnss_protocol: f.gnss_protocol.value,
      gnss_tx_pin: pin('gnss_tx_pin', 'GNSS TX pin'),
      gnss_rx_pin: pin('gnss_rx_pin', 'GNSS RX pin'),
      gnss_pps_pin: pin('gnss_pps_pin', 'PPS pin'),
      gnss_baud: Number(f.gnss_baud.value),
      ptp_loopback_pin: pin('ptp_loopback_pin', 'PTP PPS loopback pin', true),
      pps_holdover: Number(f.pps_holdover.value),
      pps_lost_serve: f.pps_lost_serve.checked,
      pps_lost_serve_min: Number(f.pps_lost_serve_min.value),
      pps_lost_stratum: Number(f.pps_lost_stratum.value),
      lcd_enabled: f.lcd_enabled.checked,
      lcd_title: f.lcd_title.value,
      lcd_24_hour: f.lcd_24_hour.checked,
      lcd_pad_hour: f.lcd_pad_hour.checked,
      lcd_show_time_zone: f.lcd_show_time_zone.checked,
      time_zone: f.time_zone.value.trim(),
      led_type: f.led_type.value,
      led_brightness: Number(f.led_brightness.value),
      led_red_pin: pin('led_red_pin', 'LED red pin'),
      led_green_pin: pin('led_green_pin', 'LED green pin'),
      led_blue_pin: pin('led_blue_pin', 'LED blue pin'),
      led_ws2812_pin: pin('led_ws2812_pin', 'WS2812 data pin'),
      led_colors: {},
      mqtt_enabled: f.mqtt_enabled.checked,
      mqtt_uri: f.mqtt_uri.value.trim(),
      mqtt_username: f.mqtt_username.value,
      mqtt_password: f.mqtt_password.value,
      mqtt_password_clear: f.mqtt_password_clear.checked,
      mqtt_topic: f.mqtt_topic.value.trim(),
      mqtt_qos: Number(f.mqtt_qos.value),
      mqtt_retain: f.mqtt_retain.checked,
      mqtt_report_period: Number(f.mqtt_report_period.value),
      mqtt_client_reporting: f.mqtt_client_reporting.checked,
      mqtt_memory_reporting: f.mqtt_memory_reporting.checked,
      mqtt_historical_reporting: f.mqtt_historical_reporting.checked,
      mqtt_distinguish_auth: f.mqtt_distinguish_auth.checked,
      mqtt_tf_client_limit: Number(f.mqtt_tf_client_limit.value),
      tf_cmd_pin: pin('tf_cmd_pin', 'TF card CMD pin'),
      tf_clk_pin: pin('tf_clk_pin', 'TF card CLK pin'),
      tf_d0_pin: pin('tf_d0_pin', 'TF card D0 pin'),
      tf_d1_pin: pin('tf_d1_pin', 'TF card D1 pin'),
      tf_d2_pin: pin('tf_d2_pin', 'TF card D2 pin'),
      tf_d3_pin: pin('tf_d3_pin', 'TF card D3 pin'),
      ntp_auth_enabled: f.ntp_auth_enabled.checked,
      ntp_key_clear: f.ntp_key_clear.checked,
      ote_enabled: f.ote_enabled.checked,
      ote_device_name: f.ote_device_name.value.trim(),
      ote_password: f.ote_password.value
    };
    // NTP keys: only sent when they were changed (otherwise the compiled keys stay in use if they are)
    if (!body.ntp_key_clear && ntpKeysChanged) {
      body.ntp_keys = [];
      ntpKeyRows.forEach((row, i) => {
        const id = Number(row.id.value);
        const key = row.key.value.trim();
        if (!Number.isInteger(id) || id < 1 || id > 65535) throw new Error('NTP key ' + (i + 1) + ': key ID 1 to 65535');
        if (!/^[0-9A-Za-z]{32}$/.test(key)) throw new Error('NTP key ' + (i + 1) + ': exactly 32 letters or digits');
        body.ntp_keys.push({ id: id, key: key });
      });
    }
    for (const [key, name] of ledStates) {
      const hex = normaliseHex(colourRows[key].input.value);
      if (!hex) throw new Error('LED colour "' + name + '": #RRGGBB expected');
      body.led_colors[key] = hex;
    }
    if (!body.ipv4_dhcp) {
      const v4 = splitCidr(f.ipv4_cidr.value.trim(), NaN);
      if (!v4) throw new Error('IPv4 address: a.b.c.d/prefix length expected');
      body.ipv4_address = v4.address;
      body.ipv4_prefix = v4.prefix;
      body.ipv4_gateway = f.ipv4_gateway.value.trim();
    }
    const v6text = f.ipv6_cidr.value.trim();
    if (v6text === '') {
      body.ipv6_address = '';
      body.ipv6_prefix = 64;
    } else {
      const v6 = splitCidr(v6text, 64);
      if (!v6) throw new Error('IPv6 address: address/prefix length expected');
      body.ipv6_address = v6.address;
      body.ipv6_prefix = v6.prefix;
    }
    if (!Number.isInteger(body.pps_holdover) || body.pps_holdover < 0 || body.pps_holdover > 600) {
      throw new Error('PPS holdover: 0 to 600 s');
    }
    if (!Number.isInteger(body.pps_lost_serve_min) || body.pps_lost_serve_min < 0 || body.pps_lost_serve_min > 1440) {
      throw new Error('Keep answering after the PPS loss: 0 to 1440 min');
    }
    if (!Number.isInteger(body.pps_lost_stratum) || body.pps_lost_stratum < 2 || body.pps_lost_stratum > 16) {
      throw new Error('Stratum after the holdover: 2 to 16');
    }
    if (!Number.isInteger(body.mqtt_report_period) || body.mqtt_report_period < 30 || body.mqtt_report_period > 86400) {
      throw new Error('MQTT reporting period: 30 to 86400 s');
    }
    if (!Number.isInteger(body.mqtt_tf_client_limit) || body.mqtt_tf_client_limit < 50 || body.mqtt_tf_client_limit > 1000) {
      throw new Error('MQTT client limit with the TF card: 50 to 1000');
    }
    // MAC: lower case with ':'; the hardware MAC is sent as "" (= hardware MAC)
    const mac = normaliseMac(f.mac_address.value);
    if (mac === null) throw new Error('MAC address: xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx or xxxxxxxxxxxx (hexadecimal) expected');
    if (mac !== '' && (parseInt(mac.slice(0, 2), 16) & 1) !== 0) throw new Error('MAC address: a unicast address is required (the lowest bit of the first byte must be 0)');
    body.mac_address = mac === state.status.mac_hardware ? '' : mac;
    body.dns = f.dns.value.split(/\r?\n/).map((s) => s.trim()).filter((s) => s !== '');
    if (body.dns.length > 3) throw new Error('At most 3 DNS servers');
    return body;
  }

  // ---- Status ----
  function yesNo(el, ok, good, bad) {
    el.textContent = ok ? good : bad;
    el.className = ok ? 'good' : 'bad';
  }

  function duration(seconds) {
    const d = Math.floor(seconds / 86400);
    const h = Math.floor(seconds % 86400 / 3600);
    const m = Math.floor(seconds % 3600 / 60);
    const s = seconds % 60;
    const pad = (n) => String(n).padStart(2, '0');
    return (d > 0 ? d + ' d ' : '') + pad(h) + ':' + pad(m) + ':' + pad(s);
  }

  // IPv6 addresses in the recommended lower case form (RFC 5952); lwIP writes upper case
  function addressText(text) {
    return String(text).indexOf(':') >= 0 ? String(text).toLowerCase() : String(text);
  }

  // One entry per line; addresses as "address (kind)" get the kind in a second line
  function fillList(id, items, empty, addresses) {
    const list = $(id);
    list.replaceChildren();
    if (!items || items.length === 0) {
      list.textContent = empty;
      return;
    }
    for (const item of items) {
      const li = document.createElement('li');
      if (addresses) {
        const m = /^(\S+) \((.+)\)$/.exec(item);
        const addr = document.createElement('span');
        addr.className = 'addr';
        addr.textContent = addressText(m ? m[1] : item);
        li.appendChild(addr);
        if (m) {
          const kind = document.createElement('span');
          kind.className = 'addr-kind';
          kind.textContent = m[2];
          li.appendChild(kind);
        }
      } else {
        li.textContent = item;
      }
      list.appendChild(li);
    }
  }

  function renderStatus() {
    const st = state.status;
    setHost(state.settings.fqdn);
    yesNo($('st-time'), st.stratum_one, st.pps_holdover ? 'stratum 1, PPS holdover (PPS lost)' : 'stratum 1, GNSS and PPS locked',
      st.time_set ? 'not stratum 1 (PPS ' + (st.pps_active ? 'active' : 'missing') + ', GNSS ' + (st.gnss_locked ? 'locked' : 'not locked') + ')' : 'waiting for the GNSS time');
    $('st-utc').textContent = st.utc || 'not set yet';
    const gnss = [st.ublox_receiver ? 'U-Blox UBX protocol' : 'NMEA standard protocol'];
    if (st.gnss_baud_in_use) gnss.push(st.gnss_baud_in_use + ' baud');
    if (st.ublox_receiver) gnss.push(st.gnss_dynamic_model ? st.gnss_dynamic_model + ' mode' : 'dynamic model not known');
    fillList('st-gnss', gnss, '', false);
    $('st-auth').textContent = st.ntp_auth_available
      ? 'on, ' + st.ntp_auth_keys_in_use + (st.ntp_auth_keys_in_use === 1 ? ' key' : ' keys') + (state.settings.ntp_key_override ? ' of the web settings' : ' compiled in')
      : (state.settings.ntp_auth_enabled ? 'on, but no usable key' : 'off');
    $('st-firmware').textContent = st.firmware + ' (' + st.build + ')';
    $('st-chip').textContent = 'ESP32-P4 ' + st.chip_revision;
    $('ptp-chip').textContent = st.chip_revision;
    $('st-uptime').textContent = duration(st.uptime_s);
    $('st-heap').textContent = Math.round(st.free_heap / 1024) + ' kB';
    $('st-mqtt').textContent = state.settings.mqtt_enabled
      ? (st.mqtt_connected ? 'connected' : 'not connected') + (st.mqtt_tf_queue ? ', reports queued on the TF card' : '')
      : 'off';
    $('st-mac').textContent = st.mac_address + (st.mac_address === st.mac_hardware ? ' (hardware MAC)' : ' (set, hardware MAC ' + st.mac_hardware + ')');
    $('mac-hardware').textContent = st.mac_hardware;
    $('mac-in-use').textContent = st.mac_address;
    $('st-fingerprint').textContent = state.fingerprint;
    // Same form as the IPv6 lines: address in the first line, gateway and source in the second
    fillList('st-ipv4', st.ipv4_address
      ? [st.ipv4_address + '/' + st.ipv4_prefix + ' (via ' + (st.ipv4_gateway || '-') + ', ' + (st.ipv4_dhcp_running ? 'DHCP' : 'static') + ')']
      : [], st.ipv4_dhcp_running ? 'waiting for DHCP' : 'none', true);
    fillList('st-ipv6', st.ipv6, 'none', true);
    fillList('st-dns', st.dns, 'none', true);
    fillList('st-dns-v4', st.dns_received_v4, 'none', true);
    fillList('st-dns-v6', st.dns_received_v6, 'none', true);
    $('st-client').textContent = addressText(st.client);

    $('banner-default-pw').hidden = !state.default_password;
    $('banner-oneshot').hidden = !state.oneshot_boot;
    $('banner-unsaved').hidden = state.oneshot_boot || !state.unsaved_changes;
    renderPending(state.network_pending);
    renderOte();
    updateDependent();
  }

  function renderPending(p) {
    const banner = $('banner-network');
    banner.hidden = !p || !p.active;
    clearInterval(countdownTimer);
    if (!p || !p.active) return;
    pendingDeadline = Date.now() + p.seconds_left * 1000;
    const urls = $('net-urls');
    urls.replaceChildren();
    for (const u of p.urls || []) {
      const li = document.createElement('li');
      const a = document.createElement('a');
      a.href = u;
      a.textContent = u;
      a.rel = 'noreferrer';
      li.appendChild(a);
      urls.appendChild(li);
    }
    $('net-urls-text').hidden = !(p.urls && p.urls.length);
    const tick = () => {
      const left = Math.max(0, Math.round((pendingDeadline - Date.now()) / 1000));
      $('net-seconds').textContent = String(left);
    };
    tick();
    countdownTimer = setInterval(tick, 1000);
  }

  async function loadState(fill) {
    const r = await api('/api/state');
    if (r.status !== 200) return false;
    state = r.data;
    csrf = state.csrf;
    $('login-card').hidden = true;
    $('app').hidden = false;
    $('logout').hidden = false;
    renderStatus();
    if (fill) {
      fillForm(state.settings);
      $('allow-list').value = (state.allow_list || []).join('\n');
    }
    return true;
  }

  function startRefresh() {
    clearInterval(refreshTimer);
    refreshTimer = setInterval(() => { if (state) loadState(false); }, 10000);
  }

  function stopAfterRestart(text) {
    clearInterval(refreshTimer);
    clearInterval(countdownTimer);
    show(text, 'warn');
  }

  function setBusy(busy) {
    document.querySelectorAll('button').forEach((b) => { if (b.id !== 'theme-toggle') b.disabled = busy; });
    if (!busy && state) renderOte(); // the upload button stays off while OTE updates are off
  }

  // ---- Actions ----
  $('login-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    setBusy(true);
    const r = await api('/api/login', { password: $('login-password').value });
    setBusy(false);
    $('login-password').value = '';
    syncPasswordEyes();
    if (r.status === 200) {
      csrf = r.data.csrf;
      show('', '');
      if (await loadState(true)) startRefresh();
    } else if (r.status === 429) {
      show('Too many failed logins. Try again in ' + r.data.retry_after + ' s.', 'error');
    } else {
      show(r.data.error || 'Login failed', 'error');
    }
  });

  $('logout').addEventListener('click', async () => {
    await api('/api/logout', {});
    show('Logged out.', 'ok');
    showLogin();
  });

  form.addEventListener('input', () => { updateDependent(); markDefaults(); });
  form.addEventListener('change', () => { updateDependent(); markDefaults(); });
  // The OTE settings sit in the Firmware Update card, outside the settings form (form="settings-form"): their events
  // do not bubble through the form
  $('ote-card').addEventListener('input', () => { updateDependent(); markDefaults(); });
  $('ote-card').addEventListener('change', () => { updateDependent(); markDefaults(); });

  async function submitSettings(path) {
    let body;
    try {
      body = collect();
    } catch (err) {
      show(err.message, 'error');
      return;
    }
    setBusy(true);
    const r = await api(path, body);
    setBusy(false);
    if (r.status !== 200) {
      show(r.data.error || 'Request failed (' + r.status + ')', 'error');
      return;
    }
    if (path === '/api/apply' && r.data.restart) {
      stopAfterRestart('Applied. The device restarts once with these settings' +
        (r.data.network_changed ? '; confirm the network settings within 120 s after the start' : '') +
        '. Reload this page in about a minute.');
      return;
    }
    if (path === '/api/apply') {
      show(r.data.network_changed ? 'Applied. Confirm the network settings within 120 s.' : 'Applied (not saved).', 'ok');
      if (r.data.network_pending) renderPending(r.data.network_pending);
    } else {
      show(r.data.message || 'Saved.', 'ok');
    }
    await loadState(true);
  }

  $('btn-apply').addEventListener('click', () => submitSettings('/api/apply'));
  $('btn-save').addEventListener('click', () => submitSettings('/api/save'));

  $('btn-reboot').addEventListener('click', async () => {
    if (!window.confirm('Restart the device now? Applied settings that were not saved are discarded.')) return;
    setBusy(true);
    const r = await api('/api/reboot', {});
    setBusy(false);
    if (r.status === 200) stopAfterRestart(r.data.message + ' Reload this page in about a minute.');
    else show(r.data.error || 'Restart failed', 'error');
  });

  $('net-confirm').addEventListener('click', async () => {
    const r = await api('/api/network/confirm', {});
    if (r.status === 200) {
      show(r.data.message, 'ok');
      await loadState(false);
    } else {
      show(r.data.error || 'Confirmation failed', 'error');
    }
  });

  $('btn-factory').addEventListener('click', () => {
    $('factory-card').hidden = false;
    $('factory-card').scrollIntoView({ block: 'center', behavior: 'smooth' });
    $('factory-password').focus({ preventScroll: true });
  });
  $('factory-cancel').addEventListener('click', () => {
    $('factory-card').hidden = true;
    $('factory-password').value = '';
    syncPasswordEyes();
  });
  $('factory-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    if (!window.confirm('Erase all saved settings, the password and the allow-list, and restart?')) return;
    setBusy(true);
    const r = await api('/api/factory-reset', { password: $('factory-password').value });
    setBusy(false);
    $('factory-password').value = '';
    syncPasswordEyes();
    if (r.status === 200) {
      $('factory-card').hidden = true;
      stopAfterRestart(r.data.message + ' Log in with the compiled password after the restart.');
    } else {
      show(r.data.error || 'Factory reset failed', 'error');
    }
  });

  $('password-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const current = $('pw-current').value;
    const next = $('pw-new').value;
    if (next !== $('pw-repeat').value) {
      show('The two new passwords differ.', 'error');
      return;
    }
    if (next.length < 12 || next.length > 128) {
      show('The new password needs 12 to 128 characters.', 'error');
      return;
    }
    setBusy(true);
    const r = await api('/api/password', { current: current, new: next });
    setBusy(false);
    $('pw-current').value = '';
    $('pw-new').value = '';
    $('pw-repeat').value = '';
    syncPasswordEyes();
    if (r.status === 200) {
      show(r.data.message, 'ok');
      await loadState(false);
    } else {
      show(r.data.error || 'Password change failed', 'error');
    }
  });

  $('allow-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const entries = $('allow-list').value.split(/\r?\n/).map((s) => s.trim()).filter((s) => s !== '');
    setBusy(true);
    const r = await api('/api/allow-list', { entries: entries });
    setBusy(false);
    show(r.status === 200 ? r.data.message : (r.data.error || 'Allow-list could not be stored'), r.status === 200 ? 'ok' : 'error');
    if (r.status === 200) await loadState(false);
  });

  // ---- Firmware update (Over The Ethernet) ----
  let oteUploading = false;

  // ote_enabled of the settings in use (a restart field: the value of this boot)
  function renderOte() {
    const on = !!state.settings.ote_enabled;
    $('ote-off').hidden = on;
    $('ote-chip').textContent = 'ESP32-P4 ' + state.status.chip_revision;
    if (!oteUploading) {
      $('ote-file').disabled = !on;
      $('ote-upload').disabled = !on;
    }
  }

  // The first bytes of an application image: image magic 0xE9, application description (magic 0xABCD5432) after
  // the image header (24 bytes) and the first segment header (8 bytes), its version text at offset 48
  async function imageVersion(file) {
    const bytes = new Uint8Array(await file.slice(0, 112).arrayBuffer());
    const view = new DataView(bytes.buffer);
    if (bytes.length < 112 || bytes[0] !== 0xE9 || view.getUint32(32, true) !== 0xABCD5432) return null;
    const text = (from, length) => new TextDecoder().decode(bytes.subarray(from, from + length)).replace(/\0.*$/s, '');
    return { version: text(48, 32), project: text(80, 32) };
  }

  $('ote-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const file = $('ote-file').files[0];
    if (!file) {
      show('Choose the firmware image (.bin) first.', 'error');
      return;
    }
    const image = await imageVersion(file);
    if (!image) {
      show('This is not an application image: use ESP32TimeServer.bin of the build folder (not the merged flash image).', 'error');
      return;
    }
    if (!window.confirm('Upload ' + image.project + ' ' + image.version + ' (' + file.name + ', ' + Math.round(file.size / 1024) +
      ' kB) and restart the device with it? Running: ' + state.status.firmware + '.')) return;

    oteUploading = true;
    clearInterval(refreshTimer); // the server answers nothing else during the upload
    setBusy(true);
    $('ote-file').disabled = true;
    const progress = $('ote-progress');
    progress.value = 0;
    progress.hidden = false;
    $('ote-state').textContent = 'Uploading …';

    const finish = (status, data) => {
      oteUploading = false;
      setBusy(false);
      if (status === 200) {
        progress.value = 100;
        $('ote-state').textContent = data.message || '';
        $('ote-file').value = '';
        stopAfterRestart((data.message || 'Firmware accepted.') + ' Reload this page in about a minute.');
        return;
      }
      progress.hidden = true;
      $('ote-state').textContent = '';
      if (status === 401) {
        showLogin();
        return;
      }
      show(data.error || 'Upload failed (' + status + ')', 'error');
      startRefresh();
    };
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ote');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.setRequestHeader('X-CSRF-Token', csrf);
    xhr.upload.onprogress = (ev) => {
      if (!ev.lengthComputable) return;
      progress.value = Math.round((ev.loaded * 100) / ev.total);
      $('ote-state').textContent = ev.loaded < ev.total ? 'Uploading … ' + progress.value + ' %' : 'Checking and writing the image …';
    };
    xhr.onload = () => {
      let data = {};
      try { data = JSON.parse(xhr.responseText); } catch (err) { data = {}; }
      finish(xhr.status, data);
    };
    // The device closes the connection when it refuses the image before the whole file is sent (wrong project
    // or chip revision): the browser then only sees an interrupted upload
    xhr.onerror = () => finish(0, { error: 'The upload was interrupted. The device may have refused the image (the reason is on the LCD and the serial console), or it does not answer.' });
    xhr.send(file);
  });

  // ---- Start ----
  buildColourTable();
  loadState(true).then((ok) => { if (ok) startRefresh(); else showLogin(); });
})();
