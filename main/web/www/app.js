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
      return { status: 0, data: { error: t('The device does not answer (restarting or a different address?)') } };
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
    api('/api/info').then((r) => { if (r.status === 200) setHost(r.data.fqdn || r.data.hostname); });
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
      ? t('No keys are set on this page: these are the keys compiled into the firmware. Changing them here stores them as keys of the web settings.')
      : t('Keys set on this page are in use.');
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
      ote_enabled: s.ote_enabled
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
    $('mqtt-password-state').textContent = s.mqtt_password_set ? t('A password is set.') : t('No password is set.');
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
      if (name === 'pps_holdover' || name === 'gnss_min_fix_age') text = value === '0' ? t('off') : text + ' s';
      if (name === 'gnss_min_satellites' && value === '0') text = t('off');
      if (name === 'pps_lost_serve_min') text = value === '0' ? t('no limit') : text + ' min';
      if (name === 'ntp_client_rate_limit') text = value === '0' ? t('off') : t('{n} per second', { n: value });
      if (name === 'pps_dispersion') text = (lang === 'de' ? text.replace('.', ',') : text) + ' ms/s';
      if (name === 'mac_address' && !state.defaults.mac_address) text = t('the hardware MAC');
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

  // Collapsed settings panels: the heading row shows how many visible values inside differ from the compiled default
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

  // Collapsed GPIO pin areas: the summary shows how many visible pin values differ from the compiled default and
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
    const kilobytes = ((1166 + 76 * limit) / 1024).toFixed(1);
    $('mqtt-buffer-note').textContent = f.mqtt_client_reporting.checked && Number.isInteger(limit) && limit >= 50 && limit <= 1000
      ? t('Report buffer: {kb} kB of RAM (1166 bytes plus 76 bytes per client). Without the TF card at most 50 clients are listed.', { kb: lang === 'de' ? kilobytes.replace('.', ',') : kilobytes })
      : (f.mqtt_client_reporting.checked ? t('50 to 1000.') : t('Only used with client activity reporting.'));
    const compiledOnly = f.ntp_key_clear.checked;
    document.querySelectorAll('#ntp-keys input, #ntp-keys button').forEach((el) => { el.disabled = compiledOnly; });
    $('ntp-key-add').disabled = compiledOnly || ntpKeyRows.length >= 8;
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
      ote_enabled: f.ote_enabled.checked
    };
    // NTP keys: only sent when they were changed (otherwise the compiled keys stay in use if they are)
    if (!body.ntp_key_clear && ntpKeysChanged) {
      body.ntp_keys = [];
      ntpKeyRows.forEach((row, i) => {
        const id = Number(row.id.value);
        const key = row.key.value.trim();
        if (!Number.isInteger(id) || id < 1 || id > 65535) throw fieldError('ntp-keys', t('NTP key {n}: key ID 1 to 65535', { n: i + 1 }));
        if (!/^[0-9A-Za-z]{32}$/.test(key)) throw fieldError('ntp-keys', t('NTP key {n}: exactly 32 letters or digits', { n: i + 1 }));
        body.ntp_keys.push({ id: id, key: key });
      });
    }
    for (const [key, name] of ledStates) {
      const hex = normaliseHex(colourRows[key].input.value);
      if (!hex) throw fieldError('led-colours', t('LED colour "{name}": #RRGGBB expected', { name: t(name) }));
      body.led_colors[key] = hex;
    }
    if (!body.ipv4_dhcp) {
      const v4 = splitCidr(f.ipv4_cidr.value.trim(), NaN);
      if (!v4) throw fieldError('ipv4_cidr', t('IPv4 address: a.b.c.d/prefix length expected'));
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
    // MAC: lower case with ':'; the hardware MAC is sent as "" (= hardware MAC)
    const mac = normaliseMac(f.mac_address.value);
    if (mac === null) throw fieldError('mac_address', t('MAC address: xx:xx:xx:xx:xx:xx, xx-xx-xx-xx-xx-xx, xxxx.xxxx.xxxx or xxxxxxxxxxxx (hexadecimal) expected'));
    if (mac !== '' && (parseInt(mac.slice(0, 2), 16) & 1) !== 0) throw fieldError('mac_address', t('MAC address: a unicast address is required (the lowest bit of the first byte must be 0)'));
    body.mac_address = mac === state.status.mac_hardware ? '' : mac;
    body.dns = f.dns.value.split(/\r?\n/).map((s) => s.trim()).filter((s) => s !== '');
    if (body.dns.length > 3) throw fieldError('dns', t('At most 3 DNS servers'));
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
    // U-Blox UBX protocol: NAV-PVT and the qErr correction of the time pulse (TIM-TP)
    if (st.gnss_pvt_satellites >= 0) gnss.push(t('{n} satellites in use (NAV-PVT)', { n: st.gnss_pvt_satellites }));
    if (st.ublox_receiver && st.gnss_pvt_satellites >= 0) {
      const slope = Number(st.gnss_qerr_slope).toFixed(2);
      gnss.push(st.gnss_qerr_state === 1 ? t('qErr correction: learning the sign')
        : st.gnss_qerr_state === 2 ? t('qErr correction active (slope {slope})', { slope })
          : st.gnss_qerr_state === 3 ? t('qErr correction off: no correlation (slope {slope})', { slope })
            : t('no qErr (TIM-TP missing or PTP loopback off)'));
    }
    fillList('st-gnss', gnss, '', false);
    const keys = st.ntp_auth_keys_in_use;
    $('st-auth').textContent = st.ntp_auth_available
      ? (state.settings.ntp_key_override
        ? t(keys === 1 ? 'on, {n} key of the web settings' : 'on, {n} keys of the web settings', { n: keys })
        : t(keys === 1 ? 'on, {n} key compiled in' : 'on, {n} keys compiled in', { n: keys }))
      : (state.settings.ntp_auth_enabled ? t('on, but no usable key') : t('off'));
    // Requests dropped by the per-client rate limit since the start (also those dropped before it was switched off)
    const limit = state.settings.ntp_client_rate_limit;
    $('st-rate-limit').textContent = limit
      ? t('{limit} per second and client, {n} requests dropped since the start', { limit: limit, n: st.ntp_rate_limited })
      : t('off, {n} requests dropped since the start', { n: st.ntp_rate_limited });
    $('st-firmware').textContent = st.firmware + ' (' + st.build + ')';
    $('st-chip').textContent = 'ESP32-P4 ' + st.chip_revision;
    $('ptp-chip').textContent = st.chip_revision;
    $('st-uptime').textContent = duration(st.uptime_s);
    $('st-heap').textContent = Math.round(st.free_heap / 1024) + ' kB';
    const mqttState = st.mqtt_connected ? t('connected') : t('not connected');
    $('st-mqtt').textContent = state.settings.mqtt_enabled
      ? (st.mqtt_tf_queue ? t('{state}, reports queued on the TF card', { state: mqttState }) : mqttState)
      : t('off');
    $('st-mac').textContent = st.mac_address === st.mac_hardware
      ? t('{mac} (hardware MAC)', { mac: st.mac_address })
      : t('{mac} (set, hardware MAC {hardware})', { mac: st.mac_address, hardware: st.mac_hardware });
    $('mac-hardware').textContent = st.mac_hardware;
    f.mac_address.placeholder = st.mac_hardware;
    $('st-fingerprint').textContent = state.fingerprint;
    // Same form as the IPv6 lines: address in the first line, gateway and source in the second
    fillList('st-ipv4', st.ipv4_address
      ? [st.ipv4_address + '/' + st.ipv4_prefix + ' (' + t('via {gateway}, {source}', { gateway: st.ipv4_gateway || '-', source: st.ipv4_dhcp_running ? 'DHCP' : t('static') }) + ')']
      : [], st.ipv4_dhcp_running ? t('waiting for DHCP') : t('none'), true);
    fillList('st-ipv6', st.ipv6, t('none'), true);
    fillList('st-dns', st.dns, t('none'), true);
    fillList('st-dns-v4', st.dns_received_v4, t('none'), true);
    fillList('st-dns-v6', st.dns_received_v6, t('none'), true);
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
      show(t('Too many failed logins. Try again in {s} s.', { s: r.data.retry_after }), 'error');
    } else {
      show(ts(r.data.error) || t('Login failed'), 'error');
    }
  });

  $('logout').addEventListener('click', async () => {
    await api('/api/logout', {});
    show(t('Logged out.'), 'ok');
    showLogin();
  });

  form.addEventListener('input', () => { formDirty = true; updateDependent(); markDefaults(); });
  form.addEventListener('change', () => { formDirty = true; updateDependent(); markDefaults(); });
  // The OTE settings sit in the Firmware Update card, outside the settings form (form="settings-form"): their events
  // do not bubble through the form
  $('ote-card').addEventListener('input', () => { formDirty = true; updateDependent(); markDefaults(); });
  $('ote-card').addEventListener('change', () => { formDirty = true; updateDependent(); markDefaults(); });

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
    }
    await loadState(true);
  }

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
      stopAfterRestart(t('{message} Log in with the compiled password after the restart.', { message: ts(r.data.message) }));
    } else {
      show(ts(r.data.error) || t('Factory reset failed'), 'error');
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

  // ---- Firmware update (Over The Ethernet) ----
  let oteUploading = false;

  // ote_enabled of the settings in use (a restart field: the value of this boot)
  function renderOte() {
    const on = !!state.settings.ote_enabled;
    $('ote-off').hidden = on;
    // On for this start only: a one-shot start while the next start (saved settings, else defaults) has them off
    const next = state.saved || state.defaults;
    $('ote-once-on').hidden = !(on && state.oneshot_boot && !next.ote_enabled);
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
