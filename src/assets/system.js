// ============================================================
// system.js — Stran: Sistem  (v3)
// Diagnostika (RAM/SDRAM, WiFi, SD, Firmware), OTA, reset/restart
//
// v3: - SRAM: esp_get_free_heap_size() → heap_caps_get_free_size(INTERNAL)
//            (prejšnja vrednost je vključevala PSRAM → previsoka)
//     - PSRAM preimenovan v SDRAM (OPI PSRAM je SDRAM tehnologija)
//     - SD kartica sekcija dodana
//     - Auto-refresh stikalo (3s, default OFF)
// ============================================================

(function () {
  const DIV = 'page-system';
  let _autoOn = false;

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Sistem</h1>
  <div style="display:flex;align-items:center;gap:10px">
    <span class="subtitle" id="sys-ts">–</span>
    <span class="text-dim text-tiny">Auto</span>
    <label class="form-toggle">
      <input type="checkbox" id="sys-auto-chk" onchange="_sysAutoToggle(this.checked)">
      <span class="form-toggle-track"></span>
    </label>
    <button class="btn" id="sys-refresh-btn" onclick="sysRefresh()">↻ Osveži</button>
  </div>
</div>

<!-- RAM & SDRAM ─────────────────────────────── -->
<div class="section-label">Pomnilnik</div>
<div class="grid-4">
  <div class="card">
    <div class="card-title">Free SRAM</div>
    <div class="metric loading" id="sys-sram">–</div>
    <div class="text-dim text-tiny mt8">interni heap</div>
  </div>
  <div class="card">
    <div class="card-title">Min-ever SRAM</div>
    <div class="metric loading" id="sys-sram-min">–</div>
    <div class="text-dim text-tiny mt8">od zagona</div>
  </div>
  <div class="card">
    <div class="card-title">Free SDRAM</div>
    <div class="metric loading" id="sys-psram">–</div>
    <div class="text-dim text-tiny mt8">OPI PSRAM</div>
  </div>
  <div class="card">
    <div class="card-title">Min-ever SDRAM</div>
    <div class="metric loading" id="sys-psram-min">–</div>
    <div class="text-dim text-tiny mt8">od zagona</div>
  </div>
</div>

<!-- WiFi / Čas ──────────────────────────────── -->
<div class="section-label mt20">Čas &amp; omrežje</div>
<div class="grid-2">
  <div class="card">
    <div class="card-title">Čas &amp; uptime</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">Uptime</span>   <span class="kv-val text-mono" id="sys-uptime">–</span></div>
      <div class="kv-row"><span class="kv-key">NTP čas</span>  <span class="kv-val text-mono" id="sys-ntp">–</span></div>
      <div class="kv-row"><span class="kv-key">NTP sync</span> <span class="kv-val" id="sys-ntp-ok">–</span></div>
    </div>
  </div>
  <div class="card">
    <div class="card-title">WiFi</div>
    <div class="kv-list">
      <div class="kv-row"><span class="kv-key">IP</span>       <span class="kv-val text-mono" id="sys-ip">–</span></div>
      <div class="kv-row"><span class="kv-key">SSID</span>     <span class="kv-val" id="sys-ssid">–</span></div>
      <div class="kv-row"><span class="kv-key">RSSI</span>     <span class="kv-val" id="sys-rssi">–</span></div>
      <div class="kv-row"><span class="kv-key">Reconn.</span>  <span class="kv-val text-mono" id="sys-reconn">–</span></div>
    </div>
  </div>
</div>

<!-- SD kartica ──────────────────────────────── -->
<div class="section-label mt20">SD kartica</div>
<div class="card">
  <div class="kv-list">
    <div class="kv-row"><span class="kv-key">Status</span>  <span class="kv-val" id="sys-sd-status">–</span></div>
    <div class="kv-row"><span class="kv-key">Skupaj</span>  <span class="kv-val text-mono" id="sys-sd-total">–</span></div>
    <div class="kv-row"><span class="kv-key">Prosto</span>  <span class="kv-val text-mono" id="sys-sd-free">–</span></div>
    <div class="kv-row"><span class="kv-key">Log flushi</span> <span class="kv-val text-mono" id="sys-log-flush">–</span></div>
  </div>
  <div class="progress-bar mt8">
    <div class="progress-fill" id="sys-sd-bar" style="width:0%"></div>
  </div>
</div>

<!-- Firmware ─────────────────────────────────── -->
<div class="section-label mt20">Firmware</div>
<div class="card">
  <div class="kv-list">
    <div class="kv-row"><span class="kv-key">Verzija</span>     <span class="kv-val text-mono" id="sys-fw-ver">–</span></div>
    <div class="kv-row"><span class="kv-key">Build datum</span> <span class="kv-val text-mono" id="sys-fw-date">–</span></div>
    <div class="kv-row"><span class="kv-key">Build čas</span>   <span class="kv-val text-mono" id="sys-fw-time">–</span></div>
    <div class="kv-row"><span class="kv-key">IDF verzija</span> <span class="kv-val text-mono" id="sys-fw-idf">–</span></div>
    <div class="kv-row"><span class="kv-key">Config mgr</span>  <span class="kv-val" id="sys-cfg-ok">–</span></div>
  </div>
</div>

<!-- OTA ─────────────────────────────────────── -->
<div class="section-label mt20">OTA posodobitev firmware</div>
<div class="card">
  <div class="drop-zone" id="ota-drop"
       onclick="document.getElementById('ota-file').click()"
       ondragover="event.preventDefault();this.classList.add('drag-over')"
       ondragleave="this.classList.remove('drag-over')"
       ondrop="otaDrop(event)">
    <span class="dz-icon">⬆</span>
    <strong style="font-family:var(--font);font-size:12px;color:var(--text)">Klikni ali povleci .bin datoteko</strong>
    <p>Firmware za ESP32 · max 3 MB</p>
  </div>
  <input type="file" id="ota-file" accept=".bin" style="display:none"
         onchange="otaFileSelected(this)">

  <div id="ota-progress-wrap" style="display:none;margin-top:12px">
    <div class="flex-row flex-between mb8">
      <span class="text-tiny text-mono" id="ota-filename">–</span>
      <span class="text-tiny text-mono" id="ota-pct">0%</span>
    </div>
    <div class="progress-bar">
      <div class="progress-fill" id="ota-bar" style="width:0%"></div>
    </div>
    <div class="text-tiny text-dim mt8" id="ota-msg">Nalagam…</div>
  </div>
</div>

<!-- AKCIJE ──────────────────────────────────── -->
<div class="section-label mt20">Upravljanje</div>
<div class="card">
  <div class="flex-row">
    <div>
      <div class="form-label">Ponastavi konfiguracijo</div>
      <div class="form-hint">Vse nastavitve se vrnejo na privzete vrednosti (NVS reset)</div>
    </div>
    <button class="btn btn-danger" onclick="sysResetCfg()">Reset na privzeto</button>
  </div>
  <div class="form-row" style="border-top:1px solid var(--border);margin-top:10px;padding-top:10px">
    <div>
      <div class="form-label">Restart ESP32</div>
      <div class="form-hint">Naprava se bo zagnala znova (~5 s)</div>
    </div>
    <button class="btn btn-danger" onclick="sysRestart()">Restart</button>
  </div>
</div>

<!-- Web UI statistika ───────────────────────── -->
<div class="section-label mt20">Web UI</div>
<div class="card">
  <div class="kv-list">
    <div class="kv-row"><span class="kv-key">Skupaj zahtevkov</span> <span class="kv-val text-mono" id="sys-req-total">–</span></div>
    <div class="kv-row"><span class="kv-key">API zahtevkov</span>    <span class="kv-val text-mono" id="sys-req-api">–</span></div>
    <div class="kv-row"><span class="kv-key">File zahtevkov</span>   <span class="kv-val text-mono" id="sys-req-files">–</span></div>
    <div class="kv-row"><span class="kv-key">Napake</span>           <span class="kv-val text-mono" id="sys-req-err">–</span></div>
    <div class="kv-row"><span class="kv-key">OTA poskusi</span>      <span class="kv-val text-mono" id="sys-ota-att">–</span></div>
    <div class="kv-row"><span class="kv-key">LittleFS</span>         <span class="kv-val" id="sys-lfs">–</span></div>
    <div class="kv-row"><span class="kv-key">Assets</span>           <span class="kv-val" id="sys-assets">–</span></div>
  </div>
</div>
`;
  }

  // ── Format helpers (lokalni) ──────────────────────────────
  function _kb(bytes) {
    if (bytes === undefined || bytes === null) return '–';
    return (bytes / 1024).toFixed(1) + ' KB';
  }

  function _uptimeLong(ms) {
    if (!ms) return '–';
    const s  = Math.floor(ms / 1000);
    const d  = Math.floor(s / 86400);
    const h  = Math.floor((s % 86400) / 3600);
    const m  = Math.floor((s % 3600) / 60);
    const ss = s % 60;
    if (d > 0) return d + 'd ' + String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(ss).padStart(2,'0');
    return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(ss).padStart(2,'0');
  }

  // ── Posodobi UI ───────────────────────────────────────────
  function _update(d) {
    const set = (id, v) => { const e = document.getElementById(id); if (e) { e.textContent = v; e.classList.remove('loading'); } };
    const cls = (id, c) => { const e = document.getElementById(id); if (e) e.className = 'metric ' + c; };

    set('sys-ts', new Date().toLocaleTimeString('sl-SI'));

    // ── SRAM / SDRAM ──
    const freeSram   = d.free_sram;
    const minSram    = d.min_free_sram;
    const freePsram  = d.free_psram;
    const minPsram   = d.min_free_psram;

    set('sys-sram',      freeSram  !== undefined ? _kb(freeSram)  : '–');
    set('sys-sram-min',  minSram   !== undefined ? _kb(minSram)   : '–');
    set('sys-psram',     freePsram !== undefined ? _kb(freePsram) : '–');
    set('sys-psram-min', minPsram  !== undefined ? _kb(minPsram)  : '–');

    // Barvanje SRAM — pod 10 KB kritično, pod 20 KB opozorilo
    if (freeSram !== undefined) {
      const kb = freeSram / 1024;
      cls('sys-sram', kb < 10 ? 'err' : kb < 20 ? 'warn' : 'ok');
    }
    if (minSram !== undefined) {
      const kb = minSram / 1024;
      cls('sys-sram-min', kb < 10 ? 'err' : kb < 20 ? 'warn' : '');
    }

    // Config mgr status
    const cfgOk      = d.config_ok;
    const cfgReplaced = d.config_replaced;
    let cfgStr = '–';
    if (cfgOk !== undefined) {
      cfgStr = cfgOk
        ? (cfgReplaced > 0 ? `OK (${cfgReplaced} default-ov ob zagonu)` : 'OK')
        : 'NAPAKA';
    }
    set('sys-cfg-ok', cfgStr);
    const cfgEl = document.getElementById('sys-cfg-ok');
    if (cfgEl) cfgEl.style.color = cfgOk === false ? 'var(--red)' : cfgReplaced > 0 ? 'var(--amber)' : '';

    // ── WiFi / uptime ──
    const w = d.wifi || {};
    set('sys-uptime',  _uptimeLong(w.uptime_ms));
    set('sys-ntp',     w.ntp_time || (w.ntp_ok ? 'OK' : 'NI sinhronizirano'));
    set('sys-ntp-ok',  w.ntp_ok !== undefined ? (w.ntp_ok ? '✓ sync' : '✗ ni sync') : '–');
    const ntpEl = document.getElementById('sys-ntp-ok');
    if (ntpEl) ntpEl.style.color = w.ntp_ok ? 'var(--green)' : 'var(--red)';
    set('sys-ip',     w.ip   || '–');
    set('sys-ssid',   w.ssid || '–');
    set('sys-rssi',   w.rssi     !== undefined ? w.rssi + ' dBm' : '–');
    set('sys-reconn', w.reconnects !== undefined ? w.reconnects  : '–');

    // ── SD kartica ──
    const sd = d.sd || {};
    const sdReady = sd.ready;
    set('sys-sd-status', sdReady !== undefined ? (sdReady ? 'READY' : (sd.status || 'napaka')) : '–');
    set('sys-sd-total',  sd.total_mb !== undefined ? sd.total_mb + ' MB' : '–');
    set('sys-sd-free',   sd.free_mb  !== undefined ? sd.free_mb  + ' MB' : '–');
    const lg = d.logger || {};
    set('sys-log-flush', lg.sd_flushes !== undefined ? lg.sd_flushes : '–');
    const sdBar = document.getElementById('sys-sd-bar');
    if (sdBar && sd.total_mb > 0) {
      const pct = Math.round(100 - (sd.free_mb / sd.total_mb) * 100);
      sdBar.style.width = pct + '%';
      sdBar.className = 'progress-fill' + (pct > 90 ? ' err' : pct > 75 ? ' warn' : '');
    }

    // ── Firmware ──
    const fw = d.firmware || {};
    set('sys-fw-ver',  fw.version    || '–');
    set('sys-fw-date', fw.build_date || '–');
    set('sys-fw-time', fw.build_time || '–');
    set('sys-fw-idf',  fw.idf_ver    || '–');

    // ── Web UI statistika ──
    const wu = d.webui || {};
    set('sys-req-total', wu.req_total   !== undefined ? wu.req_total   : '–');
    set('sys-req-api',   wu.req_api     !== undefined ? wu.req_api     : '–');
    set('sys-req-files', wu.req_files   !== undefined ? wu.req_files   : '–');
    set('sys-req-err',   wu.req_errors  !== undefined ? wu.req_errors  : '–');
    set('sys-ota-att',   wu.ota_attempts !== undefined
                           ? wu.ota_attempts + (wu.ota_success ? ' (' + wu.ota_success + ' OK)' : '')
                           : '–');
    const lfsEl = document.getElementById('sys-lfs');
    if (lfsEl) {
      lfsEl.textContent = wu.littlefs_ok !== undefined ? (wu.littlefs_ok ? 'OK' : 'NAPAKA') : '–';
      lfsEl.style.color = wu.littlefs_ok === false ? 'var(--red)' : '';
    }
    const astEl = document.getElementById('sys-assets');
    if (astEl) {
      astEl.textContent = wu.assets_ok !== undefined ? (wu.assets_ok ? 'OK' : 'manjka index.html') : '–';
      astEl.style.color = wu.assets_ok === false ? 'var(--amber)' : '';
    }
  }

  async function _poll() {
    const ts = document.getElementById('sys-ts');
    try {
      const d = await api.get('/api/status/system');
      _update(d);
    } catch(e) {
      if (ts) ts.textContent = '⚠ ' + e.message;
    }
  }

  // ── Auto-refresh stikalo ──────────────────────────────────
  window._sysAutoToggle = function(on) {
    _autoOn = on;
    const btn = document.getElementById('sys-refresh-btn');
    if (btn) btn.style.display = on ? 'none' : '';
    if (on) { _poll(); registerPoller('system', _poll, 3000); }
    else    { clearPoller('system'); }
  };

  // ── OTA ──────────────────────────────────────────────────
  window.otaDrop = function(ev) {
    ev.preventDefault();
    document.getElementById('ota-drop').classList.remove('drag-over');
    const f = ev.dataTransfer.files[0];
    if (f) _otaUpload(f);
  };
  window.otaFileSelected = function(inp) {
    if (inp.files[0]) _otaUpload(inp.files[0]);
    inp.value = '';
  };

  async function _otaUpload(file) {
    if (!file.name.endsWith('.bin')) { alert('Izberi .bin datoteko!'); return; }
    if (file.size > 3 * 1024 * 1024) { alert('Datoteka je prevelika (max 3 MB)!'); return; }

    showConfirm('OTA posodobitev',
      `Naloži "${file.name}" (${fmt.bytes(file.size)}) na napravo?`,
      async () => {
        const wrap = document.getElementById('ota-progress-wrap');
        const bar  = document.getElementById('ota-bar');
        const msg  = document.getElementById('ota-msg');
        const pct  = document.getElementById('ota-pct');
        const fn   = document.getElementById('ota-filename');

        if (wrap) wrap.style.display = 'block';
        if (fn)   fn.textContent = file.name;

        const fd = new FormData();
        fd.append('firmware', file, file.name);

        try {
          await new Promise((resolve, reject) => {
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/api/ota');
            xhr.upload.onprogress = (e) => {
              if (e.lengthComputable) {
                const p = Math.round(e.loaded / e.total * 100);
                if (bar) bar.style.width = p + '%';
                if (pct) pct.textContent = p + '%';
                if (msg) msg.textContent = p < 100 ? 'Nalagam…' : 'Čakam na potrditev…';
              }
            };
            xhr.onload  = () => xhr.status < 300 ? resolve() : reject(new Error(xhr.status + ' ' + xhr.responseText));
            xhr.onerror = () => reject(new Error('Napaka omrežja'));
            xhr.send(fd);
          });
          if (msg) msg.textContent = 'Upload OK — naprava se reštarta…';
          if (bar) bar.className = 'progress-fill ok';
        } catch(e) {
          if (msg) msg.textContent = 'Napaka: ' + e.message;
          if (bar) bar.className = 'progress-fill err';
        }
      }, 'Naloži');
  }

  // ── Reset / Restart ───────────────────────────────────────
  window.sysResetCfg = function() {
    showConfirm('Ponastavi konfiguracijo',
      'Vse nastavitve bodo vrnjene na privzete vrednosti. Naprava se NE bo zagnala znova.',
      async () => {
        try {
          const r = await api.post('/api/config/reset', {});
          alert(r.msg || 'Ponastavitev OK');
        } catch(e) { alert('Napaka: ' + e.message); }
      }, 'Ponastavi');
  };

  window.sysRestart = function() {
    showConfirm('Restart ESP32',
      'Naprava se bo zagnala znova. Web UI bo nedostopen ~5 s.',
      async () => {
        try { await api.post('/api/restart', {}); } catch { /* pričakovano */ }
        setTimeout(() => window.location.reload(), 6000);
      }, 'Restart');
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_system = function () {
    if (!document.getElementById('sys-ts')) _render();
    // Obnovi checkbox stanje ob povratku
    const chk = document.getElementById('sys-auto-chk');
    if (chk) chk.checked = _autoOn;
    const btn = document.getElementById('sys-refresh-btn');
    if (btn) btn.style.display = _autoOn ? 'none' : '';
    if (_autoOn) { _poll(); registerPoller('system', _poll, 3000); }
    else         { _poll(); }
  };

  window.sysRefresh = function() { _poll(); };
})();
