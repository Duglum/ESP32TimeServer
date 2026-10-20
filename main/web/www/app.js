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

  // ---- Dark mode: stored in this browser only ----
  function storedTheme() {
    try { return localStorage.getItem('theme'); } catch (e) { return null; }
  }
  function applyTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    // The button shows the mode it switches to: U+2600 (sun) = light mode, U+1F319 (crescent moon) = dark mode
    const toggle = $('theme-toggle');
    toggle.textContent = theme === 'dark' ? '☀️' : '\u{1F319}';
    toggle.setAttribute('aria-label', theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode');
    toggle.title = toggle.getAttribute('aria-label');
  }
  // Dark mode is the default; a choice made with the button is kept in this browser
  applyTheme(storedTheme() === 'light' ? 'light' : 'dark');
  $('theme-toggle').addEventListener('click', () => {
    const next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    applyTheme(next);
    try { localStorage.setItem('theme', next); } catch (e) { /* storage not available */ }
  });

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
    $('app').hidden = true;
    $('logout').hidden = true;
    $('banner-default-pw').hidden = true;
    $('banner-network').hidden = true;
    $('banner-oneshot').hidden = true;
    $('login-card').hidden = false;
    $('login-password').focus();
  }

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
      gnss_tx_pin: String(s.gnss_tx_pin),
      gnss_rx_pin: String(s.gnss_rx_pin),
      gnss_pps_pin: String(s.gnss_pps_pin),
      gnss_baud: String(s.gnss_baud),
      ptp_loopback_pin: s.ptp_loopback_pin < 0 ? '' : String(s.ptp_loopback_pin),
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
      ntp_auth_enabled: s.ntp_auth_enabled,
      ntp_key_id: String(s.ntp_key_id)
    };
  }

  function fillForm(s) {
    const values = formValues(s);
    for (const name of Object.keys(values)) {
      const el = f[name];
      if (!el) continue;
      if (el.type === 'checkbox') el.checked = !!values[name];
      else el.value = values[name];
    }
    f.mqtt_password.value = '';
    f.mqtt_password_clear.checked = false;
    f.ntp_key.value = '';
    f.ntp_key_clear.checked = false;
    $('mqtt-password-state').textContent = s.mqtt_password_set ? 'A password is set.' : 'No password is set.';
    $('ntp-key-state').textContent = s.ntp_key_override
      ? 'A key set on this page is in use (ID ' + s.ntp_key_id + ').'
      : 'The keys compiled into the firmware are in use.';
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
      if (name === 'dns') text = text.replace(/\n/g, ', ');
      if (name === 'ptp_loopback_pin' && value === '') text = 'off';
      small.textContent = 'Default: ' + text;
      const differs = String(currentValue(name)).replace(/\r/g, '') !== String(value);
      small.classList.toggle('differs', differs);
      f[name].classList.toggle('is-changed', differs);
    });
  }

  function updateDependent() {
    const dhcp = f.ipv4_dhcp.checked;
    f.ipv4_cidr.disabled = dhcp;
    f.ipv4_gateway.disabled = dhcp;
    const type = f.led_type.value;
    document.querySelectorAll('.led-ky016').forEach((el) => { el.hidden = type !== 'ky016'; });
    document.querySelectorAll('.led-ws2812').forEach((el) => { el.hidden = type !== 'ws2812'; });
    f.led_brightness.disabled = type === 'none';
    $('brightness-out').textContent = f.led_brightness.value + ' %';
    f.mqtt_password.disabled = f.mqtt_password_clear.checked;
    f.ntp_key.disabled = f.ntp_key_clear.checked;
  }

  function splitCidr(text, defaultPrefix) {
    const parts = text.split('/');
    if (parts.length > 2) return null;
    const prefix = parts.length === 2 ? Number(parts[1]) : defaultPrefix;
    if (!Number.isInteger(prefix)) return null;
    return { address: parts[0].trim(), prefix: prefix };
  }

  function pin(name, allowEmpty) {
    const text = f[name].value.trim();
    if (text === '' && allowEmpty) return -1;
    const value = Number(text);
    if (!Number.isInteger(value)) throw new Error('GPIO number expected: ' + name.replace(/_/g, ' '));
    return value;
  }

  function collect() {
    const body = {
      fqdn: f.fqdn.value.trim(),
      ipv4_dhcp: f.ipv4_dhcp.checked,
      ipv6_slaac: f.ipv6_slaac.checked,
      ipv6_gateway: f.ipv6_gateway.value.trim(),
      gnss_tx_pin: pin('gnss_tx_pin'),
      gnss_rx_pin: pin('gnss_rx_pin'),
      gnss_pps_pin: pin('gnss_pps_pin'),
      gnss_baud: Number(f.gnss_baud.value),
      ptp_loopback_pin: pin('ptp_loopback_pin', true),
      lcd_enabled: f.lcd_enabled.checked,
      lcd_title: f.lcd_title.value,
      lcd_24_hour: f.lcd_24_hour.checked,
      lcd_pad_hour: f.lcd_pad_hour.checked,
      lcd_show_time_zone: f.lcd_show_time_zone.checked,
      time_zone: f.time_zone.value.trim(),
      led_type: f.led_type.value,
      led_brightness: Number(f.led_brightness.value),
      led_red_pin: pin('led_red_pin'),
      led_green_pin: pin('led_green_pin'),
      led_blue_pin: pin('led_blue_pin'),
      led_ws2812_pin: pin('led_ws2812_pin'),
      mqtt_enabled: f.mqtt_enabled.checked,
      mqtt_uri: f.mqtt_uri.value.trim(),
      mqtt_username: f.mqtt_username.value,
      mqtt_password: f.mqtt_password.value,
      mqtt_password_clear: f.mqtt_password_clear.checked,
      ntp_auth_enabled: f.ntp_auth_enabled.checked,
      ntp_key_id: Number(f.ntp_key_id.value),
      ntp_key: f.ntp_key.value,
      ntp_key_clear: f.ntp_key_clear.checked
    };
    if (!body.ipv4_dhcp) {
      const v4 = splitCidr(f.ipv4_cidr.value.trim(), NaN);
      if (!v4) throw new Error('IPv4 address: a.b.c.d/prefix expected');
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
      if (!v6) throw new Error('IPv6 address: address/prefix expected');
      body.ipv6_address = v6.address;
      body.ipv6_prefix = v6.prefix;
    }
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

  function renderStatus() {
    const st = state.status;
    $('hdr-host').textContent = state.settings.fqdn;
    $('st-firmware').textContent = st.firmware + ' (' + st.build + ')';
    $('st-uptime').textContent = duration(st.uptime_s);
    $('st-utc').textContent = st.utc || 'not set yet';
    yesNo($('st-time'), st.stratum_one, 'stratum 1, GNSS and PPS locked',
      st.time_set ? 'not stratum 1 (PPS ' + (st.pps_active ? 'active' : 'missing') + ', GNSS ' + (st.gnss_locked ? 'locked' : 'not locked') + ')' : 'waiting for GNSS time');
    $('st-gnss').textContent = (st.ublox_receiver ? 'u-blox protocol, baud rate detected automatically' : 'NMEA') +
      (st.gnss_baud_in_use ? ', ' + st.gnss_baud_in_use + ' baud' : '');
    $('baud-note').textContent = st.ublox_receiver ? 'Only used with a receiver that is not u-blox compatible (UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED 0).' : '';
    $('st-ipv4').textContent = st.ipv4_address
      ? st.ipv4_address + '/' + st.ipv4_prefix + ' via ' + (st.ipv4_gateway || '-') + (st.ipv4_dhcp_running ? ' (from DHCP)' : ' (static)')
      : (st.ipv4_dhcp_running ? 'waiting for DHCP' : 'none');
    const list = $('st-ipv6');
    list.replaceChildren();
    for (const a of st.ipv6) {
      const li = document.createElement('li');
      li.textContent = a;
      li.className = 'mono small';
      list.appendChild(li);
    }
    if (st.ipv6.length === 0) list.textContent = 'none';
    $('st-dns').textContent = st.dns.length ? st.dns.join(', ') : 'none';
    $('st-mqtt').textContent = state.settings.mqtt_enabled ? (st.mqtt_connected ? 'connected' : 'not connected') : 'off';
    $('st-auth').textContent = st.ntp_auth_available ? 'on' : (state.settings.ntp_auth_enabled ? 'on, but no usable key' : 'off');
    $('st-heap').textContent = Math.round(st.free_heap / 1024) + ' kB';
    $('st-client').textContent = st.client;
    $('st-fingerprint').textContent = state.fingerprint;

    $('banner-default-pw').hidden = !state.default_password;
    $('banner-oneshot').hidden = !state.oneshot_boot;
    renderPending(state.network_pending);
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
      const lines = state.allow_list || [];
      $('allow-list').value = lines.join('\n');
    }
    return true;
  }

  function startRefresh() {
    clearInterval(refreshTimer);
    refreshTimer = setInterval(() => { if (state) loadState(false); }, 10000);
  }

  function setBusy(busy) {
    document.querySelectorAll('button').forEach((b) => { if (b.id !== 'theme-toggle') b.disabled = busy; });
  }

  // ---- Actions ----
  $('login-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    setBusy(true);
    const r = await api('/api/login', { password: $('login-password').value });
    setBusy(false);
    $('login-password').value = '';
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
      show('Applied. The device restarts once with these settings' +
        (r.data.network_changed ? '; the network settings have to be confirmed within 120 s after the start' : '') +
        '. Reload this page in about a minute.', 'warn');
      clearInterval(refreshTimer);
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
    $('factory-password').focus();
  });
  $('factory-cancel').addEventListener('click', () => {
    $('factory-card').hidden = true;
    $('factory-password').value = '';
  });
  $('factory-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    if (!window.confirm('Erase all saved settings, the password and the allow-list, and restart?')) return;
    setBusy(true);
    const r = await api('/api/factory-reset', { password: $('factory-password').value });
    setBusy(false);
    $('factory-password').value = '';
    if (r.status === 200) {
      $('factory-card').hidden = true;
      show(r.data.message + ' Log in with the compiled password after the restart.', 'warn');
      clearInterval(refreshTimer);
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

  // ---- Start ----
  loadState(true).then((ok) => { if (ok) startRefresh(); else showLogin(); });
})();
