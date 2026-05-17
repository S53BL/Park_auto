// ============================================================
// logs.js — Stran: Logi & SD  (v2)
// Tab 1: RAM logi  — polling /api/logs, filtri, obarvanost, download
// Tab 2: SD logi   — browse samo log datotek, preview, download, delete
// Tab 3: SD vse    — browse celotne SD kartice, download, delete
//
// SPREMEMBE glede na v1:
//   - Tab "Live logi" → "RAM logi": polling 2s, filter level+modul,
//     obarvanost vrstic (ERROR/WARN/INFO/DEBUG), download RAM bufferja
//   - Dodan Tab "SD logi": prikaže samo datoteke v /logs/ mapi,
//     preview zadnjih N vrstic, download, delete posamezne in bulk
//   - Tab "SD datoteke": browse celotne SD, navigacija po mapah,
//     download, delete — ohranjen iz v1 z izboljšavami
//   - Filter po modulu je client-side (API podpira samo ?level=)
//   - Modul filter: tipkanje filtrira takoj, brez reload
//   - Obarvanost: ERROR=rdeča, WARN=rumena, INFO=bela, DEBUG=siva
//     + timestamp in modul tag sta posebej obarvana
//   - DOM limit 500 vrstic — preprečuje upočasnitev brskalnika
//   - Pause tipka zaustavi polling brez izgube podatkov
// ============================================================

