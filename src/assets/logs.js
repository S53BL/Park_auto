// logs.js — Logi & SD (v4.3)
// !! OB VSAKI SPREMEMBI DVIGNI VERZIJO: v4.x → v4.(x+1) tukaj IN v page-header spodaj !!
// Tab 1: RAM logi  — polling /api/logs (text/plain), inkrementalni append, X-Log-Total cursor
// Tab 2: SD logi   — browse /logs/ mape, preview, download, delete
// Tab 3: SD vse    — browse celotne SD kartice
//
// Arhitektura (ADR-014):
//   ESP32 streama text/plain + X-Log-Total header (brez JSON enkodiranja).
//   Brskalnik naredi vse: split, parsiranje, filtriranje, rendering, inkrementalni append.
//   Cursor: delta = newTotal - prevTotal → zadnjih delta vrstic so nove → append samo teh.

(function () {
  const DIV         = 'page-logs';
  const DOM_LIMIT   = 500;
  const POLL_MS     = 5000;
  const LINES_FETCH = 200;

  let _allLines  = [];   // vse vrstice iz zadnjega odgovora
  let _prevTotal = -1;   // X-Log-Total iz zadnjega polla (-1 = prvi poll)
  let _autoScroll = true;
  let _paused     = false;
  let _filterLvl  = 'ALL';
  let _filterMod  = '';
  let _sdPath     = '/';

  // ── Parser log vrstice ───────────────────────────────────
  // Format: [HH:MM:SS][TAG:L] msg   ali   [M000123456][TAG:L] msg
  // L = E/W/I/D
  function _parse(raw) {
    const ts  = raw.match(/^\[(\d{2}:\d{2}:\d{2}|M\d+)\]/);
    const tag = raw.match(/\[([A-Z0-9_]+):([EWID])\]/);
    const lvl = tag ? tag[2] : 'I';
    const msgStart = tag ? raw.indexOf(tag[0]) + tag[0].length : 0;
    return {
      ts:  ts  ? ts[1]  : '',
      mod: tag ? tag[1] : '',
      lvl,
      msg: raw.slice(msgStart).trim(),
      cls: ({E:'ll-error', W:'ll-warn', I:'ll-info', D:'ll-debug'})[lvl] || 'll-info'
    };
  }

  function _esc(t) {
    return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  }

  // Vrne HTML string za eno log vrstico.
  // <div> je block element — vrstice se ločijo brez white-space ali br, brez CSS odvisnosti.
  // Barve so inline — neodvisne od style.css.
  const _CLR = {E:'#ef4444', W:'#f59e0b', I:'#c8d0e0', D:'#4a5570'};
  function _lineHtml(raw) {
    const p    = _parse(raw);
    const col  = _CLR[p.lvl] || '#c8d0e0';
    return '<div style="color:' + col + ';padding:1px 0;word-break:break-all">' +
      (p.ts  ? '<span style="color:#4a5570">[' + _esc(p.ts) + ']</span> ' : '') +
      (p.mod ? '<span style="color:#06b6d4;font-weight:600">[' + _esc(p.mod) + ':' + p.lvl + ']</span> ' : '') +
      _esc(p.msg) + '</div>';
  }

  // ── Filter ───────────────────────────────────────────────
  const _LVL_OK = {
    ALL:   ['E','W','I','D'],
    ERROR: ['E'],
    WARN:  ['E','W'],
    INFO:  ['E','W','I'],
    DEBUG: ['E','W','I','D']
  };

  function _passes(raw) {
    if (_filterLvl !== 'ALL') {
      const ok = _LVL_OK[_filterLvl] || [];
      if (!ok.includes(_parse(raw).lvl)) return false;
    }
    if (_filterMod && _parse(raw).mod !== _filterMod) return false;
    return true;
  }

  // ── Render ───────────────────────────────────────────────
  function _out() { return document.getElementById('log-output'); }

  function _updateCount(shown) {
    const el = document.getElementById('log-count');
    if (el) el.textContent = shown + ' vrstic';
  }

  function _fullRender() {
    const out = _out();
    if (!out) return;
    const visible = _allLines.filter(_passes).slice(-DOM_LIMIT);
    if (visible.length === 0) {
      out.innerHTML = '<div class="ll-empty">Ni logov' +
        (_filterLvl !== 'ALL' || _filterMod ? ' (aktiven filter)' : '') + '</div>';
      _updateCount(0);
    } else {
      out.innerHTML = visible.map(_lineHtml).join('');
      _updateCount(visible.length);
    }
    if (_autoScroll) out.scrollTop = out.scrollHeight;
  }

  function _appendLines(fresh) {
    const out = _out();
    if (!out) return;
    const filtered = fresh.filter(_passes);
    if (filtered.length === 0) return;
    const empty = out.querySelector('.ll-empty');
    if (empty) out.removeChild(empty);
    out.insertAdjacentHTML('beforeend', filtered.map(_lineHtml).join(''));
    while (out.children.length > DOM_LIMIT) out.removeChild(out.firstChild);
    _updateCount(out.children.length);
    if (_autoScroll && !_paused) out.scrollTop = out.scrollHeight;
  }

  // ── Poll RAM logov ────────────────────────────────────────
  async function _pollLogs() {
    if (_paused) return;
    try {
      const r = await fetch('/api/logs?lines=' + LINES_FETCH,
                            { signal: AbortSignal.timeout(5000) });
      if (!r.ok) throw new Error(r.status + ' ' + r.statusText);

      const newTotal = parseInt(r.headers.get('X-Log-Total') || '-1', 10);
      const newLines = (await r.text()).split('\n').filter(l => l.trim());

      const tsEl = document.getElementById('log-ts');
      if (tsEl) tsEl.textContent = new Date().toLocaleTimeString('sl-SI');

      if (_prevTotal < 0 || newTotal < _prevTotal) {
        // Prva naložitev ali ESP32 restart → full render
        _allLines  = newLines;
        _prevTotal = newTotal;
        _fullRender();
        return;
      }

      const delta = newTotal - _prevTotal;
      _allLines   = newLines;
      _prevTotal  = newTotal;

      if (delta <= 0) return;  // nič novega

      // Cursor: zadnjih min(delta, len) vrstic je novih → append samo teh
      _appendLines(newLines.slice(-Math.min(delta, newLines.length)));

    } catch(e) {
      const tsEl = document.getElementById('log-ts');
      if (tsEl) tsEl.textContent = '⚠ ' + e.message;
    }
  }

  // ── Download RAM buffer ───────────────────────────────────
  function _downloadRam() {
    const blob = new Blob([_allLines.join('\n')], { type: 'text/plain' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = 'parking_ram_' +
      new Date().toISOString().slice(0,19).replace(/[T:]/g,'-') + '.log';
    a.click();
    URL.revokeObjectURL(url);
  }

  // ── SD logi (/logs/ mapa) ─────────────────────────────────
  async function _loadSdLogs() {
    const tbody = document.getElementById('sdlog-body');
    const info  = document.getElementById('sdlog-info');
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>';
    try {
      const d = await api.get('/files?dir=/logs');
      const files = (d.files || []).filter(f => f.name && f.name.endsWith('.log'));
      if (info) info.textContent = files.length + ' log datotek · ' +
        d.disk_free_mb + ' MB prosto / ' + d.disk_total_mb + ' MB';
      if (files.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty-state">Ni log datotek v /logs/</td></tr>';
        return;
      }
      files.sort((a,b) => (b.date||'').localeCompare(a.date||''));
      tbody.innerHTML = files.map(f => `
        <tr>
          <td class="mono" style="word-break:break-all;font-size:11px">${f.name}</td>
          <td class="right dim" style="white-space:nowrap">${fmt.bytes(f.size_bytes)}</td>
          <td class="right dim" style="white-space:nowrap;font-size:11px">${f.date||'–'}</td>
          <td class="right" style="white-space:nowrap">
            <button class="btn btn-sm" onclick="sdLogPreview('${f.path.replace(/'/g,"\\'")}')">👁</button>
            <a href="/files?path=${encodeURIComponent(f.path)}"
               class="btn btn-sm" style="text-decoration:none;margin-left:3px" download>↓</a>
            <button class="btn btn-sm btn-danger" style="margin-left:3px"
                    onclick="sdLogDelete('${f.path.replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="4" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
    }
  }

  // SD preview — isti renderer kot RAM logi
  window.sdLogPreview = async function(path) {
    const panel  = document.getElementById('sdlog-preview');
    const title  = document.getElementById('sdlog-preview-title');
    const output = document.getElementById('sdlog-preview-output');
    if (!panel || !output) return;
    panel.style.display = 'block';
    if (title) title.textContent = path.split('/').pop();
    output.innerHTML = '<div class="ll-empty">Nalagam…</div>';
    try {
      const r = await fetch('/files?path=' + encodeURIComponent(path),
                            { signal: AbortSignal.timeout(10000) });
      if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
      const lines = (await r.text()).split('\n').filter(l => l.trim()).slice(-200);
      if (lines.length === 0) {
        output.innerHTML = '<div class="ll-empty">Prazna datoteka</div>';
      } else {
        output.innerHTML = lines.map(_lineHtml).join('');
        output.scrollTop = output.scrollHeight;
      }
    } catch(e) {
      output.innerHTML = '<div class="ll-empty">Napaka: ' + _esc(e.message) + '</div>';
    }
  };

  window.sdLogPreviewClose = function() {
    const p = document.getElementById('sdlog-preview');
    if (p) p.style.display = 'none';
  };

  window.sdLogDelete = function(path) {
    showConfirm('Izbriši log', path.split('/').pop(), async () => {
      try { await api.del('/files?path=' + encodeURIComponent(path)); _loadSdLogs(); }
      catch(e) { alert('Napaka: ' + e.message); }
    }, 'Izbriši');
  };

  window.sdLogDeleteAll = function() {
    showConfirm('Izbriši VSE log datoteke',
      'Vse datoteke v /logs/ bodo trajno izbrisane!',
      async () => {
        try {
          const d = await api.get('/files?dir=/logs');
          for (const f of (d.files||[]).filter(f => f.name && f.name.endsWith('.log')))
            await api.del('/files?path=' + encodeURIComponent(f.path));
          _loadSdLogs();
        } catch(e) { alert('Napaka: ' + e.message); }
      }, 'Izbriši vse');
  };

  // ── SD browse ────────────────────────────────────────────
  async function _loadSdAll(dir) {
    _sdPath = dir || '/';
    const tbody  = document.getElementById('sd-body');
    const info   = document.getElementById('sd-diskinfo');
    const curDir = document.getElementById('sd-curdir');
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>';
    if (curDir) curDir.textContent = _sdPath;
    try {
      const d     = await api.get('/files?dir=' + encodeURIComponent(_sdPath));
      const files = d.files || [];
      if (info) info.textContent = d.disk_free_mb + ' MB prosto / ' + d.disk_total_mb + ' MB skupaj';
      if (files.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty-state">Brez datotek</td></tr>';
        return;
      }
      files.sort((a,b) => {
        if (a.is_dir !== b.is_dir) return (b.is_dir?1:0)-(a.is_dir?1:0);
        return (a.name||'').localeCompare(b.name||'');
      });
      tbody.innerHTML = files.map(f => f.is_dir ? `
        <tr>
          <td class="mono" style="cursor:pointer;color:var(--accent)"
              onclick="sdBrowse('${f.path.replace(/'/g,"\\'")}')">📁 ${f.name}/</td>
          <td class="right dim">–</td><td class="right dim">–</td><td></td>
        </tr>` : `
        <tr>
          <td class="mono" style="font-size:11px;word-break:break-all">${f.name}</td>
          <td class="right dim" style="white-space:nowrap">${fmt.bytes(f.size_bytes)}</td>
          <td class="right dim" style="white-space:nowrap;font-size:11px">${f.date||'–'}</td>
          <td class="right" style="white-space:nowrap">
            <a href="/files?path=${encodeURIComponent(f.path)}"
               class="btn btn-sm" style="text-decoration:none" download>↓</a>
            <button class="btn btn-sm btn-danger" style="margin-left:3px"
                    onclick="sdDelete('${f.path.replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="4" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
    }
  }

  window.sdBrowse = function(path) { _loadSdAll(path); };
  window.sdDelete = function(path) {
    showConfirm('Briši datoteko', path, async () => {
      try { await api.del('/files?path=' + encodeURIComponent(path)); _loadSdAll(_sdPath); }
      catch(e) { alert('Napaka pri brisanju: ' + e.message); }
    }, 'Briši');
  };

  // ── HTML skeleton ─────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header"><h1>Logi &amp; SD</h1><span class="subtitle" style="color:var(--text3);font-size:10px">v4.3</span></div>

<div class="tab-bar">
  <button class="tab-btn active" id="ltab-ram"    onclick="logsTab('ram')">RAM logi</button>
  <button class="tab-btn"        id="ltab-sdlogs" onclick="logsTab('sdlogs')">SD logi</button>
  <button class="tab-btn"        id="ltab-sd"     onclick="logsTab('sd')">SD datoteke</button>
</div>

<!-- TAB: RAM LOGI -->
<div class="tab-panel active" id="lpanel-ram">
  <div class="log-toolbar">
    <select class="log-select" id="log-lvl" onchange="logFilterLvl(this.value)">
      <option value="ALL">ALL</option>
      <option value="ERROR">ERROR</option>
      <option value="WARN">WARN</option>
      <option value="INFO">INFO</option>
      <option value="DEBUG">DEBUG</option>
    </select>
    <select class="log-select" id="log-mod" onchange="logFilterMod(this.value)" style="width:110px">
      <option value="ALL">ALL</option>
      <option value="BSP">BSP</option>
      <option value="LOGGER">LOGGER</option>
      <option value="LLOGIC">LLOGIC</option>
      <option value="WIFI">WIFI</option>
      <option value="SENSOR">SENSOR</option>
      <option value="RADAR">RADAR</option>
      <option value="TOF">TOF</option>
      <option value="VR">VR</option>
      <option value="ALARM">ALARM</option>
      <option value="PLOG">PLOG</option>
      <option value="WEBUI">WEBUI</option>
      <option value="SDMGR">SDMGR</option>
      <option value="LED">LED</option>
      <option value="GPIO">GPIO</option>
      <option value="LIGHT">LIGHT</option>
      <option value="SCREEN">SCREEN</option>
      <option value="SDFLUSH">SDFLUSH</option>
    </select>
    <div style="display:flex;align-items:center;gap:6px;margin-left:auto;flex-wrap:wrap">
      <label style="display:flex;align-items:center;gap:5px;cursor:pointer;
                    font-size:11px;color:var(--text2);font-family:var(--font)">
        <input type="checkbox" id="log-autoscroll" checked onchange="logAutoScroll(this.checked)">
        scroll
      </label>
      <button class="btn btn-sm" id="log-pause-btn" onclick="logTogglePause()">⏸ Pause</button>
      <button class="btn btn-sm" onclick="logDownloadRam()">↓ RAM</button>
      <button class="btn btn-sm" onclick="logFlush()">💾 Flush SD</button>
      <button class="btn btn-sm" onclick="logClear()">🗑 Clear</button>
    </div>
  </div>

  <div id="log-output" style="max-height:60vh;overflow-y:auto;overflow-x:hidden;word-break:break-all"></div>

  <div class="flex-row mt8"
       style="color:var(--text3);font-size:11px;font-family:var(--font);flex-wrap:wrap;gap:8px">
    <span id="log-count">0 vrstic</span>
    <span id="log-paused-label" style="color:var(--amber);display:none">⏸ PAUSED</span>
    <span style="margin-left:auto" id="log-ts">–</span>
  </div>

  <div style="display:flex;gap:14px;margin-top:8px;font-size:10px;
              font-family:var(--font);flex-wrap:wrap;letter-spacing:0.05em">
    <span style="color:#ef4444">■ ERROR</span>
    <span style="color:#f59e0b">■ WARN</span>
    <span style="color:#c8d0e0">■ INFO</span>
    <span style="color:#4a5570">■ DEBUG</span>
    <span style="color:#06b6d4;margin-left:8px">■ modul</span>
    <span style="color:#4a5570">■ timestamp</span>
  </div>
</div>

<!-- TAB: SD LOGI -->
<div class="tab-panel" id="lpanel-sdlogs">
  <div class="flex-row mb8 flex-between" style="flex-wrap:wrap;gap:8px">
    <span class="text-dim text-tiny" id="sdlog-info">–</span>
    <div style="display:flex;gap:6px">
      <button class="btn btn-sm" onclick="_loadSdLogsPublic()">↻ Osveži</button>
      <button class="btn btn-sm btn-danger" onclick="sdLogDeleteAll()">🗑 Izbriši vse</button>
    </div>
  </div>
  <div class="card" style="padding:0;overflow:hidden">
    <table class="tbl">
      <thead><tr>
        <th>Datoteka</th><th class="right">Velikost</th>
        <th class="right">Datum</th><th class="right" style="width:110px">Akcija</th>
      </tr></thead>
      <tbody id="sdlog-body">
        <tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>
      </tbody>
    </table>
  </div>
  <div id="sdlog-preview" style="display:none;margin-top:16px">
    <div class="flex-row flex-between mb8">
      <span class="section-label" style="margin:0" id="sdlog-preview-title">–</span>
      <div style="display:flex;gap:6px">
        <span class="text-dim text-tiny" style="align-self:center">zadnjih 200 vrstic</span>
        <button class="btn btn-sm" onclick="sdLogPreviewClose()">✕ Zapri</button>
      </div>
    </div>
    <div id="sdlog-preview-output" class="log-output-sm"
         style="max-height:40vh"></div>
  </div>
</div>

<!-- TAB: SD DATOTEKE -->
<div class="tab-panel" id="lpanel-sd">
  <div class="flex-row mb8 flex-between" style="flex-wrap:wrap;gap:8px">
    <div style="display:flex;align-items:center;gap:8px">
      <button class="btn btn-sm" onclick="sdBrowse('/')">/ root</button>
      <span class="text-dim text-tiny" id="sd-curdir">/</span>
    </div>
    <div style="display:flex;align-items:center;gap:8px">
      <span class="text-dim text-tiny" id="sd-diskinfo">–</span>
      <button class="btn btn-sm" onclick="_loadSdAllPublic()">↻ Osveži</button>
    </div>
  </div>
  <div class="card" style="padding:0;overflow:hidden">
    <table class="tbl">
      <thead><tr>
        <th>Ime</th><th class="right">Velikost</th>
        <th class="right">Datum</th><th class="right" style="width:80px">Akcija</th>
      </tr></thead>
      <tbody id="sd-body">
        <tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>
      </tbody>
    </table>
  </div>
</div>
`;
  }

  // ── Globalni vmesnik ─────────────────────────────────────
  window.logsTab = function(tab) {
    document.querySelectorAll('#page-logs .tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('#page-logs .tab-panel').forEach(p => p.classList.remove('active'));
    document.getElementById('ltab-' + tab).classList.add('active');
    document.getElementById('lpanel-' + tab).classList.add('active');
    clearPoller('logs');
    if      (tab === 'ram')    registerPoller('logs', _pollLogs, POLL_MS);
    else if (tab === 'sdlogs') _loadSdLogs();
    else if (tab === 'sd')     _loadSdAll(_sdPath);
  };

  window._loadSdLogsPublic = function() { _loadSdLogs(); };
  window._loadSdAllPublic  = function() { _loadSdAll(_sdPath); };

  window.logFilterLvl = function(v) { _filterLvl = v; _fullRender(); };
  window.logFilterMod = function(v) { _filterMod = (v === 'ALL') ? '' : v; _fullRender(); };
  window.logAutoScroll = function(v) { _autoScroll = v; };

  window.logTogglePause = function() {
    _paused = !_paused;
    const btn   = document.getElementById('log-pause-btn');
    const label = document.getElementById('log-paused-label');
    if (btn)   btn.textContent     = _paused ? '▶ Resume' : '⏸ Pause';
    if (label) label.style.display = _paused ? '' : 'none';
    if (!_paused) _pollLogs();
  };

  window.logDownloadRam = function() { _downloadRam(); };

  window.logFlush = async function() {
    const tsEl = document.getElementById('log-ts');
    try {
      await api.post('/api/logs/flush', {});
      if (tsEl) tsEl.textContent = '💾 Flushed ' + new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      if (tsEl) tsEl.textContent = '⚠ ' + e.message;
    }
  };

  window.logClear = function() {
    _allLines  = [];
    _prevTotal = -1;  // naslednji poll naredi full re-render
    const out = _out();
    if (out) out.innerHTML = '';
    _updateCount(0);
  };

  window.page_logs = function() {
    if (!document.getElementById('log-output')) _render();
    _prevTotal = -1;  // force full refresh ob vsaki vrnitvi na stran
    registerPoller('logs', _pollLogs, POLL_MS);
  };

})();
