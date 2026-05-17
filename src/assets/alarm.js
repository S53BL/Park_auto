// ============================================================
// alarm.js — Stran: Alarm
// Polling 3s — GET /api/alarm
// POST /api/alarm (arm/disarm), /api/alarm/test, /api/alarm/pin
// ============================================================
(function () {
  const DIV = 'page-alarm';

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Alarm</h1>
  <span class="subtitle" id="alarm-ts">–</span>
</div>

<!-- STATUS ─────────────────────────────────── -->
<div class="card" id="alarm-status-card" style="margin-bottom:12px">
  <div style="display:flex;align-items:center;gap:14px">
    <div style="flex:1">
      <div class="card-title">Stanje</div>
      <div class="metric loading" id="alarm-state-lbl">–</div>
    </div>
    <span class="badge badge-gray" id="alarm-badge">–</span>
  </div>
  <div class="kv-list mt12">
    <div class="kv-row"><span class="kv-key">Sprožitve</span>   <span class="kv-val text-mono" id="alarm-triggers">–</span></div>
    <div class="kv-row"><span class="kv-key">Preostalo</span>   <span class="kv-val text-mono" id="alarm-remaining">–</span></div>
    <div class="kv-row"><span class="kv-key">Grace period</span><span class="kv-val text-mono" id="alarm-grace">–</span></div>
    <div class="kv-row"><span class="kv-key">Callback</span>   <span class="kv-val"           id="alarm-cb-stat">–</span></div>
    <div class="kv-row"><span class="kv-key">PIN</span>        <span class="kv-val text-mono"  id="alarm-pinlen">–</span></div>
  </div>
</div>

<!-- IZKLOP (ko je alarm aktiven) ────────────── -->
<div class="card" id="alarm-disarm-card" style="display:none;margin-bottom:12px;border-color:rgba(239,68,68,0.4)">
  <div class="card-title">Izklop alarma</div>
  <p class="form-hint" style="margin-bottom:12px">Alarm je aktiven. Izklop prek spleta ne zahteva PIN kode.<br>
     Na LCD zaslonu je potreben PIN vnos.</p>
  <div style="display:flex;gap:10px;flex-wrap:wrap">
    <button class="btn btn-danger" onclick="alarmDisarm()">Izklopi alarm</button>
    <button class="btn"           onclick="alarmTest()">Test blink</button>
  </div>
</div>

<!-- VKLOP (ko alarm ni aktiven) ─────────────── -->
<div class="card" id="alarm-arm-card" style="margin-bottom:12px">
  <div class="card-title">Vklop alarma</div>
  <div class="form-row">
    <div class="flex-col">
      <div class="form-label">Trajanje (sekunde, 0 = trajno)</div>
      <div class="form-hint">0 = aktiven dokler ne izklopite ročno</div>
    </div>
    <input class="form-input" type="number" id="alarm-in-dur"
           value="0" min="0" max="86400" step="60">
  </div>
  <div class="form-row">
    <div class="flex-col">
      <div class="form-label">Grace period (sekunde)</div>
      <div class="form-hint">Čas med arm ukazom in aktivacijo</div>
    </div>
    <input class="form-input" type="number" id="alarm-in-grace"
           value="30" min="10" max="600">
  </div>
  <div class="form-row" style="border-bottom:none;flex-wrap:wrap;gap:6px">
    <div class="flex-col" style="min-width:120px">
      <div class="form-label">Callback URL <span class="text-dim text-tiny">(opcijsko)</span></div>
      <div class="form-hint">HTTP POST ob spremembi stanja</div>
    </div>
    <input class="form-input wide" type="text" id="alarm-in-cb"
           placeholder="http://ha.local/webhook" autocomplete="off" style="flex:1;width:auto">
  </div>
  <div style="display:flex;gap:10px;flex-wrap:wrap;margin-top:12px">
    <button class="btn btn-primary" onclick="alarmArm()">Vklopi alarm</button>
    <button class="btn"            onclick="alarmTest()">Test blink</button>
  </div>
  <div class="text-dim text-tiny mt8" id="alarm-arm-st"></div>
</div>

<!-- PIN NASTAVITVE ───────────────────────────── -->
<div class="card">
  <div class="card-title">PIN koda (LCD zaslon)</div>
  <div class="form-row">
    <div class="form-label">Nova koda <span class="text-dim text-tiny">(4–8 številk)</span></div>
    <input class="form-input" type="password" id="alarm-pin1"
           maxlength="8" placeholder="••••" autocomplete="new-password">
  </div>
  <div class="form-row" style="border-bottom:none">
    <div class="form-label">Potrdi kodo</div>
    <input class="form-input" type="password" id="alarm-pin2"
           maxlength="8" placeholder="••••" autocomplete="new-password">
  </div>
  <div style="display:flex;justify-content:flex-end;margin-top:12px">
    <button class="btn btn-primary" onclick="alarmSetPin()">Shrani PIN</button>
  </div>
  <div class="text-dim text-tiny mt8" id="alarm-pin-st"></div>
</div>
`;
  }

  // ── Pomožne funkcije ─────────────────────────────────────
  function _fmtTime(s) {
    if (!s || s <= 0) return '0 s';
    const h   = Math.floor(s / 3600);
    const m   = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    if (h > 0) return h + 'h ' + String(m).padStart(2, '0') + 'm';
    if (m > 0) return m + 'm ' + String(sec).padStart(2, '0') + 's';
    return sec + ' s';
  }

  function _setStatus(elId, msg, ok) {
    const el = document.getElementById(elId);
    if (!el) return;
    el.textContent = msg;
    el.style.color = ok === true  ? 'var(--green)'
                   : ok === false ? 'var(--red)'
                   :                'var(--text3)';
  }

  // ── Posodobi UI iz /api/alarm odgovora ───────────────────
  function _update(d) {
    const badge = document.getElementById('alarm-badge');
    const lbl   = document.getElementById('alarm-state-lbl');
    const card  = document.getElementById('alarm-status-card');

    if (d.state === 'TRIGGERED') {
      badge.className = 'badge badge-red';
      badge.textContent = 'SPROŽI';
      lbl.className = 'metric err';
      lbl.textContent = 'ALARM SPROŽEN';
      card.style.borderColor = 'rgba(239,68,68,0.5)';
    } else if (d.state === 'ARMED') {
      badge.className = 'badge badge-amber';
      badge.textContent = 'AKTIVEN';
      lbl.className = 'metric warn';
      lbl.textContent = 'Alarm aktiven';
      card.style.borderColor = 'rgba(245,158,11,0.4)';
    } else {
      badge.className = 'badge badge-gray';
      badge.textContent = 'OFF';
      lbl.className = 'metric';
      lbl.textContent = 'Izklopljen';
      card.style.borderColor = '';
    }

    document.getElementById('alarm-triggers').textContent =
      d.trigger_count != null ? d.trigger_count : '–';
    document.getElementById('alarm-grace').textContent =
      d.grace_s ? d.grace_s + ' s' : '–';
    document.getElementById('alarm-pinlen').textContent =
      d.pin_len ? d.pin_len + ' znakov' : 'ni nastavljen';
    document.getElementById('alarm-cb-stat').innerHTML = d.callback_url_set
      ? '<span class="badge badge-green">Nastavljen</span>'
      : '<span class="badge badge-gray">Ni nastavljen</span>';

    if (d.active && d.permanent) {
      document.getElementById('alarm-remaining').textContent = '∞ trajno';
    } else if (d.active && d.remaining_s > 0) {
      document.getElementById('alarm-remaining').textContent = _fmtTime(d.remaining_s);
    } else {
      document.getElementById('alarm-remaining').textContent = '–';
    }

    document.getElementById('alarm-arm-card').style.display    = d.active ? 'none'  : 'block';
    document.getElementById('alarm-disarm-card').style.display = d.active ? 'block' : 'none';

    if (d.grace_s && !d.active) {
      document.getElementById('alarm-in-grace').value = d.grace_s;
    }

    document.getElementById('alarm-ts').textContent =
      new Date().toLocaleTimeString();
  }

  // ── Poll ─────────────────────────────────────────────────
  async function _poll() {
    try {
      _update(await api.get('/api/alarm'));
    } catch(e) {
      const ts = document.getElementById('alarm-ts');
      if (ts) ts.textContent = 'napaka: ' + e.message;
    }
  }

  // ── Akcije (klic iz onclick) ─────────────────────────────
  window.alarmArm = async function() {
    const duration_s   = parseInt(document.getElementById('alarm-in-dur').value)   || 0;
    const grace_s      = parseInt(document.getElementById('alarm-in-grace').value) || 30;
    const callback_url = document.getElementById('alarm-in-cb').value.trim();

    if (grace_s < 10 || grace_s > 600) {
      _setStatus('alarm-arm-st', 'Grace period mora biti 10–600 s', false);
      return;
    }
    _setStatus('alarm-arm-st', 'Vklapljam…');
    try {
      await api.post('/api/alarm', { state: 'on', duration_seconds: duration_s, callback_url, grace_s });
      _setStatus('alarm-arm-st', 'Alarm vklopljen ✓', true);
      _poll();
    } catch(e) { _setStatus('alarm-arm-st', 'Napaka: ' + e.message, false); }
  };

  window.alarmDisarm = async function() {
    try {
      await api.post('/api/alarm', { state: 'off' });
      _poll();
    } catch(e) { _setStatus('alarm-arm-st', 'Napaka: ' + e.message, false); }
  };

  window.alarmTest = async function() {
    try {
      await api.post('/api/alarm/test', {});
    } catch(e) { _setStatus('alarm-arm-st', 'Napaka: ' + e.message, false); }
  };

  window.alarmSetPin = async function() {
    const p1 = document.getElementById('alarm-pin1').value;
    const p2 = document.getElementById('alarm-pin2').value;
    if (p1 !== p2)                     { _setStatus('alarm-pin-st', 'PIN kodi se ne ujemata', false);     return; }
    if (p1.length < 4 || p1.length > 8){ _setStatus('alarm-pin-st', 'PIN mora biti 4–8 znakov', false);  return; }
    if (!/^\d+$/.test(p1))             { _setStatus('alarm-pin-st', 'Samo številke so dovoljene', false); return; }
    _setStatus('alarm-pin-st', 'Shranjujem…');
    try {
      await api.post('/api/alarm/pin', { pin: p1 });
      _setStatus('alarm-pin-st', 'PIN shranjen ✓', true);
      document.getElementById('alarm-pin1').value = '';
      document.getElementById('alarm-pin2').value = '';
      _poll();
    } catch(e) { _setStatus('alarm-pin-st', 'Napaka: ' + e.message, false); }
  };

  // ── Init (kliče ga router) ───────────────────────────────
  window.page_alarm = function() {
    if (!document.getElementById('alarm-state-lbl')) _render();
    registerPoller('alarm', _poll, 3000);
  };
})();