(function () {
  const DIV          = 'page-logs';
  const DOM_LIMIT    = 500;     // max vrstic v log-output DOM-u
  const POLL_MS      = 5000;    // interval pollinga RAM logov
  const LINES_FETCH  = 200;     // koliko vrstic zahtevamo od API

  let _lastLines  = [];         // zadnje prejete vrstice (nefiltriran buffer)
  let _autoScroll = true;
  let _paused     = false;
  let _filterLvl  = 'ALL';
  let _filterMod  = '';
  let _sdPath     = '/';        // trenutna SD pot za "SD vse" tab

  // ── Log format ───────────────────────────────────────────
  // Pričakovani format: [2026-05-13 14:32:01] [INFO:HAL_GPIO] sporočilo
  // ali krajši:         [14:32:01] [WARN:WEBUI] sporočilo

  function _levelOf(line) {
    // Format: "HH:MM:SS|[TAG:E] msg"  (E/W/I/D — ena črka)
    const m = line.match(/\[[A-Z0-9_]+:([EWID])\]/);
    if (!m) return 'info';
    return {E: 'error', W: 'warn', D: 'debug'}[m[1]] || 'info';
  }

  // Barve hardkodirane — brez odvisnosti od style.css (prepreči browser cache problem)
  const _COL = { error:'#ef4444', warn:'#f59e0b', debug:'#4a5570', info:'#c8d0e0' };
  const _COL_TS  = '#4a5570';   // timestamp
  const _COL_MOD = '#06b6d4';   // modul tag

  function _colorLine(line) {
    const lvl = _levelOf(line);
    const col = _COL[lvl];
    const escaped = line
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');

    // Format: "14:32:01|[TAG:L] msg"  ali  "M0000123456|[TAG:L] msg"
    // Container ima white-space:pre-wrap, \n med spani = nova vrstica.
    const fmt = escaped
      .replace(/^(\d{2}:\d{2}:\d{2}|M\d{6,12})/, `<span style="color:${_COL_TS}">$1</span>`)
      .replace(/(\[[A-Z0-9_]+:[EWID]\])/, `<span style="color:${_COL_MOD}">$1</span>`);

    return `<span style="color:${col}">${fmt}</span>`;
  }

  function _applyFilter(lines) {
    return lines.filter(l => {
      // Level filter — inclusive (ERROR prikaže samo ERROR, WARN prikaže WARN+ERROR, ...)
      if (_filterLvl !== 'ALL') {
        const show = {
          ERROR: ['error'],
          WARN:  ['error','warn'],
          INFO:  ['error','warn','info'],
          DEBUG: ['error','warn','info','debug']
        }[_filterLvl] || [];
        if (!show.includes(_levelOf(l))) return false;
      }
      // Modul filter — client-side, case-insensitive
      if (_filterMod) {
        if (!l.toLowerCase().includes(_filterMod.toLowerCase())) return false;
      }
      return true;
    });
  }

  // ── Render log output ─────────────────────────────────────
  function _renderLogs() {
    const out = document.getElementById('log-output');
    if (!out) return;

    const filtered = _applyFilter(_lastLines);
    // Omeji DOM na zadnjih DOM_LIMIT vrstic
    const visible  = filtered.slice(-DOM_LIMIT);

    if (visible.length === 0) {
      out.innerHTML = '<div class="ll-empty">Ni logov' +
        (_filterLvl !== 'ALL' || _filterMod ? ' (aktiven filter)' : '') + '</div>';
    } else {
      out.innerHTML = visible.map(_colorLine).join('\n');
    }

    // Šteje: filtered / total
    const cnt = document.getElementById('log-count');
    if (cnt) {
      const trimmed = filtered.length > DOM_LIMIT
        ? ` <span class="text-dim">(prikazanih ${DOM_LIMIT} od ${filtered.length})</span>` : '';
      cnt.innerHTML = filtered.length + ' vrstic' + trimmed;
    }

    if (_autoScroll && !_paused) out.scrollTop = out.scrollHeight;
  }

  // ── Poll RAM logov ────────────────────────────────────────
  async function _pollLogs() {
    if (_paused) return;
    try {
      const url = '/api/logs?lines=' + LINES_FETCH;
      const d = await api.get(url);
      _lastLines = Array.isArray(d) ? d : (d.lines || d || []);
      _renderLogs();
      const ts = document.getElementById('log-ts');
      if (ts) ts.textContent = new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      const ts = document.getElementById('log-ts');
      if (ts) ts.textContent = '⚠ ' + e.message;
    }
  }

  // ── Download RAM buffer kot .txt ──────────────────────────
  function _downloadRam() {
    const text = _lastLines.join('\n');
    const blob = new Blob([text], { type: 'text/plain' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = 'parking_ram_' + new Date().toISOString().slice(0,19).replace(/[T:]/g,'-') + '.log';
    a.click();
    URL.revokeObjectURL(url);
  }

  // ── SD log datoteke (/logs/ mapa) ─────────────────────────
  async function _loadSdLogs() {
    const tbody = document.getElementById('sdlog-body');
    const info  = document.getElementById('sdlog-info');
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="5" class="empty-state loading">Nalagam…</td></tr>';

    try {
      const d = await api.get('/files?dir=/logs');
      const files = (d.files || []).filter(f => f.name && f.name.endsWith('.log'));

      if (info) info.textContent = files.length + ' log datotek · ' +
        d.disk_free_mb + ' MB prosto / ' + d.disk_total_mb + ' MB';

      if (files.length === 0) {
        tbody.innerHTML = '<tr><td colspan="5" class="empty-state">Ni log datotek v /logs/</td></tr>';
        return;
      }

      // Razvrsti po datumu — najnovejše zgoraj
      files.sort((a, b) => (b.date || '').localeCompare(a.date || ''));

      tbody.innerHTML = files.map(f => `
        <tr>
          <td class="mono" style="word-break:break-all;font-size:11px">${f.name}</td>
          <td class="right dim" style="white-space:nowrap">${fmt.bytes(f.size_bytes)}</td>
          <td class="right dim" style="white-space:nowrap;font-size:11px">${f.date || '–'}</td>
          <td class="right" style="white-space:nowrap">
            <button class="btn btn-sm" onclick="sdLogPreview('${f.path.replace(/'/g,"\\'")}')">👁</button>
            <a href="/files?path=${encodeURIComponent(f.path)}"
               class="btn btn-sm" style="text-decoration:none;margin-left:3px" download>↓</a>
            <button class="btn btn-sm btn-danger" style="margin-left:3px"
                    onclick="sdLogDelete('${f.path.replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="5" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
    }
  }

  // ── Preview SD log datoteke (zadnjih 200 vrstic) ──────────
  window.sdLogPreview = async function(path) {
    const panel  = document.getElementById('sdlog-preview');
    const title  = document.getElementById('sdlog-preview-title');
    const output = document.getElementById('sdlog-preview-output');
    if (!panel || !output) return;

    panel.style.display = 'block';
    if (title) title.textContent = path.split('/').pop();
    output.innerHTML = '<div class="ll-empty loading">Nalagam…</div>';

    try {
      // Prenesemo datoteko — ESPAsyncWebServer vrne raw besedilo
      const r = await fetch('/files?path=' + encodeURIComponent(path),
                            { signal: AbortSignal.timeout(10000) });
      if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
      const text  = await r.text();
      const lines = text.split('\n').filter(l => l.trim());
      // Prikaži zadnjih 200 vrstic (datoteka je lahko velika)
      const tail  = lines.slice(-200);
      output.innerHTML = tail.length
        ? tail.map(_colorLine).join('\n')
        : '<span class="ll-empty">Prazna datoteka</span>';
      output.scrollTop = output.scrollHeight;
    } catch(e) {
      output.innerHTML = `<span class="ll-error">Napaka: ${e.message}</span>`;
    }
  };

  window.sdLogPreviewClose = function() {
    const panel = document.getElementById('sdlog-preview');
    if (panel) panel.style.display = 'none';
  };

  window.sdLogDelete = function(path) {
    showConfirm('Izbriši log', path.split('/').pop(), async () => {
      try {
        await api.del('/files?path=' + encodeURIComponent(path));
        _loadSdLogs();
      } catch(e) { alert('Napaka: ' + e.message); }
    }, 'Izbriši');
  };

  window.sdLogDeleteAll = function() {
    showConfirm('Izbriši VSE log datoteke',
      'Vse datoteke v /logs/ bodo trajno izbrisane!',
      async () => {
        try {
          const d = await api.get('/files?dir=/logs');
          const files = (d.files || []).filter(f => f.name && f.name.endsWith('.log'));
          for (const f of files) {
            await api.del('/files?path=' + encodeURIComponent(f.path));
          }
          _loadSdLogs();
        } catch(e) { alert('Napaka: ' + e.message); }
      }, 'Izbriši vse');
  };

  // ── SD browse (vse datoteke) ──────────────────────────────
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

      // Razvrsti: mape pred datotekami, nato abecedno
      files.sort((a, b) => {
        if (a.is_dir !== b.is_dir) return (b.is_dir ? 1 : 0) - (a.is_dir ? 1 : 0);
        return (a.name || '').localeCompare(b.name || '');
      });

      tbody.innerHTML = files.map(f => {
        const isDir = !!f.is_dir;
        if (isDir) {
          return `<tr>
            <td class="mono" style="cursor:pointer;color:var(--accent)"
                onclick="sdBrowse('${f.path.replace(/'/g,"\\'")}')">📁 ${f.name}/</td>
            <td class="right dim">–</td>
            <td class="right dim">–</td>
            <td></td>
          </tr>`;
        }
        return `<tr>
          <td class="mono" style="font-size:11px;word-break:break-all">${f.name}</td>
          <td class="right dim" style="white-space:nowrap">${fmt.bytes(f.size_bytes)}</td>
          <td class="right dim" style="white-space:nowrap;font-size:11px">${f.date || '–'}</td>
          <td class="right" style="white-space:nowrap">
            <a href="/files?path=${encodeURIComponent(f.path)}"
               class="btn btn-sm" style="text-decoration:none" download>↓</a>
            <button class="btn btn-sm btn-danger" style="margin-left:3px"
                    onclick="sdDelete('${f.path.replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`;
      }).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="4" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
    }
  }

  window.sdBrowse = function(path) { _loadSdAll(path); };

  window.sdDelete = function(path) {
    showConfirm('Briši datoteko', path, async () => {
      try {
        await api.del('/files?path=' + encodeURIComponent(path));
        _loadSdAll(_sdPath);
      } catch(e) { alert('Napaka pri brisanju: ' + e.message); }
    }, 'Briši');
  };

  // ── Skeleton HTML ─────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Logi &amp; SD</h1>
</div>

<div class="tab-bar">
  <button class="tab-btn active" id="ltab-ram"    onclick="logsTab('ram')">RAM logi</button>
  <button class="tab-btn"        id="ltab-sdlogs" onclick="logsTab('sdlogs')">SD logi</button>
  <button class="tab-btn"        id="ltab-sd"     onclick="logsTab('sd')">SD datoteke</button>
</div>

<!-- ── TAB: RAM LOGI ────────────────────────── -->
<div class="tab-panel active" id="lpanel-ram">

  <div class="log-toolbar">
    <!-- Level filter -->
    <select class="log-select" id="log-lvl" onchange="logFilterLvl(this.value)" title="Filter po nivoju">
      <option value="ALL">ALL</option>
      <option value="ERROR">ERROR</option>
      <option value="WARN">WARN</option>
      <option value="INFO">INFO</option>
      <option value="DEBUG">DEBUG</option>
    </select>

    <!-- Modul filter -->
    <input class="log-select" id="log-mod" placeholder="modul… (hal_tof, WEBUI…)"
           oninput="logFilterMod(this.value)" style="width:160px" title="Filter po modulu (client-side)">

    <!-- Desna stran -->
    <div style="display:flex;align-items:center;gap:6px;margin-left:auto;flex-wrap:wrap">
      <label style="display:flex;align-items:center;gap:5px;cursor:pointer;
                    font-size:11px;color:var(--text2);font-family:var(--font)">
        <input type="checkbox" id="log-autoscroll" checked onchange="logAutoScroll(this.checked)">
        scroll
      </label>
      <button class="btn btn-sm" id="log-pause-btn" onclick="logTogglePause()"
              title="Zaustavi/nadaljuj polling">⏸ Pause</button>
      <button class="btn btn-sm" onclick="logDownloadRam()"
              title="Prenesi RAM buffer kot .log datoteko">↓ RAM</button>
      <button class="btn btn-sm" onclick="logFlush()"
              title="Prisili zapis RAM bufferja na SD kartico">💾 Flush SD</button>
      <button class="btn btn-sm" onclick="logClear()"
              title="Počisti prikaz v brskalniku (ne na SD)">🗑 Clear</button>
    </div>
  </div>

  <!-- Log output — white-space:pre-wrap inline, neodvisno od style.css -->
  <div id="log-output" style="white-space:pre-wrap;overflow-wrap:break-word"></div>

  <!-- Footer stats -->
  <div class="flex-row mt8" style="color:var(--text3);font-size:11px;font-family:var(--font);flex-wrap:wrap;gap:8px">
    <span id="log-count">0 vrstic</span>
    <span id="log-paused-label" style="color:var(--amber);display:none">⏸ PAUSED</span>
    <span style="margin-left:auto" id="log-ts">–</span>
  </div>

  <!-- Legenda -->
  <div style="display:flex;gap:14px;margin-top:10px;font-family:var(--font);font-size:10px;
              letter-spacing:0.06em;flex-wrap:wrap">
    <span style="color:var(--red)">■ ERROR</span>
    <span style="color:var(--amber)">■ WARN</span>
    <span style="color:var(--text)">■ INFO</span>
    <span style="color:var(--text3)">■ DEBUG</span>
    <span style="color:var(--cyan);margin-left:8px">■ modul tag</span>
    <span style="color:var(--text3);margin-left:0">■ timestamp</span>
  </div>
</div>

<!-- ── TAB: SD LOGI ─────────────────────────── -->
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
        <th>Datoteka</th>
        <th class="right">Velikost</th>
        <th class="right">Datum</th>
        <th class="right" style="width:110px">Akcija</th>
      </tr></thead>
      <tbody id="sdlog-body">
        <tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>
      </tbody>
    </table>
  </div>

  <!-- Preview panel -->
  <div id="sdlog-preview" style="display:none;margin-top:16px">
    <div class="flex-row flex-between mb8">
      <span class="section-label" style="margin:0" id="sdlog-preview-title">–</span>
      <div style="display:flex;gap:6px">
        <span class="text-dim text-tiny" style="align-self:center">zadnjih 200 vrstic</span>
        <button class="btn btn-sm" onclick="sdLogPreviewClose()">✕ Zapri</button>
      </div>
    </div>
    <div id="sdlog-preview-output" class="log-output-sm" style="white-space:pre-wrap;overflow-wrap:break-word"></div>
  </div>
</div>

<!-- ── TAB: SD DATOTEKE ──────────────────────── -->
<div class="tab-panel" id="lpanel-sd">
  <div class="flex-row mb8 flex-between" style="flex-wrap:wrap;gap:8px">
    <div style="display:flex;align-items:center;gap:8px">
      <button class="btn btn-sm" onclick="sdBrowse('/')" title="Korenski imenik">/ root</button>
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
        <th>Ime</th>
        <th class="right">Velikost</th>
        <th class="right">Datum</th>
        <th class="right" style="width:80px">Akcija</th>
      </tr></thead>
      <tbody id="sd-body">
        <tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>
      </tbody>
    </table>
  </div>
</div>
`;
  }

  // ── Globalne funkcije ────────────────────────────────────
  window.logsTab = function(tab) {
    document.querySelectorAll('#page-logs .tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('#page-logs .tab-panel').forEach(p => p.classList.remove('active'));
    document.getElementById('ltab-' + tab).classList.add('active');
    document.getElementById('lpanel-' + tab).classList.add('active');

    clearPoller('logs');
    if (tab === 'ram') {
      registerPoller('logs', _pollLogs, POLL_MS);
    } else if (tab === 'sdlogs') {
      _loadSdLogs();
    } else if (tab === 'sd') {
      _loadSdAll(_sdPath);
    }
  };

  // Public wrappers za inline HTML onclick klice
  window._loadSdLogsPublic = function() { _loadSdLogs(); };
  window._loadSdAllPublic  = function() { _loadSdAll(_sdPath); };

  window.logFilterLvl = function(v) {
    _filterLvl = v;
    _renderLogs();
  };
  window.logFilterMod = function(v) {
    _filterMod = v;
    _renderLogs();
  };
  window.logAutoScroll = function(v) { _autoScroll = v; };

  window.logTogglePause = function() {
    _paused = !_paused;
    const btn   = document.getElementById('log-pause-btn');
    const label = document.getElementById('log-paused-label');
    if (btn)   btn.textContent   = _paused ? '▶ Resume' : '⏸ Pause';
    if (label) label.style.display = _paused ? '' : 'none';
    if (!_paused) _pollLogs();  // takoj osveži ob resume
  };

  window.logDownloadRam = function() { _downloadRam(); };

  window.logFlush = async function() {
    const ts = document.getElementById('log-ts');
    try {
      await api.post('/api/logs/flush', {});
      if (ts) ts.textContent = '💾 Flushed ' + new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      if (ts) ts.textContent = '⚠ ' + e.message;
    }
  };

  window.logClear = function() {
    _lastLines = [];
    _renderLogs();
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_logs = function () {
    if (!document.getElementById('log-output')) _render();
    registerPoller('logs', _pollLogs, POLL_MS);
  };

})();
