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
  let diagTimer = 0; // Diagnostics panel: refreshed while it is open
  let ptpTimer = 0; // PTP panel: its state is refreshed while it is open
  let consoleTimer = 0; // GNSS console: new lines are fetched while it is open
  let formDirty = false; // the settings form was edited since it was filled

  // ---- Language: English unless German was chosen with the button (cookie "lang"; German texts in i18n.js) ----
  const i18n = window.I18N || { lang: 'en', de: {}, server: [] };
  const lang = i18n.lang === 'de' ? 'de' : 'en';
  const missing = new Set(); // German: English texts without a translation (window.I18N.missing, for checks)
  i18n.missing = missing;
  const squash = (text) => String(text).replace(/\s+/g, ' ').trim();
  const own = (object, key) => Object.prototype.hasOwnProperty.call(object, key);
  function lookup(text) {
    if (lang !== 'de') return null;
    const key = squash(text);
    if (own(i18n.de, key)) return i18n.de[key];
    if (/[A-Za-z]{2}/.test(key)) missing.add(key);
    return null;
  }
  // Text in the selected language; {name} is replaced by values.name (values are inserted as they are)
  function t(text, values) {
    const de = lookup(text);
    let out = de === null ? text : de;
    if (values) out = out.replace(/\{(\w+)\}/g, (all, name) => (own(values, name) ? String(values[name]) : all));
    return out;
  }
  // Messages of the device: fixed texts, or printf formats of the firmware whose values are kept ({1}, {2} in German)
  const serverFormats = (i18n.server || []).map(([english, german]) => {
    const parts = english.split(/%[-+ 0-9.]*(?:ll|l|z|h)?[sdufxX]/);
    const pattern = parts.map((p) => p.replace(/%%/g, '%').replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('(.+?)');
    return [new RegExp('^' + pattern + '$'), german];
  });
  function ts(text) {
    if (lang !== 'de' || !text) return text;
    const key = squash(text);
    if (own(i18n.de, key)) return i18n.de[key];
    for (const [pattern, german] of serverFormats) {
      const m = pattern.exec(key);
      if (m) return german.replace(/\{(\d)\}/g, (all, i) => m[Number(i)] || '');
    }
    missing.add(key);
    return text;
  }

  // Static texts of the page. An element with text of its own and no form controls inside is translated as a whole,
  // its inline markup included (the German HTML comes from i18n.js, never from the server); loose text next to form
  // controls one text node at a time; title, placeholder and aria-label attributes
  const CONTAINERS = 'input,select,textarea,button,progress,ul,ol';
  // German text for source, or null when there is none or it is the same (the source then keeps its white space)
  const german = (source) => {
    const de = lookup(source);
    return de === null || de === squash(source) ? null : de;
  };
  function translateElement(el) {
    if (el.matches('script,style')) return;
    for (const name of ['title', 'placeholder', 'aria-label']) {
      if (!el.hasAttribute(name)) continue;
      const de = german(el.getAttribute(name));
      if (de !== null) el.setAttribute(name, de);
    }
    const ownText = [...el.childNodes].some((n) => n.nodeType === Node.TEXT_NODE && n.nodeValue.trim() !== '');
    if (ownText && !el.querySelector(CONTAINERS)) {
      const html = el.children.length > 0;
      const de = german(html ? el.innerHTML : el.textContent);
      if (de !== null) {
        if (html) el.innerHTML = de;
        else el.textContent = de;
      }
      return;
    }
    for (const node of [...el.childNodes]) {
      if (node.nodeType === Node.TEXT_NODE && node.nodeValue.trim() !== '') {
        const de = german(node.nodeValue);
        if (de !== null) {
          const m = /^(\s*)[\s\S]*?(\s*)$/.exec(node.nodeValue);
          node.nodeValue = m[1] + de + m[2];
        }
      } else if (node.nodeType === Node.ELEMENT_NODE) {
        translateElement(node);
      }
    }
  }
  if (lang === 'de') translateElement(document.body);
  document.documentElement.classList.remove('i18n-pending');

  // Language button: shows the language in use (flag); switching stores the choice in a cookie and reloads the page
  $('lang-flag').className = 'flag flag-' + (lang === 'de' ? 'de' : 'gb');
  {
    const label = t('Language: English (click for German)');
    $('lang-toggle').title = label;
    $('lang-toggle').setAttribute('aria-label', label);
  }
  $('lang-toggle').addEventListener('click', () => {
    if (formDirty && !window.confirm(t('Switching the language reloads the page: changes that were not applied are lost. Continue?'))) return;
    const next = lang === 'de' ? 'en' : 'de';
    document.cookie = 'lang=' + next + '; Path=/; Max-Age=31536000; SameSite=Strict' + (location.protocol === 'https:' ? '; Secure' : '');
    location.reload();
  });

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
    const label = theme === 'dark' ? t('Dark mode (click for light mode)') : t('Light mode (click for dark mode)');
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
  // First line: the FQDN when it has a domain, otherwise the host name; the tab title keeps the short host name
  function setHost(fqdn) {
    const name = String(fqdn || '').replace(/\.$/, '');
    const host = name.split('.')[0] || 'ESP32 Time Server';
    $('hdr-host').textContent = name.indexOf('.') > 0 ? name : host;
    $('hdr-sub').textContent = 'ESP32 Time Server';
    document.title = host + ' – ESP32 Time Server';
  }

  // ---- Messages ----
  function show(text, kind) {
    const m = $('message');
    m.textContent = text;
    m.className = 'banner ' + (kind || 'ok');
    m.hidden = !text;
    // To the very top: the header is sticky, so scrolling the banner into view would leave it under the header
    if (text) window.scrollTo({ top: 0, behavior: 'smooth' });
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
      return { status: 0, data: { error: t('The device does not answer (restarting or a different address?)') } };
    }
    let data = {};
    try { data = await response.json(); } catch (e) { data = {}; }
    // A refused login (both steps) is no expired session: loginDone() shows it
    if (response.status === 401 && path !== '/api/login' && path !== '/api/login/totp') showLogin();
    return { status: response.status, data: data };
  }

  function showLogin() {
    csrf = '';
    state = null;
    clearInterval(refreshTimer);
    clearInterval(countdownTimer);
    clearInterval(diagTimer);
    clearInterval(ptpTimer);
    clearInterval(consoleTimer);
    $('app').hidden = true;
    $('logout').hidden = true;
    $('hdr-user').hidden = true;
    for (const id of ['banner-default-pw', 'banner-network', 'banner-oneshot', 'banner-unsaved']) $(id).hidden = true;
    $('login-card').hidden = false;
    $('totp-card').hidden = true;
    $('recovery-card').hidden = true;
    $('login-password').focus();
    api('/api/info').then((r) => {
      if (r.status !== 200) return;
      setHost(r.data.fqdn || r.data.hostname);
      // RADIUS logins on: a user name (empty = the local user admin); off: the hidden admin field for password managers
      $('login-user-row').hidden = !r.data.login_user;
      $('login-user-fixed').disabled = !!r.data.login_user;
      if (r.data.login_user) $('login-user').focus();
    });
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
      label.textContent = t(name);
      const small = document.createElement('small');
      small.textContent = t(meaning);
      label.appendChild(small);

      const select = document.createElement('select');
      select.setAttribute('aria-label', t('{name} colour', { name: t(name) }));
      for (const [hex, presetName] of presets) {
        const o = document.createElement('option');
        o.value = hex;
        o.textContent = t(presetName);
        select.appendChild(o);
      }
      const custom = document.createElement('option');
      custom.value = 'custom';
      custom.textContent = t('Custom');
      select.appendChild(custom);

      const input = document.createElement('input');
      input.type = 'text';
      input.maxLength = 7;
      input.placeholder = '#RRGGBB';
      input.spellcheck = false;
      input.setAttribute('aria-label', t('{name} colour as #RRGGBB', { name: t(name) }));

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
    entry.button.title = shown ? t('Hide the text') : t('Show the text');
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
    button.setAttribute('aria-label', t('Show or hide the text'));
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

  // ---- Key tables: NTP keys and PTP keys (ID 1 to 65535, 32 letters or digits) ----

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

  // A table of keys in the element tableId with its add button and state line; at most max keys
  function makeKeyTable(tableId, addId, stateId, max) {
    const kt = { rows: [], changed: false };
    const updateAdd = () => { $(addId).disabled = kt.rows.length >= max; };
    function check(row) {
      row.key.classList.toggle('bad', !/^[0-9A-Za-z]{32}$/.test(row.key.value.trim()));
      const id = Number(row.id.value);
      row.id.classList.toggle('bad', !Number.isInteger(id) || id < 1 || id > 65535 ||
        kt.rows.some((other) => other !== row && Number(other.id.value) === id));
    }
    function changed() {
      kt.changed = true;
      kt.rows.forEach(check);
      updateAdd();
    }
    function add(id, key) {
      const table = $(tableId);
      const idInput = document.createElement('input');
      idInput.type = 'number';
      idInput.min = '1';
      idInput.max = '65535';
      idInput.value = String(id);
      idInput.setAttribute('aria-label', t('Key ID'));
      const keyInput = document.createElement('input');
      keyInput.type = 'text';
      keyInput.maxLength = 32;
      keyInput.spellcheck = false;
      keyInput.autocomplete = 'off';
      keyInput.value = key;
      keyInput.setAttribute('aria-label', t('Key'));
      const generate = document.createElement('button');
      generate.type = 'button';
      generate.textContent = t('Generate');
      generate.title = t('New random key (32 letters or digits)');
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.textContent = t('Remove');
      const row = { id: idInput, key: keyInput, elements: [idInput, keyInput, generate, remove] };
      generate.addEventListener('click', () => { keyInput.value = randomKey(); changed(); });
      remove.addEventListener('click', () => {
        row.elements.forEach((el) => el.remove());
        kt.rows = kt.rows.filter((r) => r !== row);
        changed();
      });
      idInput.addEventListener('input', changed);
      keyInput.addEventListener('input', changed);
      table.append(idInput, keyInput, generate, remove);
      kt.rows.push(row);
      check(row);
    }
    kt.set = (keys) => {
      kt.rows.forEach((row) => row.elements.forEach((el) => el.remove()));
      kt.rows = [];
      for (const k of keys || []) add(k.id, k.key);
      kt.changed = false;
      $(stateId).textContent = kt.rows.length ? t('Keys set on this page are in use.') : t('No keys are set yet.');
      updateAdd();
    };
    kt.updateAdd = updateAdd;
    // The keys for the request; idMessage / keyMessage: texts with {n} for a wrong key ID / key
    kt.collect = (idMessage, keyMessage) => kt.rows.map((row, i) => {
      const id = Number(row.id.value);
      const key = row.key.value.trim();
      if (!Number.isInteger(id) || id < 1 || id > 65535) throw fieldError(tableId, t(idMessage, { n: i + 1 }));
      if (!/^[0-9A-Za-z]{32}$/.test(key)) throw fieldError(tableId, t(keyMessage, { n: i + 1 }));
      return { id: id, key: key };
    });
    $(addId).addEventListener('click', () => {
      const used = kt.rows.map((r) => Number(r.id.value));
      let id = 1;
      while (used.includes(id)) id++;
      add(id, randomKey());
      changed();
    });
    return kt;
  }
  const ntpKeys = makeKeyTable('ntp-keys', 'ntp-key-add', 'ntp-key-state', 8);
  const ptpKeys = makeKeyTable('ptp-keys', 'ptp-key-add', 'ptp-key-state', 4);

  // ---- Settings <-> form ----
  function formValues(s) {
    // the form view of a settings object (also used for the defaults)
    return {
      fqdn: s.fqdn,
      ip_mode: s.ip_mode,
      ipv4_dhcp: s.ipv4_dhcp,
      ipv4_cidr: s.ipv4_address ? s.ipv4_address + '/' + s.ipv4_prefix : '',
      ipv4_gateway: s.ipv4_gateway,
      ipv6_slaac: s.ipv6_slaac,
      ipv6_cidr: s.ipv6_address ? s.ipv6_address + '/' + s.ipv6_prefix : '',
      ipv6_gateway: s.ipv6_gateway,
      dns: (s.dns || []).join('\n'),
      // "" = hardware MAC: the field stays empty and shows the hardware MAC as its (grey) placeholder
      mac_address: s.mac_address || '',
      ntp_client_rate_limit: String(s.ntp_client_rate_limit),
      gnss_protocol: s.gnss_protocol,
      gnss_tx_pin: String(s.gnss_tx_pin),
      gnss_rx_pin: String(s.gnss_rx_pin),
      gnss_pps_pin: String(s.gnss_pps_pin),
      gnss_baud: String(s.gnss_baud),
      ptp_loopback_pin: s.ptp_loopback_pin < 0 ? '' : String(s.ptp_loopback_pin),
      gnss_min_fix_age: String(s.gnss_min_fix_age),
      gnss_min_satellites: String(s.gnss_min_satellites),
      pps_holdover: String(s.pps_holdover),
      pps_lost_serve: s.pps_lost_serve,
      pps_lost_serve_min: String(s.pps_lost_serve_min),
      pps_lost_stratum: String(s.pps_lost_stratum),
      // ppm (us per second) as ms per second
      pps_dispersion: String(s.pps_dispersion_ppm / 1000),
      lcd_enabled: s.lcd_enabled,
      lcd_title: s.lcd_title,
      lcd_24_hour: s.lcd_24_hour,
      lcd_pad_hour: s.lcd_pad_hour,
      lcd_show_time_zone: s.lcd_show_time_zone,
      lcd_address: s.lcd_address,
      button_enabled: s.button_enabled,
      button_pin: String(s.button_pin),
      time_zone: s.time_zone,
      led_type: s.led_type,
      led_brightness: String(s.led_brightness),
      led_red_pin: String(s.led_red_pin),
      led_green_pin: String(s.led_green_pin),
      led_blue_pin: String(s.led_blue_pin),
      led_ws2812_pin: String(s.led_ws2812_pin),
      mqtt_enabled: s.mqtt_enabled,
      mqtt_broker: s.mqtt_broker || '',
      mqtt_port: s.mqtt_port ? String(s.mqtt_port) : '',
      mqtt_tls: s.mqtt_tls,
      mqtt_ca_pem: s.mqtt_ca_pem || '',
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
      ntp_auth_required: s.ntp_auth_required,
      ote_enabled: s.ote_enabled,
      ptp_l2_enabled: s.ptp_l2_enabled,
      ptp_udp4_enabled: s.ptp_udp4_enabled,
      ptp_udp6_enabled: s.ptp_udp6_enabled,
      ptp_master_selection: s.ptp_master_selection,
      ptp_auth_enabled: s.ptp_auth_enabled,
      ptp_auth_required: s.ptp_auth_required,
      ptp_auth_mutable: s.ptp_auth_mutable,
      ...Object.fromEntries(ptpNumbers.map(([name]) => [name, String(s[name])])),
      ...loginValues(s)
    };
  }

  // Logins (Security): the RADIUS servers (the shared secrets are never sent: their fields stay empty) and the methods
  function loginValues(s) {
    const radius = s.radius || {};
    const servers = radius.servers || [];
    const web = s.web_login || {};
    const values = {};
    for (const n of [1, 2]) {
      const server = servers[n - 1] || {};
      values['r' + n + '_host'] = server.host || '';
      values['r' + n + '_port'] = String(server.port || 1812);
      values['r' + n + '_secret'] = '';
    }
    return Object.assign(values, {
      r_timeout: String(radius.timeout_s), r_retries: String(radius.retries), r_deadtime: String(radius.dead_time_min),
      r_nas_id: radius.nas_identifier || '', r_nas_ip: radius.nas_ip || '', r_nas_ip6: radius.nas_ipv6 || '',
      r_service: String(radius.service_type), r_port_type: String(radius.nas_port_type), r_priv: String(radius.min_privilege),
      r_required: radius.required || '', r_require_ma: radius.require_message_authenticator !== false,
      web_local: web.local !== false, web_radius: !!web.radius, web_fallback: web.local_fallback !== false,
      totp_local: !!web.totp_local, totp_radius: !!web.totp_radius, totp_enroll: !!web.totp_enroll
    });
  }

  // PTP settings with a number (inputs and selects): name, lowest, highest, label of the error message
  const ptpNumbers = [
    ['ptp_domain', 0, 127, 'PTP domain: 0 to 127'],
    ['ptp_priority1', 0, 255, 'PTP priority 1: 0 to 255'],
    ['ptp_priority2', 0, 255, 'PTP priority 2: 0 to 255'],
    ['ptp_clock_accuracy', 32, 254, 'PTP clock accuracy: choose a value'],
    ['ptp_utc_offset', 0, 100, 'PTP TAI − UTC: 0 to 100 s'],
    ['ptp_log_sync_interval', -3, 4, 'PTP sync interval: choose a value'],
    ['ptp_log_announce_interval', 0, 4, 'PTP announce interval: choose a value'],
    ['ptp_log_min_delay_req_interval', -3, 6, 'PTP minimum delay request interval: choose a value'],
    ['ptp_follower_timeout', 5, 3600, 'PTP follower timeout: 5 to 3600 s'],
    ['ptp_delay_req_rate_limit', 0, 1000, 'PTP delay requests per follower: 0 to 1000 per second'],
    ['ptp_ingress_latency_ns', -100000, 100000, 'PTP ingress latency: -100000 to 100000 ns'],
    ['ptp_egress_latency_ns', -100000, 100000, 'PTP egress latency: -100000 to 100000 ns'],
    ['ptp_auth_spp', 0, 255, 'PTP security parameter pointer: 0 to 255'],
    ['ptp_auth_active_key_id', 1, 65535, 'PTP active key ID: 1 to 65535']
  ];
  // Text of a selected option (the defaults of the selects)
  function optionText(name, value) {
    const option = Array.from(f[name].options).find((o) => o.value === String(value));
    return option ? option.textContent : String(value);
  }

  const displayNames = {
    ip_mode: { dual: 'IPv4 and IPv6', ipv4: 'IPv4 only', ipv6: 'IPv6 only' },
    lcd_address: { both: 'IPv4 and IPv6 address in turn', ipv4: 'IPv4 address', ipv6: 'IPv6 address',
      status: 'Status of IPv4 and IPv6 (IPv4: OK/N/A, IPv6: GUA/ULA/LLA/N/A)', none: 'Empty (privacy)' },
    gnss_protocol: { nmea: 'NMEA Standard Protocol (needs RMC sentence)', ubx: 'U-Blox UBX Protocol' },
    led_type: { none: 'None', ky016: 'KY-016', ws2812: 'WS2812' },
    mqtt_qos: { 0: '0 (at most once)', 1: '1 (at least once)', 2: '2 (exactly once)' },
    r_service: { 1: 'Login (1)', 6: 'Administrative (6)', 7: 'NAS-Prompt (7)', 0: 'not sent' },
    r_port_type: { 5: 'Virtual (5)', 0: 'Async (0)', '-1': 'not sent' }
  };

  // The shared secret fields: whether one is stored (the page never gets it)
  function renderRadiusSecrets() {
    const servers = (state && state.settings.radius && state.settings.radius.servers) || [];
    for (const n of [1, 2]) {
      const set = !!(servers[n - 1] || {}).secret_set;
      f['r' + n + '_secret'].placeholder = set ? t('set – empty keeps it') : t('not set');
    }
  }

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
    ntpKeys.set(s.ntp_keys);
    ptpKeys.set(s.ptp_keys);
    $('mqtt-password-state').textContent = s.mqtt_password_set ? t('A password is set.') : t('No password is set.');
    renderRadiusSecrets();
    updateDependent();
    markDefaults();
    formDirty = false;
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
      let text = value === '' || value === undefined ? t('(empty)') : String(value);
      if (typeof value === 'boolean') text = value ? t('on') : t('off');
      if (name === 'dns') text = value ? value.replace(/\n/g, ', ') : t('(empty: received servers)');
      if (name === 'ptp_loopback_pin' && value === '') text = t('off');
      if (name === 'led_brightness') text += ' %';
      if (name === 'mqtt_report_period') text += ' s';
      if (name === 'mqtt_port' && value === '') text = t('automatic (8883 with TLS, 1883 without)');
      if (name === 'pps_holdover' || name === 'gnss_min_fix_age') text = value === '0' ? t('off') : text + ' s';
      if (name === 'gnss_min_satellites' && value === '0') text = t('off');
      if (name === 'pps_lost_serve_min') text = value === '0' ? t('no limit') : text + ' min';
      if (name === 'ntp_client_rate_limit') text = value === '0' ? t('off') : t('{n} per second', { n: value });
      if (name === 'pps_dispersion') text = (lang === 'de' ? text.replace('.', ',') : text) + ' ms/s';
      if (name === 'mac_address' && !state.defaults.mac_address) text = t('the hardware MAC');
      if (f[name] && f[name].tagName === 'SELECT' && name.startsWith('ptp_')) text = optionText(name, value);
      if (name === 'ptp_delay_req_rate_limit') text = value === '0' ? t('no limit') : t('{n} per second', { n: value });
      if (name === 'ptp_follower_timeout' || name === 'ptp_utc_offset') text += ' s';
      if (name === 'ptp_ingress_latency_ns' || name === 'ptp_egress_latency_ns') text += ' ns';
      if (displayNames[name] && displayNames[name][value]) text = t(displayNames[name][value]);
      small.textContent = t('Default: {value}', { value: text });
      // Decimal fields compare as numbers (0.10 = 0.1)
      const differs = name === 'pps_dispersion'
        ? Number(currentValue(name)) !== Number(value)
        : String(currentValue(name)).replace(/\r/g, '') !== String(value);
      small.classList.toggle('differs', differs);
      f[name].classList.toggle('is-changed', differs);
    });
    for (const [key] of ledStates) {
      const row = colourRows[key];
      const def = state.defaults.led_colors ? state.defaults.led_colors[key] : null;
      const differs = def !== null && normaliseHex(row.input.value) !== def;
      row.input.classList.toggle('is-changed', differs);
      row.input.title = t('Default: {value}', { value: def || '' });
    }
    markPinAreas();
    markPanels();
  }

  // Collapsed settings panels: the heading row shows how many visible values inside differ from the default
  // and whether a warning inside is shown, as the GPIO pin areas do (a note on the state of the device, such as
  // "updates are off", is no warning about a setting)
  function markPanels() {
    document.querySelectorAll('details.panel').forEach((panel) => {
      const visible = (el) => !el.closest('[hidden]');
      const changed = [...panel.querySelectorAll('.is-changed')].filter(visible).length;
      const warning = [...panel.querySelectorAll('small.note.bad:not(.device-state)')].some(visible);
      const parts = [];
      if (changed > 0) parts.push(changed === 1 ? t('1 value differs from the default') : t('{n} values differ from the default', { n: changed }));
      if (warning) parts.push(t('see the warning'));
      const marker = panel.querySelector('.panel-changed');
      marker.textContent = parts.join(', ');
      marker.hidden = parts.length === 0;
      marker.classList.toggle('bad', warning);
    });
  }

  // Opens the panel (and the GPIO pin area) of a form field (name) or element (id), e.g. the one an error names
  function revealField(name) {
    const field = $(name) || f[name];
    const el = field && (field.nodeType ? field : field[0]); // a radio group is a list
    if (!el) return;
    for (let d = el.closest('details'); d; d = d.parentElement.closest('details')) d.open = true;
  }

  // Collapsed GPIO pin areas: the summary shows how many visible pin values differ from the default and
  // whether a warning inside is shown, so nothing changed stays hidden
  function markPinAreas() {
    document.querySelectorAll('details.pins').forEach((area) => {
      const visible = (el) => !el.closest('[hidden]');
      const changed = [...area.querySelectorAll('.is-changed')].filter(visible).length;
      const warning = [...area.querySelectorAll('small.note.bad')].some(visible);
      const parts = [];
      if (changed > 0) parts.push(changed === 1 ? t('1 pin differs from the default') : t('{n} pins differ from the default', { n: changed }));
      if (warning) parts.push(t('see the warning'));
      const marker = area.querySelector('.pins-changed');
      marker.textContent = parts.join(', ');
      marker.hidden = parts.length === 0;
      marker.classList.toggle('bad', warning);
    });
  }

  function updateDependent() {
    const mode = f.ip_mode.value;
    document.querySelectorAll('.v4-field').forEach((el) => { el.hidden = mode === 'ipv6'; });
    document.querySelectorAll('.v6-field').forEach((el) => { el.hidden = mode === 'ipv4'; });
    const dhcp = f.ipv4_dhcp.checked;
    f.ipv4_cidr.disabled = dhcp;
    f.ipv4_gateway.disabled = dhcp;
    const ubx = f.gnss_protocol.value === 'ubx';
    f.gnss_baud.disabled = ubx;
    $('baud-note').hidden = !ubx;
    const type = f.led_type.value;
    document.querySelectorAll('.led-ky016').forEach((el) => { el.hidden = type !== 'ky016'; });
    document.querySelectorAll('.led-ws2812').forEach((el) => { el.hidden = type !== 'ws2812'; });
    $('led-pins-none').hidden = type !== 'none';
    $('brightness-out').textContent = f.led_brightness.value + ' %';
    f.mqtt_password.disabled = f.mqtt_password_clear.checked;
    f.pps_lost_serve_min.disabled = !f.pps_lost_serve.checked;
    f.pps_lost_stratum.disabled = !f.pps_lost_serve.checked;
    // The client limit only matters with the client list; the report buffer is sized from it (RAM, no PSRAM)
    f.mqtt_tf_client_limit.disabled = !f.mqtt_client_reporting.checked;
    const limit = Number(f.mqtt_tf_client_limit.value);
    const kilobytes = ((3874 + 76 * limit) / 1024).toFixed(1); // web_mqtt_report_size() of the firmware
    $('mqtt-buffer-note').textContent = f.mqtt_client_reporting.checked && Number.isInteger(limit) && limit >= 50 && limit <= 1000
      ? t('Report buffer: {kb} kB of RAM (3874 bytes plus 76 bytes per client). Without the TF card at most 50 clients are listed.', { kb: lang === 'de' ? kilobytes.replace('.', ',') : kilobytes })
      : (f.mqtt_client_reporting.checked ? t('50 to 1000.') : t('Only used with client activity reporting.'));
    ntpKeys.updateAdd();
    ptpKeys.updateAdd();
    f.button_pin.disabled = !f.button_enabled.checked;
    // The login methods of the page (Security)
    $('web-auth-error').hidden = f.web_local.checked || f.web_radius.checked;
    f.web_fallback.disabled = !(f.web_local.checked && f.web_radius.checked);
    f.totp_enroll.disabled = !f.totp_radius.checked;
    syncPasswordEyes();
    if (state) {
      $('ptp-chip-warning').hidden = state.status.ptp_loopback_supported || f.ptp_loopback_pin.value.trim() === '';
    }
    markPinAreas();
    markPanels();
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

  // An input error of a field: the message is shown and the panel of the field opened (revealField)
  // An IPv4 or IPv6 address or a DNS name (RFC 1123 labels; the last label is not all digits)
  function hostValid(host) {
    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(host)) return host.split('.').every((p) => Number(p) <= 255 && !/^0\d/.test(p));
    if (host.includes(':')) {
      if (!/^[0-9A-Fa-f:.]+$/.test(host) || host.length > 45) return false;
      try { new URL('http://[' + host + ']/'); return true; } catch (e) { return false; }
    }
    if (host.length > 253) return false;
    const labels = host.replace(/\.$/, '').split('.');
    return labels.every((l) => /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(l)) && !/^\d+$/.test(labels[labels.length - 1]);
  }

  // MQTT broker, port, topic and CA certificate (the checks of the firmware, web_settings_validate)
  function collectMqttBroker(body) {
    if (body.mqtt_ca_pem !== '') body.mqtt_ca_pem += '\n';
    const port = f.mqtt_port.value.trim();
    body.mqtt_port = port === '' ? 0 : Number(port);
    if (!Number.isInteger(body.mqtt_port) || body.mqtt_port < 0 || body.mqtt_port > 65535) throw fieldError('mqtt_port', t('MQTT port: 1 to 65535, or empty'));
    if (/\.local\.?$/i.test(body.mqtt_broker)) throw fieldError('mqtt_broker', t('MQTT broker: .local names (mDNS) are not supported; use an address or a DNS name'));
    if (body.mqtt_broker !== '' && !hostValid(body.mqtt_broker)) throw fieldError('mqtt_broker', t('MQTT broker: a host name or an IPv4 or IPv6 address expected'));
    if (body.mqtt_enabled && body.mqtt_broker === '') throw fieldError('mqtt_broker', t('MQTT broker: required while MQTT is on'));
    if (body.mqtt_topic === '' || /[+#\x00-\x1f\x7f]/.test(body.mqtt_topic) || body.mqtt_topic[0] === '$' || body.mqtt_topic.endsWith('/')) {
      throw fieldError('mqtt_topic', t('MQTT topic: 1 to 100 characters without +, #, control characters, a leading $ or a trailing /'));
    }
    if (body.mqtt_ca_pem !== '' && !/^(\s*-----BEGIN CERTIFICATE-----[A-Za-z0-9+/=\s]+-----END CERTIFICATE-----)+\s*$/.test(body.mqtt_ca_pem)) {
      throw fieldError('mqtt_ca_pem', t('MQTT CA certificate: PEM with one or more BEGIN CERTIFICATE / END CERTIFICATE blocks expected'));
    }
    if (body.mqtt_enabled && body.mqtt_tls && body.mqtt_ca_pem === '') throw fieldError('mqtt_ca_pem', t('MQTT: TLS needs the CA certificate of the broker (PEM)'));
  }

  // Connection state of the MQTT client under Status
  function renderMqtt(st) {
    const el = $('st-mqtt');
    if (!st.mqtt_active) {
      // On, but the client did not start (memory): the reason; otherwise off
      el.textContent = state.settings.mqtt_enabled && st.mqtt_error ? t('off: {error}', { error: ts(st.mqtt_error) }) : t('off');
      el.className = state.settings.mqtt_enabled && st.mqtt_error ? 'bad' : '';
      return;
    }
    const where = st.mqtt_broker + ':' + st.mqtt_port + (st.mqtt_tls ? ' (TLS)' : '');
    if (st.mqtt_connected) {
      el.textContent = t('connected to {where} for {time}; {n} reports published, the latest {age}', {
        where: where, time: duration(st.mqtt_connected_s), n: st.mqtt_published,
        age: st.mqtt_publish_age_s < 0 ? t('none yet') : t('{n} s ago', { n: st.mqtt_publish_age_s }) });
      el.className = 'good';
    } else {
      el.textContent = st.mqtt_error
        ? t('not connected to {where}: {error} ({age} s ago)', { where: where, error: ts(st.mqtt_error), age: st.mqtt_error_age_s })
        : t('connecting to {where} …', { where: where });
      el.className = st.mqtt_error ? 'bad' : '';
    }
    if (st.mqtt_tf_queue) el.textContent += '; ' + t('reports queued on the TF card');
  }

  function fieldError(field, text) {
    const err = new Error(text);
    err.field = field;
    return err;
  }

  function pin(name, label, allowEmpty) {
    const text = f[name].value.trim();
    if (text === '' && allowEmpty) return -1;
    const value = Number(text);
    if (text === '' || !Number.isInteger(value)) throw fieldError(name, t('{label}: GPIO number expected', { label: t(label) }));
    return value;
  }

  function collect() {
    const body = {
      fqdn: f.fqdn.value.trim(),
      ip_mode: f.ip_mode.value,
      ipv4_dhcp: f.ipv4_dhcp.checked,
      ipv6_slaac: f.ipv6_slaac.checked,
      ipv6_gateway: f.ipv6_gateway.value.trim(),
      ntp_client_rate_limit: Number(f.ntp_client_rate_limit.value),
      gnss_protocol: f.gnss_protocol.value,
      gnss_tx_pin: pin('gnss_tx_pin', 'GNSS TX pin'),
      gnss_rx_pin: pin('gnss_rx_pin', 'GNSS RX pin'),
      gnss_pps_pin: pin('gnss_pps_pin', 'PPS pin'),
      gnss_baud: Number(f.gnss_baud.value),
      ptp_loopback_pin: pin('ptp_loopback_pin', 'PTP PPS loopback pin', true),
      gnss_min_fix_age: Number(f.gnss_min_fix_age.value),
      gnss_min_satellites: Number(f.gnss_min_satellites.value),
      pps_holdover: Number(f.pps_holdover.value),
      pps_lost_serve: f.pps_lost_serve.checked,
      pps_lost_serve_min: Number(f.pps_lost_serve_min.value),
      pps_lost_stratum: Number(f.pps_lost_stratum.value),
      pps_dispersion_ppm: Math.round(Number(f.pps_dispersion.value) * 1000),
      lcd_enabled: f.lcd_enabled.checked,
      lcd_title: f.lcd_title.value,
      lcd_24_hour: f.lcd_24_hour.checked,
      lcd_pad_hour: f.lcd_pad_hour.checked,
      lcd_show_time_zone: f.lcd_show_time_zone.checked,
      lcd_address: f.lcd_address.value,
      button_enabled: f.button_enabled.checked,
      button_pin: pin('button_pin', 'Button pin'),
      time_zone: f.time_zone.value.trim(),
      led_type: f.led_type.value,
      led_brightness: Number(f.led_brightness.value),
      led_red_pin: pin('led_red_pin', 'LED red pin'),
      led_green_pin: pin('led_green_pin', 'LED green pin'),
      led_blue_pin: pin('led_blue_pin', 'LED blue pin'),
      led_ws2812_pin: pin('led_ws2812_pin', 'WS2812 data pin'),
      led_colors: {},
      mqtt_enabled: f.mqtt_enabled.checked,
      mqtt_broker: f.mqtt_broker.value.trim(),
      mqtt_tls: f.mqtt_tls.checked,
      mqtt_ca_pem: f.mqtt_ca_pem.value.replace(/\r/g, '').trim(),
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
      ntp_auth_required: f.ntp_auth_required.checked,
      ote_enabled: f.ote_enabled.checked,
      ptp_l2_enabled: f.ptp_l2_enabled.checked,
      ptp_udp4_enabled: f.ptp_udp4_enabled.checked,
      ptp_udp6_enabled: f.ptp_udp6_enabled.checked,
      ptp_master_selection: f.ptp_master_selection.checked,
      ptp_auth_enabled: f.ptp_auth_enabled.checked,
      ptp_auth_required: f.ptp_auth_required.checked,
      ptp_auth_mutable: f.ptp_auth_mutable.checked
    };
    if (body.ptp_udp4_enabled && body.ip_mode === 'ipv6') throw fieldError('ptp_udp4_enabled', t('PTP over UDP/IPv4 needs IPv4 (address families)'));
    if (body.ptp_udp6_enabled && body.ip_mode === 'ipv4') throw fieldError('ptp_udp6_enabled', t('PTP over UDP/IPv6 needs IPv6 (address families)'));
    for (const [name, lowest, highest, message] of ptpNumbers) {
      const text = f[name].value.trim();
      const value = Number(text);
      if (text === '' || !Number.isInteger(value) || value < lowest || value > highest) throw fieldError(name, t(message));
      body[name] = value;
    }
    // NTP and PTP keys: only sent when they were changed (an empty list removes all keys)
    if (ntpKeys.changed) body.ntp_keys = ntpKeys.collect('NTP key {n}: key ID 1 to 65535', 'NTP key {n}: exactly 32 letters or digits');
    if (ptpKeys.changed) body.ptp_keys = ptpKeys.collect('PTP key {n}: key ID 1 to 65535', 'PTP key {n}: exactly 32 letters or digits');
    for (const [key, name] of ledStates) {
      const hex = normaliseHex(colourRows[key].input.value);
      if (!hex) throw fieldError('led-colours', t('LED colour "{name}": #RRGGBB expected', { name: t(name) }));
      body.led_colors[key] = hex;
    }
    if (!body.ipv4_dhcp && body.ip_mode !== 'ipv6') {
      const v4 = splitCidr(f.ipv4_cidr.value.trim(), NaN);
      if (!v4) throw fieldError('ipv4_cidr', t('IPv4 address: a.b.c.d/prefix length expected'));
      body.ipv4_address = v4.address;
      body.ipv4_prefix = v4.prefix;
      body.ipv4_gateway = f.ipv4_gateway.value.trim();
    }
    const v6text = f.ipv6_cidr.value.trim();
    if (body.ip_mode === 'ipv4') {
      delete body.ipv6_gateway;
    } else if (v6text === '') {
      body.ipv6_address = '';
      body.ipv6_prefix = 64;
    } else {
      const v6 = splitCidr(v6text, 64);
      if (!v6) throw fieldError('ipv6_cidr', t('IPv6 address: address/prefix length expected'));
      body.ipv6_address = v6.address;
      body.ipv6_prefix = v6.prefix;
    }
    if (f.ntp_client_rate_limit.value.trim() === '' || !Number.isInteger(body.ntp_client_rate_limit) ||
        body.ntp_client_rate_limit < 0 || body.ntp_client_rate_limit > 10000) {
      throw fieldError('ntp_client_rate_limit', t('Rate limit per client: 0 to 10000 requests per second'));
    }
    if (f.gnss_min_satellites.value.trim() === '' || !Number.isInteger(body.gnss_min_satellites) ||
        body.gnss_min_satellites < 0 || body.gnss_min_satellites > 50) {
      throw fieldError('gnss_min_satellites', t('Minimum satellites in use: 0 to 50'));
    }
    if (f.gnss_min_fix_age.value.trim() === '' || !Number.isInteger(body.gnss_min_fix_age) ||
        body.gnss_min_fix_age < 0 || body.gnss_min_fix_age > 3600) {
      throw fieldError('gnss_min_fix_age', t('Minimum GNSS fix age: 0 to 3600 s'));
    }
    if (!Number.isInteger(body.pps_holdover) || body.pps_holdover < 0 || body.pps_holdover > 600) {
      throw fieldError('pps_holdover', t('PPS holdover: 0 to 600 s'));
    }
    if (!Number.isInteger(body.pps_lost_serve_min) || body.pps_lost_serve_min < 0 || body.pps_lost_serve_min > 1440) {
      throw fieldError('pps_lost_serve_min', t('Keep answering after the PPS loss: 0 to 1440 min'));
    }
    if (!Number.isInteger(body.pps_lost_stratum) || body.pps_lost_stratum < 2 || body.pps_lost_stratum > 16) {
      throw fieldError('pps_lost_stratum', t('Stratum after the holdover: 2 to 16'));
    }
    if (f.pps_dispersion.value.trim() === '' || !(body.pps_dispersion_ppm >= 1 && body.pps_dispersion_ppm <= 10000)) {
      throw fieldError('pps_dispersion', t('Root dispersion growth: 0.001 to 10 ms per second'));
    }
    if (!Number.isInteger(body.mqtt_report_period) || body.mqtt_report_period < 30 || body.mqtt_report_period > 86400) {
      throw fieldError('mqtt_report_period', t('MQTT reporting period: 30 to 86400 s'));
    }
    if (!Number.isInteger(body.mqtt_tf_client_limit) || body.mqtt_tf_client_limit < 50 || body.mqtt_tf_client_limit > 1000) {
      throw fieldError('mqtt_tf_client_limit', t('MQTT client limit with the TF card: 50 to 1000'));
    }
    collectMqttBroker(body);
    // MAC: lower case with ':'; the hardware MAC is sent as "" (= hardware MAC)
    const mac = normaliseMac(f.mac_address.value);
    if (mac === null) throw fieldError('mac_address', t('MAC address: xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx or xxxxxxxxxxxx (hexadecimal) expected'));
    if (mac !== '' && (parseInt(mac.slice(0, 2), 16) & 1) !== 0) throw fieldError('mac_address', t('MAC address: a unicast address is required (the lowest bit of the first byte must be 0)'));
    body.mac_address = mac === state.status.mac_hardware ? '' : mac;
    body.dns = f.dns.value.split(/\r?\n/).map((s) => s.trim()).filter((s) => s !== '');
    if (body.dns.length > 3) throw fieldError('dns', t('At most 3 DNS servers'));
    // Logins (Security)
    if (!f.web_local.checked && !f.web_radius.checked) throw fieldError('web_local', t('At least one login method must stay on.'));
    body.radius = collectRadius();
    body.web_login = {
      local: f.web_local.checked, radius: f.web_radius.checked, local_fallback: f.web_fallback.checked,
      totp_local: f.totp_local.checked, totp_radius: f.totp_radius.checked, totp_enroll: f.totp_enroll.checked
    };
    return body;
  }

  // The RADIUS fields of the form ("radius" of the settings): the ranges of main/auth/auth_config.cpp (the device checks
  // them again); a secret only when one was typed (an empty one keeps the stored secret)
  function collectRadius() {
    const number = (name, min, max, text) => {
      const v = currentValue(name);
      if (!/^-?\d{1,5}$/.test(v) || Number(v) < min || Number(v) > max) throw fieldError(name, text);
      return Number(v);
    };
    const servers = [1, 2].map((n) => {
      const server = { host: currentValue('r' + n + '_host') };
      server.port = number('r' + n + '_port', 1, 65535, t('RADIUS server {n}: UDP port 1 to 65535', { n: n }));
      const secret = f['r' + n + '_secret'].value;
      if (secret !== '') server.secret = secret;
      return server;
    });
    return {
      servers: servers,
      timeout_s: number('r_timeout', 1, 30, t('RADIUS timeout: 1 to 30 s')),
      retries: number('r_retries', 0, 5, t('RADIUS retries: 0 to 5')),
      dead_time_min: number('r_deadtime', 0, 60, t('RADIUS dead time: 0 to 60 minutes')),
      nas_identifier: currentValue('r_nas_id'), nas_ip: currentValue('r_nas_ip'), nas_ipv6: currentValue('r_nas_ip6'),
      service_type: Number(f.r_service.value), nas_port_type: Number(f.r_port_type.value),
      min_privilege: number('r_priv', 1, 15, t('Privilege level at least: 1 to 15')),
      required: currentValue('r_required'), require_message_authenticator: f.r_require_ma.checked
    };
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

  // Kinds of the addresses in the status (texts of the firmware and of renderStatus); others are shown as they are
  const ADDRESS_KINDS = ['link-local', 'static', 'SLAAC, unique local', 'SLAAC, global', 'not used: IPv4 only', 'not used: IPv6 only'];

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
          kind.textContent = ADDRESS_KINDS.includes(m[2]) ? t(m[2]) : m[2];
          li.appendChild(kind);
        }
      } else {
        li.textContent = item;
      }
      list.appendChild(li);
    }
  }

  // oneshot_boot: this start uses the one-shot set; the firmware ends it when Save Settings stores exactly the settings
  // in use (the next restart loads the same values). The check of unsaved_changes stays as a second guard.
  function renderBanners() {
    $('banner-oneshot').hidden = !(state.oneshot_boot && state.unsaved_changes);
    $('banner-unsaved').hidden = state.oneshot_boot || !state.unsaved_changes;
  }

  function renderStatus() {
    const st = state.status;
    setHost(state.settings.fqdn);
    yesNo($('st-time'), st.stratum_one, st.pps_holdover ? t('stratum 1, PPS holdover (PPS lost)') : t('stratum 1, GNSS and PPS locked'),
      !st.time_set
        ? t('waiting for the GNSS time')
        : st.gnss_fix_waiting
          ? t('not stratum 1 yet: GNSS fix {age} s old, {min} s required after the start', { age: st.gnss_fix_age_s, min: state.settings.gnss_min_fix_age })
          : t('not stratum 1 (PPS {pps}, GNSS {gnss})', { pps: st.pps_active ? t('active') : t('missing'), gnss: st.gnss_locked ? t('locked') : t('not locked') }));
    $('st-utc').textContent = st.utc || t('not set yet');
    const gnss = [st.ublox_receiver ? t('U-Blox UBX protocol') : t('NMEA standard protocol')];
    if (st.gnss_baud_in_use) gnss.push(t('{baud} baud', { baud: st.gnss_baud_in_use }));
    if (st.ublox_receiver) gnss.push(st.gnss_dynamic_model ? t('{model} mode', { model: st.gnss_dynamic_model }) : t('dynamic model not known'));
    // NMEA protocol: GGA (optional) and the reception of the RMC after the PPS edge
    if (st.gnss_satellites_used >= 0) gnss.push(t('{n} satellites in use, fix quality {q} (GGA)', { n: st.gnss_satellites_used, q: st.gnss_fix_quality }));
    if (st.gnss_rmc_after_pps_ms >= 0) {
      gnss.push(t('RMC {ms} ms after the PPS ({min} to {max} ms since the start)',
        { ms: st.gnss_rmc_after_pps_ms, min: st.gnss_rmc_after_pps_min_ms, max: st.gnss_rmc_after_pps_max_ms }));
    }
    // U-Blox UBX protocol: satellites of NAV-PVT (qErr and MON-RF are in the diagnostics panel)
    if (st.gnss_pvt_satellites >= 0) gnss.push(t('{n} satellites in use (NAV-PVT)', { n: st.gnss_pvt_satellites }));
    fillList('st-gnss', gnss, '', false);
    const keys = st.ntp_auth_keys_in_use;
    let auth = st.ntp_auth_available
      ? t(keys === 1 ? 'on, {n} key of the web settings' : 'on, {n} keys of the web settings', { n: keys })
      : (state.settings.ntp_auth_enabled ? t('on, but no usable key') : t('off'));
    // Policy: unsigned requests refused since the start (also those refused before it was switched back to mixed)
    if (state.settings.ntp_auth_enabled) {
      auth += '; ' + (state.settings.ntp_auth_required
        ? t('required: {n} unsigned requests refused since the start', { n: st.ntp_unsigned_refused })
        : t('mixed: unsigned requests are answered'));
    }
    $('st-auth').textContent = auth;
    // Requests dropped by the per-client rate limit since the start (also those dropped before it was switched off)
    const limit = state.settings.ntp_client_rate_limit;
    $('st-rate-limit').textContent = limit
      ? t('{limit} per second and client, {n} requests dropped since the start', { limit: limit, n: st.ntp_rate_limited })
      : t('off, {n} requests dropped since the start', { n: st.ntp_rate_limited });
    $('st-firmware').textContent = st.firmware + ' (' + st.build + ')' +
      (st.firmware_check_pending ? ', ' + t('new after an update: being checked (at most 15 min)') : '');
    const chip = (st.chip_model || 'ESP32-P4') + ' ' + st.chip_revision;
    $('st-chip').textContent = chip;
    $('ptp-chip').textContent = chip;
    // ESP32-S31: TF card reader on SPI (D1, D2 not used), GPIOs up to 61
    const s31 = st.chip_model === 'ESP32-S31';
    document.querySelectorAll('.tf-sd-only').forEach((el) => { el.hidden = s31; });
    $('tf-p4-note').hidden = s31;
    $('tf-sd-hint').hidden = s31;
    $('tf-s31-note').hidden = !s31;
    if (st.gpio_max) document.querySelectorAll('input[data-gpio]').forEach((el) => { el.max = String(st.gpio_max); });
    $('st-uptime').textContent = duration(st.uptime_s);
    $('st-heap').textContent = Math.round(st.free_heap / 1024) + ' kB';
    renderMqtt(st);
    const link = $('st-link');
    link.textContent = st.link_up
      ? t('link up, {speed} Mbit/s {duplex} duplex', { speed: st.link_speed_mbps, duplex: st.link_full_duplex ? t('full') : t('half') })
      : t('no link');
    if (st.eth_recoveries > 0) link.textContent += '; ' + t('{n} restarts of the Ethernet driver since the start', { n: st.eth_recoveries });
    link.className = st.link_up ? 'good' : 'bad';
    $('st-mac').textContent = st.mac_address === st.mac_hardware
      ? t('{mac} (hardware MAC)', { mac: st.mac_address })
      : t('{mac} (set, hardware MAC {hardware})', { mac: st.mac_address, hardware: st.mac_hardware });
    $('mac-hardware').textContent = st.mac_hardware;
    f.mac_address.placeholder = st.mac_hardware;
    $('st-fingerprint').textContent = state.fingerprint;
    $('st-certificate').textContent = certificateText(state.tls);
    renderTls(false);
    // Same form as the IPv6 lines: address in the first line, gateway and source in the second
    const mode = state.settings.ip_mode;
    fillList('st-ipv4', st.ipv4_address
      ? [st.ipv4_address + '/' + st.ipv4_prefix + ' (' + t('via {gateway}, {source}', { gateway: st.ipv4_gateway || '-', source: st.ipv4_dhcp_running ? 'DHCP' : t('static') }) + ')']
      : [], mode === 'ipv6' ? t('off (IPv6 only)') : st.ipv4_dhcp_running ? t('waiting for DHCP') : t('none'), true);
    fillList('st-ipv6', st.ipv6, mode === 'ipv4' ? t('off (IPv4 only)') : t('none'), true);
    fillList('st-dns', st.dns, t('none'), true);
    // Servers of an address family that is off are listed as received but not used (except link-local IPv6 ones)
    const unused = (items, off) => (items || []).map((a) => off && !/^fe[89ab]/i.test(a) ? a + ' (not used: ' + (mode === 'ipv4' ? 'IPv4 only' : 'IPv6 only') + ')' : a);
    fillList('st-dns-v4', unused(st.dns_received_v4, mode === 'ipv6'), t('none'), true);
    fillList('st-dns-v6', unused(st.dns_received_v6, mode === 'ipv4'), t('none'), true);
    $('st-client').textContent = addressText(st.client);

    $('banner-default-pw').hidden = !state.default_password;
    renderBanners();
    renderPending(state.network_pending);
    renderSession();
    renderRadiusStatus();
    renderTotp();
    renderOte();
    updateDependent();
  }

  // ---- The login of this session (header and Status; "session" of /api/state) ----
  function renderSession() {
    const s = state.session || {};
    const user = s.user || 'admin';
    const how = (s.radius ? 'RADIUS' : t('local password')) + (s.totp ? ' + TOTP' : '');
    $('st-session').textContent = user + ' (' + how + ')';
    $('hdr-user').textContent = user + ' (' + how + ')';
    $('hdr-user').hidden = false;
  }

  // ---- RADIUS in Status ("radius_status" of /api/state) ----
  function renderRadiusStatus() {
    const r = state.radius_status;
    if (!r) return;
    (r.servers || []).forEach((s, i) => {
      const el = $('st-radius-' + (i + 1));
      if (!el) return;
      const host = s.address && s.address !== s.host ? s.host + ' (' + addressText(s.address) + ')' : s.host;
      const where = (host.indexOf(':') >= 0 && !host.includes('(') ? '[' + host + ']' : host) + ':' + s.port;
      if (!s.used) {
        el.textContent = t('not set');
        el.className = '';
      } else if (!r.running) {
        el.textContent = t('{addr}: not asked yet (RADIUS logins are off)', { addr: where });
        el.className = '';
      } else if (s.dead) {
        el.textContent = t('{addr}: no answer, skipped for {s} s more (dead time)', { addr: where, s: s.dead_for_s });
        el.className = 'bad';
      } else if (s.last_answer_s >= 0) {
        el.textContent = t('{addr}: answers, the last answer {age} ago after {ms} ms', { addr: where, age: duration(s.last_answer_s), ms: s.rtt_ms });
        el.className = 'good';
      } else {
        el.textContent = t('{addr}: not asked yet', { addr: where }) + (s.timeouts > 0 ? ', ' + t('{n} logins without an answer', { n: s.timeouts }) : '');
        el.className = s.timeouts > 0 ? 'bad' : '';
      }
      if (s.used && s.dropped > 0) el.textContent += ', ' + t('{n} invalid answers dropped', { n: s.dropped });
    });
    $('st-radius-count').textContent = t('{a} accepted, {r} rejected, {n} refused by the privilege level or the required attribute, {t} without an answer, {b} refused as busy',
      { a: r.accepted, r: r.rejected, n: r.refused, t: r.no_answer, b: r.busy });
  }

  // ---- TOTP tokens (Security; "totp" of /api/state) ----
  function dateText(unix) {
    if (!unix) return '–';
    const d = new Date(unix * 1000);
    const two = (v) => String(v).padStart(2, '0');
    return d.getFullYear() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' + two(d.getHours()) + ':' + two(d.getMinutes());
  }
  function renderTotp() {
    const totp = state.totp;
    if (!totp) return;
    const tokens = totp.tokens || [];
    $('st-totp').textContent = t('{n} of {max}', { n: tokens.length, max: totp.max }) +
      (totp.time_ok ? '' : '; ' + t('the clock is not set: only the recovery codes work'));
    $('st-totp').className = totp.time_ok ? '' : 'bad';
    $('totp-time').hidden = totp.time_ok;
    const body = $('totp-users');
    body.replaceChildren();
    for (const token of tokens) {
      const row = document.createElement('tr');
      for (const text of [token.user, token.user === 'admin' ? t('local') : 'RADIUS', dateText(token.created), dateText(token.last_used),
        String(token.recovery_left)]) {
        const td = document.createElement('td');
        td.textContent = text;
        row.appendChild(td);
      }
      const td = document.createElement('td');
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = t('Delete');
      button.addEventListener('click', async () => {
        if (!window.confirm(t('Delete the token of {user}? Its logins then need a new one.', { user: token.user }))) return;
        const r = await api('/api/totp/delete', { user: token.user });
        show(r.status === 200 ? ts(r.data.message) : (ts(r.data.error) || t('Request failed ({status})', { status: r.status })), r.status === 200 ? 'ok' : 'error');
        await loadState(false);
      });
      td.appendChild(button);
      row.appendChild(td);
      body.appendChild(row);
    }
    if (tokens.length === 0) {
      const row = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 6;
      td.textContent = t('No tokens.');
      row.appendChild(td);
      body.appendChild(row);
    }
  }

  // The QR code of a new token ("qr" of the device: one hex string per row, the left module the highest bit)
  function drawQr(svg, qr) {
    const size = qr.size;
    const quiet = 2;
    svg.setAttribute('viewBox', [-quiet, -quiet, size + 2 * quiet, size + 2 * quiet].join(' '));
    let d = '';
    qr.rows.forEach((row, y) => {
      for (let x = 0; x < size; x++) {
        const nibble = parseInt(row[x >> 2], 16);
        if ((nibble >> (3 - (x & 3))) & 1) d += 'M' + x + ' ' + y + 'h1v1h-1z';
      }
    });
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', d);
    svg.replaceChildren(path);
  }
  function fillRecovery(list, codes) {
    list.replaceChildren();
    for (const code of codes) {
      const li = document.createElement('li');
      li.textContent = code;
      list.appendChild(li);
    }
  }

  let enrolling = '';
  $('totp-new').addEventListener('click', async () => {
    const user = $('totp-new-user').value.trim();
    if (user === '') {
      show(t('Enter a user name.'), 'error');
      return;
    }
    const r = await api('/api/totp/enroll', { user: user });
    if (r.status !== 200) {
      show(ts(r.data.error) || t('Request failed ({status})', { status: r.status }), 'error');
      return;
    }
    enrolling = r.data.user;
    drawQr($('totp-qr'), r.data.qr);
    $('totp-secret').textContent = r.data.secret;
    $('totp-enroll-for').textContent = r.data.replaces ? t('A new token for {user}: it replaces the one in use once it is confirmed.', { user: r.data.user })
      : t('A new token for {user}.', { user: r.data.user });
    $('totp-confirm-code').value = '';
    $('totp-enroll-box').hidden = false;
    $('totp-recovery').hidden = true;
    $('totp-confirm-code').focus();
  });
  $('totp-confirm').addEventListener('click', async () => {
    const code = $('totp-confirm-code').value.trim();
    if (!/^\d{6}$/.test(code)) {
      show(t('Enter the 6 digits shown by the app.'), 'error');
      return;
    }
    const r = await api('/api/totp/confirm', { user: enrolling, code: code });
    if (r.status !== 200) {
      show(ts(r.data.error) || t('Request failed ({status})', { status: r.status }), 'error');
      return;
    }
    $('totp-enroll-box').hidden = true;
    $('totp-secret').textContent = '';
    $('totp-qr').replaceChildren();
    $('totp-recovery-for').textContent = t('Recovery codes for {user}: each works once instead of a code; shown only now, keep them safe.', { user: r.data.user });
    fillRecovery($('totp-recovery-list'), r.data.recovery_codes || []);
    $('totp-recovery').hidden = false;
    show(t('The token of {user} is stored.', { user: r.data.user }), 'ok');
    await loadState(false);
  });

  // ---- The test of the RADIUS settings of the form ----
  $('r-test').addEventListener('click', async () => {
    const out = $('r-test-result');
    let radius;
    try {
      radius = collectRadius();
    } catch (err) {
      if (err.field) revealField(err.field);
      show(err.message, 'error');
      return;
    }
    const user = $('r-test-user').value.trim();
    if (user === '') {
      show(t('Enter a user name.'), 'error');
      return;
    }
    out.hidden = false;
    out.className = 'note';
    out.textContent = t('Testing…');
    $('r-test').disabled = true;
    const r = await api('/api/radius/test', { user: user, password: $('r-test-pw').value, radius: radius });
    $('r-test').disabled = false;
    $('r-test-pw').value = '';
    syncPasswordEyes();
    if (r.status !== 200) {
      out.textContent = ts(r.data.error) || t('Request failed ({status})', { status: r.status });
      out.className = 'note bad';
      return;
    }
    const d = r.data;
    const from = d.server ? t('from server {n} after {ms} ms', { n: d.server, ms: d.rtt_ms }) : '';
    const texts = {
      accept: t('Access-Accept {from}: privilege level {p}, the login would be accepted.', { from: from, p: d.privilege }),
      low_privilege: t('Access-Accept {from}: privilege level {p} is below the setting, the login would be refused.', { from: from, p: d.privilege }),
      missing_attribute: t('Access-Accept {from} without the required attribute: the login would be refused.', { from: from }),
      reject: d.challenge ? t('Access-Challenge {from}: not supported, the login would be refused.', { from: from })
        : t('Access-Reject {from}.', { from: from }),
      no_answer: t('No RADIUS server answered.'),
      busy: t('Too many RADIUS logins at once: try again.')
    };
    out.textContent = (texts[d.result] || d.text) + (d.summary ? ' ' + d.summary : '');
    out.className = d.result === 'accept' ? 'note good' : 'note bad';
  });

  // ---- Diagnostics (review 4.5): loaded only while the panel is open ----
  // Number with a fixed count of decimals (decimal comma in German)
  function num(value, digits) {
    const text = Number(value).toFixed(digits);
    return lang === 'de' ? text.replace('.', ',') : text;
  }
  function percent(part, total) {
    if (part > 0 && part < total && 100 * part / total < 0.1) return '< ' + num(0.1, 1) + ' %';
    return (total > 0 ? num(100 * part / total, part === 0 || part === total ? 0 : 1) : num(0, 0)) + ' %';
  }
  // Shows or hides a row of a status grid (the dd holding id and its dt)
  function diagRow(id, visible) {
    const dd = $(id).closest('dd');
    dd.hidden = !visible;
    dd.previousElementSibling.hidden = !visible;
  }
  // Non-empty bins of a histogram as "label share", e.g. "300-399 ms 97 %"
  function bins(counts, labels) {
    const total = counts.reduce((sum, n) => sum + n, 0);
    return counts.map((n, i) => (n > 0 ? labels[i] + ' ' + percent(n, total) : '')).filter((s) => s).join(', ');
  }

  function renderDiag(d) {
    // PTP clock and servo
    $('dg-measurement').textContent = !d.servo ? t('not available (the PTP clock is set once per second)')
      : !d.servo_valid ? t('no estimate (waiting for the PPS)')
        : d.from_loopback ? t('PTP PPS loopback (hardware capture)')
          : d.loopback_active ? t('software readings (loopback pulse missing)') : t('software readings');
    const valid = d.servo && d.servo_valid;
    diagRow('dg-offset', valid);
    diagRow('dg-rate', valid);
    diagRow('dg-filter', d.servo);
    diagRow('dg-loopback', d.servo);
    diagRow('dg-qerr', d.servo && d.ublox);
    $('dg-offset').textContent = t('{offset} ns (raw PTP clock minus true time), noise {noise} ns',
      { offset: num(d.offset_ns, 1), noise: num(d.noise_ns, 1) });
    $('dg-rms').textContent = d.rms_age_s < 0 ? t('no complete minute yet')
      : d.rms_age_s > 120 ? t('no data in the last 2 minutes')
        : t('{minute} ns in the last minute, {hour} ns in the last hour', { minute: num(d.rms_minute_ns, 1), hour: num(d.rms_hour_ns, 1) });
    $('dg-rate').textContent = t('{free} ppb free-running, command {cmd} ppb', { free: num(d.free_ppb, 1), cmd: d.cmd_ppb });
    let filter = t('outliers {o}, steps {s}, command errors {e}', { o: d.outliers, s: d.steps, e: d.command_failures });
    if (d.relock_residual_ns) filter += '; ' + t('re-lock residual {r} ns', { r: num(d.relock_residual_ns, 1) });
    $('dg-filter').textContent = filter;
    // PPS holdover: the time error is measured against the PPS when it comes back (the ramp then removes it)
    diagRow('dg-holdover', d.servo);
    const holdover = [];
    if (d.holdover_running_s >= 0) holdover.push(t('running for {d}', { d: duration(d.holdover_running_s) }));
    if (d.holdovers > 0) {
      const ended = { d: duration(Math.round(d.last_holdover_s)), t: duration(d.last_holdover_end_s), n: d.holdovers };
      holdover.push(d.last_holdover_stepped
        ? t('{d} long, ended at uptime {t}: more than 100 \u00b5s off, the time was set instead of ramped ({n} since the start)', ended)
        : t('{d} long, ended at uptime {t}: served time {e} ns off when the PPS came back ({n} since the start)',
          Object.assign(ended, { e: (d.last_holdover_error_ns > 0 ? '+' : '') + num(d.last_holdover_error_ns, 1) })));
    }
    $('dg-holdover').textContent = holdover.length ? holdover.join('; ') : t('none since the start');
    $('dg-loopback').textContent = !d.loopback_active ? t('off')
      : t('pulses used {used}, missing {missing}, implausible {rejected}; software minus loopback {bias} ns',
        { used: d.loopback_used, missing: d.loopback_missing, rejected: d.loopback_rejected, bias: num(d.sw_bias_ns, 1) });
    // qErr correction of the time pulse (TIM-TP): its state comes with the status
    const st = state ? state.status : {};
    const slope = Number(st.gnss_qerr_slope || 0).toFixed(2);
    const qerrState = st.gnss_qerr_state === 1 ? t('qErr correction: learning the sign')
      : st.gnss_qerr_state === 2 ? t('qErr correction active (slope {slope})', { slope })
        : st.gnss_qerr_state === 3 ? t('qErr correction off: no correlation (slope {slope})', { slope })
          : st.gnss_qerr_state === 4 ? t('no qErr: the receiver flags it invalid (TIM-TP)')
            : t('no qErr (TIM-TP missing or PTP loopback off)');
    $('dg-qerr').textContent = qerrState + '; ' + t('correction sign {sign}, samples {n}, standard deviation {sd} ns; TIM-TP matched {m}, missing {u}, invalid {i}',
      { sign: d.qerr_sign > 0 ? '+1' : d.qerr_sign < 0 ? '-1' : t('none'), n: d.qerr_samples, sd: num(d.qerr_sd_ns, 1),
        m: d.qerr_matched, u: d.qerr_unmatched, i: d.qerr_invalid }) +
      (d.tp_messages > 0 ? '; ' + t('latest TIM-TP: qErr {q} ps, flags 0x{f}',
        { q: d.tp_last_qerr_ps, f: Number(d.tp_last_flags).toString(16).toUpperCase().padStart(2, '0') }) : '');
    const isrLabels = ['0.25', '0.5', '1', '2', '5'].map((v) => '≤ ' + (lang === 'de' ? v.replace('.', ',') : v) + ' µs');
    isrLabels.push('> 5 µs');
    $('dg-isr').textContent = d.isr_total > 0
      ? t('latency {lat} µs; excess over the central value: {bins}; max {max} µs',
        { lat: num(d.isr_latency_us, 2), bins: bins(d.isr_bins, isrLabels), max: num(d.isr_max_ns / 1000, 2) })
      : t('latency {lat} µs', { lat: num(d.isr_latency_us, 2) });

    // Stability of the GNSS PPS against the free-running PTP clock
    const rows = d.stability || [];
    $('dg-stability-note').textContent = rows.length === 0 ? t('Not enough data yet.')
      : t('{n} s of continuous data ({source}). ADEV: overlapping Allan deviation, TDEV: time deviation. The frequency commands of the servo are taken out, outliers are replaced by the prediction.',
        { n: d.stability_samples, source: d.stability_source === 1 ? t('PTP PPS loopback') : t('software readings') });
    const body = $('dg-stability');
    body.replaceChildren();
    for (const row of rows) {
      const tr = document.createElement('tr');
      const adev = Number(row.adev).toExponential(2);
      for (const text of [row.tau + ' s', lang === 'de' ? adev.replace('.', ',') : adev,
        row.tdev_ns > 0 ? num(row.tdev_ns, row.tdev_ns < 10 ? 2 : 1) + ' ns' : '-']) {
        const td = document.createElement('td');
        td.textContent = text;
        tr.appendChild(td);
      }
      body.appendChild(tr);
    }

    // NTP paths since the start
    fillList('dg-replies', [
      t('hardware path {n}: interleaved {i}, without hardware TX time stamp {x}',
        { n: d.hw_replies, i: percent(d.hw_interleaved, d.hw_replies), x: percent(d.hw_no_tx_stamp, d.hw_replies) }),
      t('socket path {n}: interleaved {i}', { n: d.sw_replies, i: percent(d.sw_interleaved, d.sw_replies) })
    ], '', false);
    $('dg-lookup').textContent = t('latest exchange of the address {r}, exchange map {m}, not found (basic mode) {x}',
      { r: d.lookup_record_hits, m: d.lookup_map_hits, x: d.lookup_misses });
    diagRow('dg-txdelay', d.tx_delay);
    const txDelay = [t('IPv4 {v4} ns ({n4} values), IPv6 {v6} ns ({n6} values)',
      { v4: d.tx_delay_v4_ns, n4: d.tx_delay_v4_samples, v6: d.tx_delay_v6_ns, n6: d.tx_delay_v6_samples })];
    // Per family without authentication: range since the start and the running mean per 100 ms of the second of T3
    const us = (ns) => num(ns / 1000, 1);
    ['IPv4', 'IPv6'].forEach((family, i) => {
      const phase = (d.tx_delay_phase || [])[i];
      if (!phase || !d.tx_delay_max_ns || d.tx_delay_max_ns[i] <= 0) return;
      txDelay.push(t('{family}: minimum {min} µs, maximum {max} µs',
        { family: family, min: us(d.tx_delay_min_ns[i]), max: us(d.tx_delay_max_ns[i]) }));
      const parts = phase.n.map((n, b) => (n > 0 ? (b * 100) + '-' + (b * 100 + 99) + ' ms ' + us(phase.ns[b]) + ' µs (' + n + ')' : ''))
        .filter((s) => s);
      txDelay.push(t('{family} by phase of the second (mean, values): {bins}', { family: family, bins: parts.join(', ') }));
    });
    fillList('dg-txdelay', txDelay, '', false);
    $('dg-shed').textContent = t('{n} requests dropped since the start', { n: d.shed });
    // A receive task got no empty queue for 20 ms (a flood) and rested for 1 ms, so the other tasks of its core get time;
    // the Ethernet receive task rests after 10 ms of frames without a gap
    $('dg-rests').textContent = t('{n} rests of 1 ms since the start (an NTP receive task was busy for 20 ms without a pause)', { n: d.receive_rests || 0 }) +
      '; ' + t('Ethernet receive task: {n} (10 ms of frames without a gap)', { n: d.eth_receive_rests || 0 });

    // GNSS receiver
    const gnss = [];
    if (d.ublox) {
      gnss.push(t('UBX frames {f}, NAK {n}, UART overflows {o}', { f: d.ubx_frames, n: d.ubx_naks, o: d.uart_overflows }));
      gnss.push(d.first_resolved_s < 0 ? t('fullyResolved: not yet')
        : t('fullyResolved first at uptime {t}, lost since: {n}', { t: duration(d.first_resolved_s), n: d.resolved_losses }));
      gnss.push(d.first_valid_utc_s < 0 ? t('validUTC: not yet')
        : t('validUTC first at uptime {t}, lost since: {n}', { t: duration(d.first_valid_utc_s), n: d.valid_utc_losses }));
      // Time pulse 1 (CFG-TP, read at the setup)
      if (!d.tp_known) {
        gnss.push(t('time pulse: configuration not read'));
      } else if (!d.tp_enabled) {
        gnss.push(t('time pulse: off'));
      } else {
        const period = (v) => (d.tp_frequency_mode ? v + ' Hz' : num(v / 1e6, v % 1e6 === 0 ? 0 : 3) + ' s');
        const length = (v) => (d.tp_length_mode ? num(v / 1000, v % 1000 === 0 ? 0 : 3) + ' ms' : t('ratio'));
        const unlocked = d.tp_length_mode && d.tp_length_us === 0 ? t('no pulse without lock')
          : t('without lock: period {p}, length {l}', { p: period(d.tp_period), l: length(d.tp_length_us) });
        gnss.push(t('time pulse: period {p}, length {l}; {unlocked}',
          { p: period(d.tp_period_lock), l: length(d.tp_length_lock_us), unlocked }));
        gnss.push(t('time pulse: {edge} edge, {grid} grid{aligned}, cable delay {c} ns, user delay {u} ns',
          { edge: d.tp_rising ? t('rising') : t('falling'), grid: ['UTC', 'GPS', 'GLONASS', 'BeiDou', 'Galileo'][d.tp_time_grid] || '?',
            aligned: d.tp_aligned ? '' : ', ' + t('not aligned to the second'), c: d.tp_cable_delay_ns, u: d.tp_user_delay_ns }));
      }
      if (d.rf_known) {
        // Antenna supervisor and jamming monitor (MON-RF); their states come with the status
        const antenna = [t('starting'), t('unknown'), t('OK'), t('short circuit'), t('open')][st.gnss_antenna_status] || t('unknown');
        const jamming = [t('unknown'), t('none'), t('warning'), t('critical')][st.gnss_jamming_state] || t('unknown');
        gnss.push(t('antenna {antenna}, jamming {jamming}, AGC {agc} % ({count}), noise {n} per ms, CW jamming indicator {j} (MON-RF)',
          { antenna, jamming, agc: Math.round(d.rf_agc_count * 100 / 8191), count: d.rf_agc_count, n: d.rf_noise_per_ms, j: d.rf_jam_indicator }));
      }
    } else {
      gnss.push(t('sentences {n}, checksum errors {e}, UART overflows {o}', { n: d.nmea_sentences, e: d.nmea_checksum_errors, o: d.uart_overflows }));
      const rmcLabels = d.rmc_phase_bins.map((n, i) => (i * 100) + '-' + (i * 100 + 99) + ' ms');
      const rmc = bins(d.rmc_phase_bins, rmcLabels);
      if (rmc) gnss.push(t('RMC after the PPS: {bins}', { bins: rmc }));
    }
    fillList('dg-gnss', gnss, '', false);

    // Internal heap and the stack reserve of the tasks, the tightest first (review E-14)
    const kb = (bytes) => num(bytes / 1024, 1);
    $('dg-heap').textContent = t('free {free} kB, lowest since the start {min} kB, largest block {block} kB',
      { free: kb(d.heap_free), min: kb(d.heap_min_free), block: kb(d.heap_largest_block) });
    const tasks = (d.tasks || []).slice().sort((x, y) => x.free / x.size - y.free / y.size);
    const stacks = $('dg-stacks');
    stacks.replaceChildren();
    for (const task of tasks) {
      const li = document.createElement('li');
      li.textContent = t('{name}: at least {free} of {size} B free ({p})',
        { name: task.name, free: task.free, size: task.size, p: percent(task.free, task.size) });
      if (task.free < task.size / 10) li.className = 'bad';
      stacks.appendChild(li);
    }
    diagRow('dg-stacks', tasks.length > 0);
    // NVS entries of 32 bytes: a new TOTP token needs about 12 and is only stored while one more settings set fits
    const nvs = d.nvs;
    if (nvs) {
      $('dg-nvs').textContent = t('{used} of {total} entries used, {free} available; a settings set needs {set}, a TOTP token about 12',
        { used: nvs.used, total: nvs.total, free: nvs.available, set: nvs.settings });
      $('dg-nvs').className = nvs.available < nvs.settings + 16 ? 'bad' : '';
    }
    diagRow('dg-nvs', !!nvs);

    // Core dump of the last crash (review E-07)
    const crash = d.crash || {};
    $('dg-crash').textContent = crash.present
      ? t('task {task}, PC {pc}{reason} ({kb} kB core dump)',
        { task: crash.task || '?', pc: crash.pc, reason: crash.reason ? ': ' + crash.reason : '', kb: num(crash.size / 1024, 1) })
      : t('none recorded');
    $('dg-crash-actions').hidden = !crash.present;
  }

  $('dg-crash-erase').addEventListener('click', async () => {
    if (!confirm(t('Delete the stored core dump?'))) return;
    const r = await api('/api/coredump/erase', {});
    show(r.status === 200 ? ts(r.data.message) : (ts(r.data.error) || t('Request failed ({status})', { status: r.status })),
      r.status === 200 ? 'ok' : 'error');
    loadDiag();
  });

  // ---- PTP panel: state, counters and followers, loaded only while the panel is open ----
  const ptpTransportNames = { l2: 'Ethernet', udp4: 'UDP/IPv4', udp6: 'UDP/IPv6' };
  function renderPtp(p) {
    $('ptp-identity').textContent = p.started ? p.clock_identity : t('PTP is not running (no hardware time stamps)');
    $('ptp-utc').textContent = t('{n} s ({source})', { n: p.utc_offset,
      source: p.utc_offset_source === 'gnss' ? t('from the GNSS receiver') : t('setting') });
    const auth = p.authentication || {};
    $('ptp-auth').textContent = auth.signing
      ? (auth.required ? t('on: sent messages are signed, unsigned requests are dropped')
        : t('on: sent messages are signed, unsigned requests are answered'))
      : (auth.required ? t('required, but no usable key: PTP is silent') : t('off'));
    const ports = $('ptp-ports');
    ports.textContent = '';
    for (const port of p.ports || []) {
      if (!port.enabled) continue;
      const li = document.createElement('li');
      const head = document.createElement('strong');
      head.textContent = ptpTransportNames[port.transport] + ': ' + ts(port.state);
      head.className = port.state === 'serving' ? 'good' : '';
      li.appendChild(head);
      li.appendChild(document.createElement('br'));
      li.appendChild(document.createTextNode(t('{f} followers; sent: Sync {s}, Follow_Up {u}, Announce {a}, Delay_Resp {r}; received: Delay_Req {q}, Announce of other masters {o}', {
        f: port.followers, s: port.sync, u: port.follow_up, a: port.announce, r: port.delay_resp, q: port.delay_req, o: port.foreign_announces })));
      const problems = port.rate_limited + port.drops + port.invalid + port.rx_timestamp_misses + port.tx_timestamp_misses + port.transmit_failures;
      if (problems > 0 || port.ignored > 0) {
        li.appendChild(document.createElement('br'));
        li.appendChild(document.createTextNode(t('rate limited {l}, dropped {d}, invalid {i}, ignored {g}, receive stamp missing {rx}, transmit stamp missing {tx}, send failures {sf}', {
          l: port.rate_limited, d: port.drops, i: port.invalid, g: port.ignored, rx: port.rx_timestamp_misses, tx: port.tx_timestamp_misses, sf: port.transmit_failures })));
      }
      if (port.auth_rejected > 0 || port.unsigned_refused > 0) {
        li.appendChild(document.createElement('br'));
        li.appendChild(document.createTextNode(t('authentication: {r} rejected (key, ICV or TLV), {u} unsigned refused', {
          r: port.auth_rejected, u: port.unsigned_refused })));
      }
      ports.appendChild(li);
    }
    if (!ports.firstChild) {
      const li = document.createElement('li');
      li.textContent = t('off');
      ports.appendChild(li);
    }
    const list = $('ptp-followers');
    list.textContent = '';
    const followers = (p.followers || []).slice().sort((x, y) => x.last_seen_s - y.last_seen_s);
    for (const follower of followers) {
      const li = document.createElement('li');
      li.textContent = t('{mac} ({transport}, port {port}), last delay request {s} s ago', {
        mac: follower.mac, transport: ptpTransportNames[follower.transport] || follower.transport, port: follower.port_identity, s: follower.last_seen_s });
      list.appendChild(li);
    }
    if (!list.firstChild) {
      const li = document.createElement('li');
      li.textContent = t('none');
      list.appendChild(li);
    }
  }

  async function loadPtp() {
    if (!state || !$('ptp-panel').open) return;
    const r = await api('/api/ptp');
    if (r.status === 200) renderPtp(r.data);
  }

  $('ptp-panel').addEventListener('toggle', () => {
    clearInterval(ptpTimer);
    if (!$('ptp-panel').open) return;
    loadPtp();
    ptpTimer = setInterval(loadPtp, 10000);
  });

  async function loadDiag() {
    if (!state || !$('diag-panel').open) return;
    const r = await api('/api/diag');
    if (r.status === 200) renderDiag(r.data);
  }

  $('diag-panel').addEventListener('toggle', () => {
    clearInterval(diagTimer);
    if (!$('diag-panel').open) return;
    loadDiag();
    diagTimer = setInterval(loadDiag, 10000);
  });

  // ---- GNSS console (sub-panel of GNSS and PPS) ----
  // While it is open (and its panel) the page asks for new lines every second; the device records them only then
  const CONSOLE_KEEP = 500;  // lines kept on the page
  const consoleLines = [];   // { kind: 'sent' | 'text' | 'ubx' | 'gap', periodic, text }
  const consoleHistory = []; // commands sent, for the arrow keys
  let consoleHistoryIndex = 0;
  let consoleAfter = 0;      // sequence number of the latest entry received
  let consoleBusy = false;
  let consoleUblox = false;

  const consoleOpen = () => $('gnss-console').open && $('gnss-console').closest('details.panel').open;
  const consoleMode = () => ($('console-mode-ubx').checked ? 'ubx' : 'text');

  function updateConsoleMode() {
    const ubx = consoleMode() === 'ubx';
    $('console-text-options').hidden = ubx;
    $('console-hint-text').hidden = ubx;
    $('console-hint-ubx').hidden = !ubx;
    $('console-ubx-off').hidden = !ubx || consoleUblox;
    $('console-command').placeholder = ubx ? '0A 04' : '$CFGMSG,0,4,1';
  }

  function addConsoleLine(kind, periodic, text) {
    consoleLines.push({ kind: kind, periodic: periodic, text: text });
    if (consoleLines.length > CONSOLE_KEEP) consoleLines.splice(0, consoleLines.length - CONSOLE_KEEP);
  }

  function renderConsole() {
    const out = $('console-output');
    const atBottom = out.scrollHeight - out.scrollTop - out.clientHeight < 24;
    const periodic = $('console-periodic').checked;
    const fragment = document.createDocumentFragment();
    for (const line of consoleLines) {
      if (line.periodic && !periodic) continue;
      const span = document.createElement('span');
      span.className = line.kind;
      span.textContent = (line.kind === 'sent' ? '> ' : '') + line.text + '\n';
      fragment.appendChild(span);
    }
    out.replaceChildren(fragment);
    if (atBottom) out.scrollTop = out.scrollHeight;
  }

  async function loadConsole() {
    if (!state || !consoleOpen() || consoleBusy) return;
    consoleBusy = true;
    const r = await api('/api/gnss/console?after=' + consoleAfter);
    consoleBusy = false;
    if (r.status !== 200) return;
    const d = r.data;
    const entries = d.entries || [];
    if (d.reset && consoleAfter > 0) addConsoleLine('gap', false, t('(the device was restarted)'));
    if (d.skipped) addConsoleLine('gap', false, t('(older lines were overwritten: some are missing here)'));
    for (const e of entries) addConsoleLine(e.kind, !!e.periodic, e.text);
    consoleAfter = d.last || 0;
    if (consoleUblox !== !!d.ublox) {
      consoleUblox = !!d.ublox;
      updateConsoleMode();
    }
    if (entries.length > 0 || d.skipped || d.reset) renderConsole();
  }

  async function sendConsoleCommand() {
    const input = $('console-command');
    const command = input.value.trim();
    if (!command || $('console-send').disabled) return;
    $('console-send').disabled = true;
    const r = await api('/api/gnss/command', {
      mode: consoleMode(), command: command, checksum: $('console-checksum').checked, crlf: $('console-crlf').checked
    });
    $('console-send').disabled = false;
    if (r.status === 200) {
      if (consoleHistory[consoleHistory.length - 1] !== command) consoleHistory.push(command);
      if (consoleHistory.length > 20) consoleHistory.shift();
      consoleHistoryIndex = consoleHistory.length;
      input.value = '';
      await loadConsole();
    } else {
      addConsoleLine('gap', false, t('Not sent: {error}', { error: ts(r.data.error) || r.status }));
      renderConsole();
    }
  }

  $('gnss-console').addEventListener('toggle', () => {
    clearInterval(consoleTimer);
    if (!$('gnss-console').open) return;
    updateConsoleMode();
    loadConsole();
    consoleTimer = setInterval(loadConsole, 1000);
  });
  // The console is not part of the settings: its fields do not mark the form as changed
  for (const type of ['input', 'change']) {
    $('gnss-console').addEventListener(type, (e) => {
      e.stopPropagation();
      if (e.target.name === 'console-mode') updateConsoleMode();
      if (e.target.id === 'console-periodic') renderConsole();
    });
  }
  $('console-send').addEventListener('click', sendConsoleCommand);
  $('console-clear').addEventListener('click', () => {
    consoleLines.length = 0;
    renderConsole();
  });
  $('console-command').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      sendConsoleCommand();
    } else if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && consoleHistory.length > 0) {
      e.preventDefault();
      consoleHistoryIndex = Math.max(0, Math.min(consoleHistory.length, consoleHistoryIndex + (e.key === 'ArrowUp' ? -1 : 1)));
      e.target.value = consoleHistory[consoleHistoryIndex] || '';
    }
  });

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
    $('totp-card').hidden = true;
    $('recovery-card').hidden = true;
    $('app').hidden = false;
    $('logout').hidden = false;
    renderStatus();
    if (fill) {
      fillForm(state.settings);
      $('allow-list').value = (state.allow_list || []).join('\n');
      renderTls(true);
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
    clearInterval(diagTimer);
    clearInterval(ptpTimer);
    show(text, 'warn');
  }

  function setBusy(busy) {
    document.querySelectorAll('button').forEach((b) => { if (b.id !== 'theme-toggle') b.disabled = busy; });
    if (!busy && state) renderOte(); // the upload button stays off while OTE updates are off
  }

  // ---- Actions ----
  // The answer of a login (both steps): the session, the second factor, or the error
  let pendingLogin = '';
  async function loginDone(r) {
    if (r.status === 200 && r.data.pending) {
      // The password was right: the second factor (a new token with the first login: its QR code)
      pendingLogin = r.data.pending;
      $('login-card').hidden = true;
      $('totp-card').hidden = false;
      $('totp-for').textContent = t('The password of {user} is right.', { user: r.data.user });
      $('totp-login-enroll').hidden = !r.data.totp_enroll;
      if (r.data.totp_enroll) {
        drawQr($('totp-login-qr'), r.data.qr);
        $('totp-login-secret').textContent = r.data.secret;
      }
      $('totp-code').value = '';
      $('totp-code').focus();
      show('', '');
      return;
    }
    if (r.status === 200) {
      csrf = r.data.csrf;
      pendingLogin = '';
      $('totp-card').hidden = true;
      $('totp-login-qr').replaceChildren();
      $('totp-login-secret').textContent = '';
      show('', '');
      if (r.data.recovery_codes) {
        // A token set up with this login: its recovery codes first
        fillRecovery($('recovery-list'), r.data.recovery_codes);
        $('login-card').hidden = true;
        $('recovery-card').hidden = false;
        return;
      }
      if (await loadState(true)) startRefresh();
      return;
    }
    if (r.status === 401 && pendingLogin && /log in again|expired/.test(r.data.error || '')) {
      pendingLogin = '';
      $('totp-card').hidden = true;
      $('login-card').hidden = false;
    }
    if (r.status === 429) {
      const wait = Number(r.data.retry_after) || 0;
      show(wait >= 120 ? t('Too many failed logins. Try again in {m} min.', { m: Math.ceil(wait / 60) })
        : t('Too many failed logins. Try again in {s} s.', { s: wait }), 'error');
    } else {
      show(ts(r.data.error) || t('Login failed'), 'error');
    }
  }

  $('login-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    setBusy(true);
    const body = { password: $('login-password').value };
    if (!$('login-user-row').hidden) body.user = $('login-user').value.trim();
    const r = await api('/api/login', body);
    setBusy(false);
    $('login-password').value = '';
    syncPasswordEyes();
    await loginDone(r);
  });
  $('totp-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    setBusy(true);
    const r = await api('/api/login/totp', { pending: pendingLogin, code: $('totp-code').value.trim() });
    setBusy(false);
    $('totp-code').value = '';
    await loginDone(r);
  });
  $('totp-cancel').addEventListener('click', () => {
    pendingLogin = '';
    $('totp-card').hidden = true;
    $('login-card').hidden = false;
    $('login-password').focus();
  });
  $('recovery-done').addEventListener('click', async () => {
    $('recovery-list').replaceChildren();
    $('recovery-card').hidden = true;
    if (await loadState(true)) startRefresh();
  });

  $('logout').addEventListener('click', async () => {
    await api('/api/logout', {});
    show(t('Logged out.'), 'ok');
    showLogin();
  });

  form.addEventListener('input', () => { formDirty = true; updateDependent(); markDefaults(); });
  form.addEventListener('change', () => { formDirty = true; updateDependent(); markDefaults(); });
  // The OTE settings sit in the Firmware and Configuration card, outside the settings form (form="settings-form"): their events
  // do not bubble through the form
  $('ote-card').addEventListener('input', () => { formDirty = true; updateDependent(); markDefaults(); });
  $('ote-card').addEventListener('change', () => { formDirty = true; updateDependent(); markDefaults(); });
  // The login settings of Security are fields of the settings form as well (form="settings-form")
  $('login-methods').addEventListener('input', () => { formDirty = true; updateDependent(); markDefaults(); });
  $('login-methods').addEventListener('change', () => { formDirty = true; updateDependent(); markDefaults(); });

  async function submitSettings(path) {
    let body;
    try {
      body = collect();
    } catch (err) {
      if (err.field) revealField(err.field);
      show(err.message, 'error');
      return;
    }
    setBusy(true);
    const r = await api(path, body);
    setBusy(false);
    if (r.status !== 200) {
      // Server messages of the form "Field <name>: ..." open the panel of that field
      const named = /^Field ([a-z0-9_]+):/.exec(String(r.data.error || ''));
      if (named) revealField(named[1]);
      show(ts(r.data.error) || t('Request failed ({status})', { status: r.status }), 'error');
      return;
    }
    if (path === '/api/apply' && r.data.restart) {
      stopAfterRestart(r.data.network_changed
        ? t('Applied. The device restarts once with these settings; confirm the network settings within 120 s after the start. Reload this page in about a minute.')
        : t('Applied. The device restarts once with these settings. Reload this page in about a minute.'));
      return;
    }
    if (path === '/api/apply') {
      show(r.data.network_changed ? t('Applied. Confirm the network settings within 120 s.') : t('Applied (not saved).'), 'ok');
      if (r.data.network_pending) renderPending(r.data.network_pending);
    } else {
      show(ts(r.data.message) || t('Saved.'), 'ok');
      // The banners follow the save at once (the firmware ended a one-shot start when the saved settings equal the
      // ones in use); the /api/state below says the same
      if (state && typeof r.data.unsaved_changes === 'boolean') {
        state.oneshot_boot = !!r.data.oneshot_boot;
        state.unsaved_changes = r.data.unsaved_changes;
        renderBanners();
      }
    }
    await loadState(true);
  }

  // ---- Configuration export and import ----
  // The export carries secrets: it is fetched with the CSRF header (a plain link could not send it) and handed to
  // the browser as a download
  $('config-export').addEventListener('click', async () => {
    let response;
    try {
      response = await fetch('/api/config', { credentials: 'same-origin', cache: 'no-store', headers: { 'X-CSRF-Token': csrf } });
    } catch (e) {
      show(t('The device does not answer (restarting or a different address?)'), 'error');
      return;
    }
    if (response.status === 401) { showLogin(); return; }
    if (response.status !== 200) {
      let data = {};
      try { data = await response.json(); } catch (e) { data = {}; }
      show(ts(data.error) || t('Request failed ({status})', { status: response.status }), 'error');
      return;
    }
    const blob = await response.blob();
    const d = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    const host = state && state.settings ? String(state.settings.fqdn).split('.')[0] : 'timeserver';
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = 'timeserver-config-' + host + '-' + d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + '.json';
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 10000);
    show(t('Configuration exported. The file contains secrets: keep it safe.'), 'ok');
  });

  $('config-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const file = $('config-file').files[0];
    if (!file) return;
    // The device takes at most 25600 bytes (CONFIG_BODY_MAX); a larger file is not even read
    if (file.size > 25600) {
      show(t('The file is too large for a configuration file (at most 25 kB).'), 'error');
      return;
    }
    let body;
    try {
      body = JSON.parse(await file.text());
    } catch (err) {
      show(t('The file is no JSON file.'), 'error');
      return;
    }
    if (!body || typeof body !== 'object' || Array.isArray(body)) {
      show(ts('This is no configuration file of ESP32 Time Server'), 'error');
      return;
    }
    if (!window.confirm(t('Replace all settings and the allow-list with those of {file}?', { file: file.name }))) return;
    setBusy(true);
    const r = await api('/api/config', body);
    setBusy(false);
    if (r.status !== 200) {
      show(ts(r.data.error) || t('Request failed ({status})', { status: r.status }), 'error');
      return;
    }
    $('config-file').value = '';
    const message = ts(r.data.message);
    if (r.data.restart) {
      stopAfterRestart(t('{message} Reload this page in about a minute.', { message: message }));
      return;
    }
    formDirty = false;
    show(message, 'ok');
    if (r.data.network_pending) renderPending(r.data.network_pending);
    await loadState(true);
  });

  $('btn-apply').addEventListener('click', () => submitSettings('/api/apply'));
  $('btn-save').addEventListener('click', () => submitSettings('/api/save'));

  $('ote-once').addEventListener('click', async () => {
    if (!window.confirm(t('Restart the device now with firmware updates enabled until its next restart?'))) return;
    setBusy(true);
    const r = await api('/api/ote/once', {});
    setBusy(false);
    if (r.status === 200) stopAfterRestart(t('{message} Reload this page and log in again in about a minute.', { message: ts(r.data.message) }));
    else show(ts(r.data.error) || t('Restart failed'), 'error');
  });

  $('btn-reboot').addEventListener('click', async () => {
    if (!window.confirm(t('Restart the device now? Applied settings that were not saved are discarded.'))) return;
    setBusy(true);
    const r = await api('/api/reboot', {});
    setBusy(false);
    if (r.status === 200) stopAfterRestart(t('{message} Reload this page in about a minute.', { message: ts(r.data.message) }));
    else show(ts(r.data.error) || t('Restart failed'), 'error');
  });

  $('net-confirm').addEventListener('click', async () => {
    const r = await api('/api/network/confirm', {});
    if (r.status === 200) {
      show(ts(r.data.message), 'ok');
      await loadState(false);
    } else {
      show(ts(r.data.error) || t('Confirmation failed'), 'error');
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
    if (!window.confirm(t('Erase all saved settings, the password and the allow-list, and restart?'))) return;
    setBusy(true);
    const r = await api('/api/factory-reset', { password: $('factory-password').value });
    setBusy(false);
    $('factory-password').value = '';
    syncPasswordEyes();
    if (r.status === 200) {
      $('factory-card').hidden = true;
      stopAfterRestart(t('{message} Log in with the default password after the restart.', { message: ts(r.data.message) }));
    } else {
      show(ts(r.data.error) || t('Reset to defaults failed'), 'error');
    }
  });

  $('password-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const current = $('pw-current').value;
    const next = $('pw-new').value;
    if (next !== $('pw-repeat').value) {
      show(t('The two new passwords differ.'), 'error');
      return;
    }
    if (next.length < 12 || next.length > 128) {
      show(t('The new password needs 12 to 128 characters.'), 'error');
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
      show(ts(r.data.message), 'ok');
      await loadState(false);
    } else {
      show(ts(r.data.error) || t('Password change failed'), 'error');
    }
  });

  $('allow-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const entries = $('allow-list').value.split(/\r?\n/).map((s) => s.trim()).filter((s) => s !== '');
    setBusy(true);
    const r = await api('/api/allow-list', { entries: entries });
    setBusy(false);
    show(r.status === 200 ? ts(r.data.message) : (ts(r.data.error) || t('Allow-list could not be stored')), r.status === 200 ? 'ok' : 'error');
    if (r.status === 200) await loadState(false);
  });

  // ---- TLS certificate of the web admin ----
  function tlsMode() {
    const checked = document.querySelector('input[name="tls_mode"]:checked');
    return checked ? checked.value : 'self';
  }

  function daysLeft(own) {
    if (own.days_left === null) return '';
    return ' ' + (own.days_left < 0 ? t('(expired {days} days ago)', { days: -own.days_left }) : t('({days} days left)', { days: own.days_left }));
  }

  function certificateText(tls) {
    if (!tls) return '';
    if (tls.mode !== 'own' || !tls.own) return t('self-signed');
    const o = tls.own;
    return t('own: {subject}, issued by {issuer}, valid until {date}', { subject: o.subject, issuer: o.issuer, date: o.not_after.slice(0, 10) }) + daysLeft(o);
  }

  // A name of the certificate (DNS name or IP address; "*." stands for one label) covers the host of this page
  function sanMatches(names, host) {
    host = host.replace(/^\[|\]$/g, '').toLowerCase();
    return (names || []).some((name) => {
      name = name.toLowerCase();
      if (name === host) return true;
      const dot = host.indexOf('.');
      return name.startsWith('*.') && dot > 0 && host.slice(dot + 1) === name.slice(2);
    });
  }

  // fill: the selection and the empty text boxes from the device (at the start and after storing); otherwise only
  // what follows from the selection, so the 10-s refresh does not touch what is being typed
  function renderTls(fill) {
    const tls = (state && state.tls) || { mode: 'self', own: null };
    if (fill) {
      for (const r of document.querySelectorAll('input[name="tls_mode"]')) r.checked = r.value === tls.mode;
      $('tls-cert').value = '';
      $('tls-key').value = '';
    }
    const own = tlsMode() === 'own';
    const o = tls.own;
    $('tls-self-note').hidden = own;
    $('tls-own-fields').hidden = !own;
    $('tls-cert-state').textContent = o ? t('An own certificate is stored (see below). Leave empty to keep it.')
      : t('At most 8192 characters. ECDSA (P-256, P-384) or RSA (2048 to 4096 bit).');
    $('tls-key-state').textContent = o ? t('A key is stored. Leave empty to keep it; it is never shown again.') : t('No key stored.');
    $('tls-info').hidden = !own || !o;
    const warnings = [];
    if (o) {
      $('tls-subject').textContent = o.subject;
      $('tls-san').textContent = o.san.length ? o.san.join(', ') : t('none');
      $('tls-issuer').textContent = o.issuer;
      $('tls-valid').textContent = t('{from} to {to}', { from: o.not_before.slice(0, 10), to: o.not_after.slice(0, 10) }) + daysLeft(o);
      $('tls-key-type').textContent = t('{type}, belongs to the certificate', { type: o.key_type }) +
        (o.chain_length > 1 ? ' · ' + t('{n} certificates in the chain', { n: o.chain_length }) : '');
      $('tls-fingerprint').textContent = o.fingerprint;
      if (!o.valid_now) warnings.push(o.days_left !== null && o.days_left < 0 ? t('The certificate has expired.') : t('The certificate is not valid yet.'));
      if (!sanMatches(o.san, location.hostname)) {
        warnings.push(t('The certificate does not contain the name or address this page is opened with ({host}).', { host: location.hostname }));
      }
    }
    $('tls-warn').textContent = warnings.join(' ');
    $('tls-warn').hidden = !own || warnings.length === 0;
  }

  for (const r of document.querySelectorAll('input[name="tls_mode"]')) r.addEventListener('change', () => renderTls(false));

  $('tls-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = { mode: tlsMode() };
    if (body.mode === 'own') {
      const cert = $('tls-cert').value.trim();
      const key = $('tls-key').value.trim();
      if (cert) body.certificate = cert + '\n';
      if (key) body.key = key + '\n';
    }
    setBusy(true);
    const r = await api('/api/tls', body);
    setBusy(false);
    if (r.status !== 200) {
      show(ts(r.data.error) || t('Request failed ({status})', { status: r.status }), 'error');
      return;
    }
    state.tls = r.data.tls;
    renderTls(true);
    show(ts(r.data.message), 'ok');
    // The device closed its connections: load the page again over a new one, with the certificate in use now
    setTimeout(() => location.reload(), 2500);
  });

  // ---- Firmware update (Over The Ethernet) ----
  let oteUploading = false;

  // ote_enabled of the settings in use (a restart field: the value of this boot)
  function renderOte() {
    const on = !!state.settings.ote_enabled;
    $('ote-off').hidden = on;
    // On for this start only: a one-shot start while the next start (saved settings, else defaults) has them off
    const next = state.saved || state.defaults;
    $('ote-once-on').hidden = !(on && state.oneshot_boot && !next.ote_enabled);
    $('ote-chip').textContent = (state.status.chip_model || 'ESP32-P4') + ' ' + state.status.chip_revision;
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
      show(t('Choose the firmware image (.bin) first.'), 'error');
      return;
    }
    const image = await imageVersion(file);
    if (!image) {
      show(t('This is not an application image: use ESP32TimeServer.bin of the build folder (not the merged flash image).'), 'error');
      return;
    }
    if (!window.confirm(t('Upload {project} {version} ({file}, {kb} kB) and restart the device with it? Running: {running}.', {
      project: image.project, version: image.version, file: file.name, kb: Math.round(file.size / 1024), running: state.status.firmware
    }))) return;

    oteUploading = true;
    clearInterval(refreshTimer); // the server answers nothing else during the upload
    clearInterval(diagTimer);
    clearInterval(ptpTimer);
    setBusy(true);
    $('ote-file').disabled = true;
    const progress = $('ote-progress');
    progress.value = 0;
    progress.hidden = false;
    $('ote-state').textContent = t('Uploading …');

    const finish = (status, data) => {
      oteUploading = false;
      setBusy(false);
      if (status === 200) {
        progress.value = 100;
        $('ote-state').textContent = ts(data.message) || '';
        $('ote-file').value = '';
        stopAfterRestart(t('{message} Reload this page in about a minute.', { message: ts(data.message) || t('Firmware accepted.') }));
        return;
      }
      progress.hidden = true;
      $('ote-state').textContent = '';
      if (status === 401) {
        showLogin();
        return;
      }
      show(ts(data.error) || t('Upload failed ({status})', { status: status }), 'error');
      startRefresh();
    };
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ote');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.setRequestHeader('X-CSRF-Token', csrf);
    xhr.upload.onprogress = (ev) => {
      if (!ev.lengthComputable) return;
      progress.value = Math.round((ev.loaded * 100) / ev.total);
      $('ote-state').textContent = ev.loaded < ev.total ? t('Uploading … {p} %', { p: progress.value }) : t('Checking and writing the image …');
    };
    xhr.onload = () => {
      let data = {};
      try { data = JSON.parse(xhr.responseText); } catch (err) { data = {}; }
      finish(xhr.status, data);
    };
    // The device closes the connection when it refuses the image before the whole file is sent (wrong project
    // or chip revision): the browser then only sees an interrupted upload
    xhr.onerror = () => finish(0, { error: t('The upload was interrupted. The device may have refused the image (the reason is on the LCD and the serial console), or it does not answer.') });
    xhr.send(file);
  });

  // ---- Start ----
  buildColourTable();
  loadState(true).then((ok) => { if (ok) startRefresh(); else showLogin(); });
})();
