// ============================================================
// party.js — Stran: Party Mode
// Panel A: Upravljanje real-time (toggle, sloti, barva, bri/spd)
// Panel B: Konfiguracija slotov (ime, efekt, barva, bri, spd)
// Panel C: Urniki (date_range, čas/lux vklop/izklop)
// Panel D: Nastavitve (WLED IP, resume_delay_s)
// ============================================================
(function () {
  const DIV = 'page-party';

  // Stanje
  let _wledIp      = '';
  let _wledEffects = [];   // [{id, name}]
  let _slots       = [];   // 9 slotov
  let _schedules   = [];   // 10 urnikov
  let _status      = { party_on: false, active_slot: 0xFF, suspended: false, wled_on: false, resume_delay_s: 30 };

  const CLR_PALETTE = [
    { rgb: '#ffffff', val: 0xFFFFFF, lbl: 'Bela'      },
    { rgb: '#ff2020', val: 0xFF2020, lbl: 'Rdeča'     },
    { rgb: '#ff8000', val: 0xFF8000, lbl: 'Oranžna'   },
    { rgb: '#ffdd00', val: 0xFFDD00, lbl: 'Rumena'    },
    { rgb: '#22cc22', val: 0x22CC22, lbl: 'Zelena'    },
    { rgb: '#2266ff', val: 0x2266FF, lbl: 'Modra'     },
    { rgb: '#aa00cc', val: 0xAA00CC, lbl: 'Vijolična' },
  ];

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Party Mode</h1>
  <span class="subtitle" id="party-ts">–</span>
</div>

<!-- TAB NAVIGACIJA ──────────────────────── -->
<div class="tab-bar" id="party-tab-bar">
  <button class="tab-btn active" onclick="partyTab('a')">Upravljanje</button>
  <button class="tab-btn"       onclick="partyTab('b')">Sloti</button>
  <button class="tab-btn"       onclick="partyTab('c')">Urniki</button>
  <button class="tab-btn"       onclick="partyTab('d')">Nastavitve</button>
</div>

<!-- ── PANEL A — Upravljanje ─────────────── -->
<div id="party-panel-a">

  <!-- Toggle + status -->
  <div class="card" style="margin-bottom:12px">
    <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
      <div style="flex:1;min-width:160px">
        <div class="card-title">Party Mode</div>
        <div class="metric" id="party-state-lbl">–</div>
      </div>
      <div style="display:flex;flex-direction:column;gap:6px;align-items:flex-end">
        <button class="btn btn-primary" id="party-toggle-btn" onclick="partyToggle()">–</button>
        <span class="badge badge-gray" id="party-suspended-badge" style="display:none">SUSPENDIRAN</span>
      </div>
    </div>
    <div class="kv-list mt12">
      <div class="kv-row"><span class="kv-key">Aktiven slot</span><span class="kv-val text-mono" id="party-active-slot">–</span></div>
      <div class="kv-row"><span class="kv-key">WLED stanje</span><span class="kv-val" id="party-wled-state">–</span></div>
    </div>
  </div>

  <!-- 3×3 sloti -->
  <div class="card" style="margin-bottom:12px">
    <div class="card-title">Sloti</div>
    <div id="party-slot-grid" style="display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:10px">
      ${Array.from({length:9}, (_,i) =>
        `<button class="btn party-slot-btn" id="pslot-${i}" onclick="partyActivateSlot(${i})">Slot ${i}</button>`
      ).join('')}
    </div>
  </div>

  <!-- Barva -->
  <div class="card" style="margin-bottom:12px">
    <div class="card-title">Barva</div>
    <div style="display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;align-items:center">
      ${CLR_PALETTE.map((c,i) =>
        `<button class="party-color-dot" id="pclr-${i}" title="${c.lbl}"
           style="background:${c.rgb}"
           onclick="partySetColor(${c.val},${i})"></button>`
      ).join('')}
    </div>
  </div>

  <!-- Svetlost + Hitrost -->
  <div class="card">
    <div class="form-row">
      <div class="form-label">Svetlost</div>
      <div style="display:flex;align-items:center;gap:10px;flex:1">
        <input type="range" id="party-bri-sl" min="0" max="255" value="191"
               style="flex:1" onchange="partySetBri(this.value)">
        <span class="text-mono" id="party-bri-val" style="min-width:30px">191</span>
      </div>
    </div>
    <div class="form-row" style="border-bottom:none">
      <div class="form-label">Hitrost</div>
      <div style="display:flex;align-items:center;gap:10px;flex:1">
        <input type="range" id="party-spd-sl" min="0" max="255" value="128"
               style="flex:1" onchange="partySetSpd(this.value)">
        <span class="text-mono" id="party-spd-val" style="min-width:30px">128</span>
      </div>
    </div>
  </div>

</div>

<!-- ── PANEL B — Konfiguracija slotov ────── -->
<div id="party-panel-b" style="display:none">

  <div class="card" style="margin-bottom:12px">
    <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center">
      <button class="btn" onclick="partyLoadEffects()">Naloži efekte iz WLED</button>
      <button class="btn" id="party-wled-link" onclick="partyOpenWled()">Napredne nastavitve WLED ↗</button>
    </div>
    <div class="text-dim text-tiny mt8" id="party-fx-status"></div>
  </div>

  <div id="party-slots-cfg"></div>

</div>

<!-- ── PANEL C — Urniki ──────────────────── -->
<div id="party-panel-c" style="display:none">

  <div style="display:flex;justify-content:flex-end;margin-bottom:10px">
    <button class="btn btn-primary" onclick="partyNewSched()">+ Nov urnik</button>
  </div>
  <div id="party-schedules-list"></div>

</div>

<!-- ── PANEL D — Nastavitve ──────────────── -->
<div id="party-panel-d" style="display:none">

  <div class="card">
    <div class="card-title">WLED konfiguracija</div>
    <div class="form-row">
      <div class="form-label">WLED IP naslov</div>
      <input class="form-input" type="text" id="party-cfg-ip"
             placeholder="192.168.2.171" autocomplete="off">
    </div>
    <div class="form-row" style="border-bottom:none">
      <div class="flex-col">
        <div class="form-label">Resume zakasnitev (s)</div>
        <div class="form-hint">Čas mirovanja pred nadaljevanjem party po prekinitvi</div>
      </div>
      <div style="display:flex;align-items:center;gap:10px">
        <input type="range" id="party-cfg-resume" min="5" max="300" value="30"
               style="width:120px" oninput="document.getElementById('party-cfg-resume-val').textContent=this.value">
        <span class="text-mono" id="party-cfg-resume-val">30</span> s
      </div>
    </div>
    <div style="display:flex;justify-content:flex-end;margin-top:12px">
      <button class="btn btn-primary" onclick="partySaveCfg()">Shrani</button>
    </div>
    <div class="text-dim text-tiny mt8" id="party-cfg-st"></div>
  </div>

</div>

<style>
.tab-bar { display:flex; gap:4px; margin-bottom:12px; }
.tab-btn { flex:1; padding:8px 4px; border:1px solid var(--border);
           background:var(--card); color:var(--text2); border-radius:6px;
           cursor:pointer; font-size:13px; }
.tab-btn.active { background:var(--blue); color:#fff; border-color:var(--blue); }
.party-slot-btn { width:100%; padding:10px 4px; font-size:12px;
                  border:1px solid var(--border); background:var(--card);
                  color:var(--text2); border-radius:6px; cursor:pointer; }
.party-slot-btn.active { border-color:var(--cyan,#06b6d4); color:var(--cyan,#06b6d4);
                         background:rgba(6,182,212,0.12); }
.party-slot-btn:disabled { opacity:0.35; cursor:default; }
.party-color-dot { width:34px; height:34px; border-radius:50%;
                   border:2px solid transparent; cursor:pointer; padding:0; }
.party-color-dot.sel { border-color:#fff; transform:scale(1.15); }
.sched-card { background:var(--card); border:1px solid var(--border);
              border-radius:8px; padding:14px; margin-bottom:10px; }
</style>
`;
  }

  // ── Tab navigacija ───────────────────────────────────────
  window.partyTab = function(tab) {
    ['a','b','c','d'].forEach(t => {
      document.getElementById('party-panel-' + t).style.display = t === tab ? '' : 'none';
    });
    document.querySelectorAll('#party-tab-bar .tab-btn').forEach((btn, i) => {
      btn.classList.toggle('active', ['a','b','c','d'][i] === tab);
    });
    if (tab === 'b') _renderSlotsCfg();
    if (tab === 'c') _renderSchedules();
    if (tab === 'd') _renderCfg();
  };

  // ── Status posodobitev ───────────────────────────────────
  function _applyStatus(s) {
    _status = s;
    const on  = s.party_on;
    const susp = s.suspended;

    const lbl = document.getElementById('party-state-lbl');
    const btn = document.getElementById('party-toggle-btn');
    const sus = document.getElementById('party-suspended-badge');
    if (lbl) lbl.textContent = on ? (susp ? 'Suspendiran' : 'Aktiven') : 'Izklopljen';
    if (lbl) lbl.className = 'metric' + (on && !susp ? ' ok' : susp ? ' warn' : '');
    if (btn) { btn.textContent = on ? 'Izklopi' : 'Vklopi'; btn.className = 'btn ' + (on ? 'btn-danger' : 'btn-primary'); }
    if (sus) sus.style.display = susp ? '' : 'none';

    const wledEl = document.getElementById('party-wled-state');
    if (wledEl) wledEl.innerHTML = s.wled_on
      ? '<span class="badge badge-green">ON</span>'
      : '<span class="badge badge-gray">OFF</span>';

    const aslot = document.getElementById('party-active-slot');
    if (aslot) aslot.textContent = (s.active_slot !== 255 && s.active_slot != null)
      ? (_slots[s.active_slot] ? _slots[s.active_slot].name : 'Slot ' + s.active_slot)
      : '–';

    _applySlotVisual(s.active_slot, on);
  }

  function _applySlotVisual(activeIdx, partyOn) {
    for (let i = 0; i < 9; i++) {
      const btn = document.getElementById('pslot-' + i);
      if (!btn) continue;
      btn.classList.toggle('active', i === activeIdx && partyOn);
      btn.disabled = !partyOn || (_slots[i] && !_slots[i].enabled);
    }
  }

  function _applySlotLabels() {
    for (let i = 0; i < 9; i++) {
      const btn = document.getElementById('pslot-' + i);
      if (btn && _slots[i]) btn.textContent = _slots[i].name || ('Slot ' + i);
    }
    _applySlotVisual(_status.active_slot, _status.party_on);
  }

  // ── Poll ─────────────────────────────────────────────────
  async function _poll() {
    try {
      const s = await api.get('/api/party/status');
      _applyStatus(s);
      _wledIp = s.wled_ip || _wledIp;
      document.getElementById('party-ts').textContent = new Date().toLocaleTimeString();
    } catch(e) {
      const ts = document.getElementById('party-ts');
      if (ts) ts.textContent = 'napaka: ' + e.message;
    }
  }

  // ── Naloži podatke ob inicializaciji ─────────────────────
  async function _loadSlots() {
    try {
      const d = await api.get('/api/party/slots');
      _slots = d.slots || [];
      _applySlotLabels();
    } catch(e) { /* tiho */ }
  }

  async function _loadConfig() {
    try {
      const d = await api.get('/api/party/config');
      _wledIp = d.wled_ip || '';
    } catch(e) { /* tiho */ }
  }

  // ── Panel A — Akcije ─────────────────────────────────────
  window.partyToggle = async function() {
    const on = !_status.party_on;
    try {
      await api.post('/api/party/toggle', { on });
      _status.party_on = on;
      _applyStatus(_status);
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  window.partyActivateSlot = async function(idx) {
    if (!_status.party_on) return;
    try {
      await api.post('/api/party/activate', { slot_idx: idx });
      _status.active_slot = idx;
      _applySlotVisual(idx, true);
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  window.partySetColor = async function(rgb, dotIdx) {
    if (!_status.party_on || !_wledIp) return;
    document.querySelectorAll('.party-color-dot').forEach((d,i) => d.classList.toggle('sel', i === dotIdx));
    const r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    try {
      await fetch(`http://${_wledIp}/json/state`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ seg: [{ col: [[r, g, b]] }] })
      });
    } catch(e) { /* WLED nedosegljiv */ }
  };

  window.partySetBri = async function(val) {
    document.getElementById('party-bri-val').textContent = val;
    if (!_status.party_on || !_wledIp) return;
    try {
      await fetch(`http://${_wledIp}/json/state`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ bri: parseInt(val) })
      });
    } catch(e) { /* WLED nedosegljiv */ }
  };

  window.partySetSpd = async function(val) {
    document.getElementById('party-spd-val').textContent = val;
    if (!_status.party_on || !_wledIp) return;
    try {
      await fetch(`http://${_wledIp}/json/state`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ seg: [{ sx: parseInt(val) }] })
      });
    } catch(e) { /* WLED nedosegljiv */ }
  };

  // ── Panel B — Konfiguracija slotov ───────────────────────
  function _fxOptions(selectedId) {
    if (!_wledEffects.length) {
      return `<option value="${selectedId}">FX ID ${selectedId} — naloži efekte</option>`;
    }
    return _wledEffects.map(fx =>
      `<option value="${fx.id}" ${fx.id === selectedId ? 'selected' : ''}>${fx.id}: ${fx.name}</option>`
    ).join('');
  }

  function _rgbHex(n) {
    return '#' + ('000000' + (n >>> 0).toString(16)).slice(-6);
  }

  function _renderSlotsCfg() {
    const el = document.getElementById('party-slots-cfg');
    if (!el) return;
    if (!_slots.length) { el.innerHTML = '<div class="empty-state">Naloži slote…</div>'; return; }
    el.innerHTML = _slots.map((sl, i) => `
<div class="card" style="margin-bottom:10px">
  <div class="card-title">Slot ${i}</div>
  <div class="form-row">
    <div class="form-label">Ime <span class="text-dim text-tiny">(max 15)</span></div>
    <input class="form-input" type="text" id="scfg-name-${i}" maxlength="15" value="${_esc(sl.name)}">
  </div>
  <div class="form-row">
    <div class="form-label">Efekt</div>
    <select class="form-input" id="scfg-fx-${i}" style="width:auto;flex:1">${_fxOptions(sl.fx_id)}</select>
  </div>
  <div class="form-row">
    <div class="form-label">Barva <span class="text-dim text-tiny">(0x000000 = auto)</span></div>
    <div style="display:flex;align-items:center;gap:8px">
      <input type="color" id="scfg-clr-${i}" value="${_rgbHex(sl.color_rgb)}" style="width:44px;height:36px;padding:2px;cursor:pointer">
      <label style="display:flex;align-items:center;gap:4px;color:var(--text2);font-size:13px">
        <input type="checkbox" id="scfg-auto-${i}" ${sl.color_rgb === 0 ? 'checked' : ''}
               onchange="document.getElementById('scfg-clr-${i}').disabled=this.checked">
        Auto (WLED ohrani paleto)
      </label>
    </div>
  </div>
  <div class="form-row">
    <div class="form-label">Svetlost</div>
    <div style="display:flex;align-items:center;gap:8px;flex:1">
      <input type="range" min="0" max="255" id="scfg-bri-${i}" value="${sl.brightness}"
             style="flex:1" oninput="document.getElementById('scfg-bri-val-${i}').textContent=this.value">
      <span class="text-mono" id="scfg-bri-val-${i}">${sl.brightness}</span>
    </div>
  </div>
  <div class="form-row">
    <div class="form-label">Hitrost</div>
    <div style="display:flex;align-items:center;gap:8px;flex:1">
      <input type="range" min="0" max="255" id="scfg-spd-${i}" value="${sl.speed}"
             style="flex:1" oninput="document.getElementById('scfg-spd-val-${i}').textContent=this.value">
      <span class="text-mono" id="scfg-spd-val-${i}">${sl.speed}</span>
    </div>
  </div>
  <div class="form-row" style="border-bottom:none">
    <div class="form-label">Aktiven</div>
    <input type="checkbox" id="scfg-en-${i}" ${sl.enabled ? 'checked' : ''}>
  </div>
  <div style="display:flex;justify-content:flex-end;margin-top:12px">
    <button class="btn btn-primary" onclick="partySaveSlot(${i})">Shrani slot ${i}</button>
  </div>
  <div class="text-dim text-tiny mt8" id="scfg-st-${i}"></div>
</div>`).join('');

    // Nastavi disabled stanje za color picker (auto checked)
    _slots.forEach((sl, i) => {
      const cp = document.getElementById('scfg-clr-' + i);
      if (cp) cp.disabled = sl.color_rgb === 0;
    });
  }

  window.partyLoadEffects = async function() {
    const st = document.getElementById('party-fx-status');
    if (st) st.textContent = 'Nalagam efekte iz WLED…';
    if (!_wledIp) {
      await _loadConfig();
    }
    if (!_wledIp) { if (st) st.textContent = 'WLED IP ni nastavljen'; return; }
    try {
      const effects = await fetch(`http://${_wledIp}/json/effects`, { signal: AbortSignal.timeout(5000) })
        .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });
      _wledEffects = effects.map((name, idx) => ({ id: idx, name }));
      if (st) st.textContent = `Naloženih ${_wledEffects.length} efektov ✓`;
      _renderSlotsCfg();
    } catch(e) {
      if (st) st.textContent = `WLED nedosegljiv na ${_wledIp}: ` + e.message;
    }
  };

  window.partyOpenWled = function() {
    if (_wledIp) window.open('http://' + _wledIp, '_blank');
  };

  window.partySaveSlot = async function(i) {
    const auto = document.getElementById('scfg-auto-' + i)?.checked;
    let colorVal = 0;
    if (!auto) {
      const hex = document.getElementById('scfg-clr-' + i)?.value || '#ffffff';
      colorVal = parseInt(hex.replace('#', ''), 16);
    }
    const body = {
      idx:        i,
      name:       (document.getElementById('scfg-name-' + i)?.value || '').trim().slice(0,15),
      fx_id:      parseInt(document.getElementById('scfg-fx-' + i)?.value || '0'),
      color_rgb:  colorVal,
      brightness: parseInt(document.getElementById('scfg-bri-' + i)?.value || '180'),
      speed:      parseInt(document.getElementById('scfg-spd-' + i)?.value || '128'),
      enabled:    document.getElementById('scfg-en-' + i)?.checked ?? true,
    };
    const st = document.getElementById('scfg-st-' + i);
    try {
      await api.post('/api/party/slots', body);
      if (st) { st.textContent = 'Shranjeno ✓'; st.style.color = 'var(--green)'; }
      await _loadSlots();
    } catch(e) {
      if (st) { st.textContent = 'Napaka: ' + e.message; st.style.color = 'var(--red)'; }
    }
  };

  // ── Panel C — Urniki ─────────────────────────────────────
  async function _loadSchedules() {
    try {
      const d = await api.get('/api/party/schedules');
      _schedules = d.schedules || [];
    } catch(e) { /* tiho */ }
  }

  function _renderSchedules() {
    const el = document.getElementById('party-schedules-list');
    if (!el) return;
    if (!_schedules.length) { el.innerHTML = '<div class="empty-state">Ni urnikov</div>'; return; }

    const slotOpts = Array.from({length:9}, (_,i) =>
      `<option value="${i}">${i}: ${_slots[i] ? _esc(_slots[i].name) : 'Slot '+i}</option>`
    ).join('');

    el.innerHTML = _schedules.map((sc, i) => `
<div class="sched-card">
  <div style="display:flex;align-items:center;gap:8px;margin-bottom:12px">
    <input class="form-input" type="text" id="sch-name-${i}" maxlength="23"
           value="${_esc(sc.name)}" placeholder="Ime urnika" style="flex:1">
    <label style="display:flex;align-items:center;gap:4px;white-space:nowrap;color:var(--text2);font-size:13px">
      <input type="checkbox" id="sch-en-${i}" ${sc.enabled ? 'checked' : ''}> Aktiven
    </label>
  </div>
  <div class="kv-list">
    <div class="kv-row">
      <span class="kv-key">Slot</span>
      <select class="form-input" id="sch-slot-${i}" style="width:auto">${slotOpts.replace(`value="${sc.slot_idx}"`, `value="${sc.slot_idx}" selected`)}</select>
    </div>
    <div class="kv-row">
      <span class="kv-key">Od (MM-DD)</span>
      <div style="display:flex;gap:4px">
        <input class="form-input" type="number" id="sch-fm-${i}" min="1" max="12" value="${sc.from_month}" style="width:60px" placeholder="MM">
        <input class="form-input" type="number" id="sch-fd-${i}" min="1" max="31" value="${sc.from_day}"   style="width:60px" placeholder="DD">
      </div>
    </div>
    <div class="kv-row">
      <span class="kv-key">Do (MM-DD)</span>
      <div style="display:flex;gap:4px">
        <input class="form-input" type="number" id="sch-tm-${i}" min="1" max="12" value="${sc.to_month}" style="width:60px" placeholder="MM">
        <input class="form-input" type="number" id="sch-td-${i}" min="1" max="31" value="${sc.to_day}"   style="width:60px" placeholder="DD">
      </div>
    </div>
    <div class="kv-row">
      <span class="kv-key">Vklop</span>
      <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
        <label><input type="radio" name="sch-on-mode-${i}" value="time" ${!sc.use_lux_on?'checked':''}
                 onchange="_schedOnMode(${i})"> Ob uri</label>
        <label><input type="radio" name="sch-on-mode-${i}" value="lux"  ${sc.use_lux_on?'checked':''}
                 onchange="_schedOnMode(${i})"> Ob lux</label>
        <span id="sch-on-time-${i}" style="${sc.use_lux_on?'display:none':''}">
          <input class="form-input" type="number" id="sch-onh-${i}" min="0" max="23" value="${sc.time_on_h}" style="width:55px" placeholder="HH">
          <span style="color:var(--text2)">:</span>
          <input class="form-input" type="number" id="sch-onm-${i}" min="0" max="59" value="${sc.time_on_m}" style="width:55px" placeholder="MM">
        </span>
        <span id="sch-on-lux-${i}" style="${sc.use_lux_on?'':'display:none'}">
          <input class="form-input" type="number" id="sch-onlux-${i}" min="0" max="100000" value="${sc.lux_on}" style="width:90px" placeholder="lux"> lux
        </span>
      </div>
    </div>
    <div class="kv-row">
      <span class="kv-key">Izklop</span>
      <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
        <label><input type="radio" name="sch-off-mode-${i}" value="time" ${!sc.use_lux_off?'checked':''}
                 onchange="_schedOffMode(${i})"> Ob uri</label>
        <label><input type="radio" name="sch-off-mode-${i}" value="lux"  ${sc.use_lux_off?'checked':''}
                 onchange="_schedOffMode(${i})"> Ob lux</label>
        <span id="sch-off-time-${i}" style="${sc.use_lux_off?'display:none':''}">
          <input class="form-input" type="number" id="sch-offh-${i}" min="0" max="23" value="${sc.time_off_h}" style="width:55px" placeholder="HH">
          <span style="color:var(--text2)">:</span>
          <input class="form-input" type="number" id="sch-offm-${i}" min="0" max="59" value="${sc.time_off_m}" style="width:55px" placeholder="MM">
        </span>
        <span id="sch-off-lux-${i}" style="${sc.use_lux_off?'':'display:none'}">
          <input class="form-input" type="number" id="sch-offlux-${i}" min="0" max="100000" value="${sc.lux_off}" style="width:90px" placeholder="lux"> lux
        </span>
      </div>
    </div>
  </div>
  <div style="display:flex;gap:8px;justify-content:flex-end;margin-top:12px;flex-wrap:wrap">
    <button class="btn btn-danger" onclick="partyDelSched(${i})">Onemogoči / Pobriši</button>
    <button class="btn btn-primary" onclick="partySaveSched(${i})">Shrani</button>
  </div>
  <div class="text-dim text-tiny mt8" id="sch-st-${i}"></div>
</div>`).join('');
  }

  window._schedOnMode = function(i) {
    const lux  = document.querySelector(`input[name="sch-on-mode-${i}"]:checked`)?.value === 'lux';
    document.getElementById('sch-on-time-' + i).style.display = lux ? 'none' : '';
    document.getElementById('sch-on-lux-'  + i).style.display = lux ? '' : 'none';
  };

  window._schedOffMode = function(i) {
    const lux = document.querySelector(`input[name="sch-off-mode-${i}"]:checked`)?.value === 'lux';
    document.getElementById('sch-off-time-' + i).style.display = lux ? 'none' : '';
    document.getElementById('sch-off-lux-'  + i).style.display = lux ? '' : 'none';
  };

  window.partyNewSched = async function() {
    const emptyIdx = _schedules.findIndex(s => !s.enabled && !s.name);
    if (emptyIdx >= 0) {
      // fokusiraj obstoječ prazen
      document.getElementById('sch-name-' + emptyIdx)?.focus();
      return;
    }
    const freeIdx = _schedules.length < 10 ? _schedules.length : -1;
    if (freeIdx < 0) { alert('Največ 10 urnikov'); return; }
    try {
      await api.post('/api/party/schedules', {
        idx: freeIdx, name: 'Nov urnik', slot_idx: 0, enabled: false,
        from_month:1, from_day:1, to_month:12, to_day:31,
        use_lux_on: false, time_on_h:20, time_on_m:0,
        use_lux_off: false, time_off_h:23, time_off_m:0
      });
      await _loadSchedules();
      _renderSchedules();
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  window.partySaveSched = async function(i) {
    const useLuxOn  = document.querySelector(`input[name="sch-on-mode-${i}"]:checked`)?.value === 'lux';
    const useLuxOff = document.querySelector(`input[name="sch-off-mode-${i}"]:checked`)?.value === 'lux';
    const slotSel = parseInt(document.getElementById('sch-slot-' + i)?.value || '0');
    if (slotSel < 0 || slotSel > 8) { alert('slot_idx mora biti 0-8'); return; }
    const body = {
      idx:         i,
      name:        (document.getElementById('sch-name-' + i)?.value || '').trim(),
      slot_idx:    slotSel,
      enabled:     document.getElementById('sch-en-' + i)?.checked ?? false,
      from_month:  parseInt(document.getElementById('sch-fm-' + i)?.value  || '1'),
      from_day:    parseInt(document.getElementById('sch-fd-' + i)?.value  || '1'),
      to_month:    parseInt(document.getElementById('sch-tm-' + i)?.value  || '12'),
      to_day:      parseInt(document.getElementById('sch-td-' + i)?.value  || '31'),
      use_lux_on:  useLuxOn,
      lux_on:      parseInt(document.getElementById('sch-onlux-' + i)?.value || '0'),
      time_on_h:   parseInt(document.getElementById('sch-onh-'  + i)?.value || '20'),
      time_on_m:   parseInt(document.getElementById('sch-onm-'  + i)?.value || '0'),
      use_lux_off: useLuxOff,
      lux_off:     parseInt(document.getElementById('sch-offlux-' + i)?.value || '0'),
      time_off_h:  parseInt(document.getElementById('sch-offh-' + i)?.value || '23'),
      time_off_m:  parseInt(document.getElementById('sch-offm-' + i)?.value || '0'),
    };
    const st = document.getElementById('sch-st-' + i);
    try {
      await api.post('/api/party/schedules', body);
      if (st) { st.textContent = 'Shranjeno ✓'; st.style.color = 'var(--green)'; }
      await _loadSchedules();
    } catch(e) {
      if (st) { st.textContent = 'Napaka: ' + e.message; st.style.color = 'var(--red)'; }
    }
  };

  window.partyDelSched = async function(i) {
    showConfirm('Onemogoči urnik', 'Urnik bo onemogočen.', async () => {
      const st = document.getElementById('sch-st-' + i);
      try {
        await api.del('/api/party/schedules?idx=' + i);
        if (st) { st.textContent = 'Onemogočen ✓'; st.style.color = 'var(--green)'; }
        await _loadSchedules();
        _renderSchedules();
      } catch(e) {
        if (st) { st.textContent = 'Napaka: ' + e.message; st.style.color = 'var(--red)'; }
      }
    }, 'Onemogoči');
  };

  // ── Panel D — Nastavitve ─────────────────────────────────
  function _renderCfg() {
    const ipEl = document.getElementById('party-cfg-ip');
    const rdEl = document.getElementById('party-cfg-resume');
    const rvEl = document.getElementById('party-cfg-resume-val');
    if (ipEl) ipEl.value = _wledIp;
    if (rdEl) { rdEl.value = _status.resume_delay_s || 30; }
    if (rvEl) rvEl.textContent = _status.resume_delay_s || 30;
  }

  window.partySaveCfg = async function() {
    const ip = (document.getElementById('party-cfg-ip')?.value || '').trim();
    const rd = parseInt(document.getElementById('party-cfg-resume')?.value || '30');
    const st = document.getElementById('party-cfg-st');
    if (!ip) { if (st) { st.textContent = 'IP naslov je obvezen'; st.style.color='var(--red)'; } return; }
    if (rd < 5 || rd > 300) { if (st) { st.textContent = 'Resume delay: 5–300 s'; st.style.color='var(--red)'; } return; }
    try {
      await api.post('/api/party/config', { wled_ip: ip, resume_delay_s: rd });
      _wledIp = ip;
      if (st) { st.textContent = 'Shranjeno ✓'; st.style.color = 'var(--green)'; }
    } catch(e) {
      if (st) { st.textContent = 'Napaka: ' + e.message; st.style.color = 'var(--red)'; }
    }
  };

  // ── Pomožne ──────────────────────────────────────────────
  function _esc(s) {
    return (s || '').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');
  }

  // ── Init ─────────────────────────────────────────────────
  window.page_party = async function() {
    if (!document.getElementById('party-state-lbl')) _render();
    await Promise.all([_loadConfig(), _loadSlots()]);
    registerPoller('party', _poll, 3000);
  };
})();
