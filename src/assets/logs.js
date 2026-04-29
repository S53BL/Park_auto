// ============================================================
// logs.js — Stran: Logi & SD
// Tab 1: Live log stream — polling /api/logs vsakih 2 s
// Tab 2: SD datoteke — browse, download, delete
// ============================================================

(function () {
  const DIV = 'page-logs';
  let _lastLines   = [];
  let _autoScroll  = true;
  let _filterLvl   = 'ALL';
  let _filterMod   = '';

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Logi &amp; SD</h1>
</div>

<div class="tab-bar">
  <button class="tab-btn active" id="ltab-logs" onclick="logsTab('logs')">Live logi</button>
  <button class="tab-btn"        id="ltab-sd"   onclick="logsTab('sd')">SD datoteke</button>
</div>

<!-- TAB: LOGI ──────────────────────────────── -->
<div class="tab-panel active" id="lpanel-logs">
  <div class="log-toolbar">
    <select class="log-select" id="log-lvl" onchange="logFilterLvl(this.value)">
      <option value="ALL">ALL</option>
      <option value="ERROR">ERROR</option>
      <option value="WARN">WARN</option>
      <option value="INFO">INFO</option>
      <option value="DEBUG">DEBUG</option>
    </select>
    <input class="log-select" id="log-mod" placeholder="filter modul…"
           oninput="logFilterMod(this.value)"
           style="width:140px">
    <label style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:12px;color:var(--text2);margin-left:auto">
      <input type="checkbox" id="log-autoscroll" checked onchange="logAutoScroll(this.checked)">
      auto-scroll
    </label>
    <button class="btn btn-sm" onclick="logFlush()">Flush SD</button>
    <button class="btn btn-sm" onclick="logClear()">Clear</button>
  </div>
  <div id="log-output"></div>
  <div class="flex-row mt8" style="color:var(--text3);font-size:11px;font-family:var(--font)">
    <span id="log-count">0 vrstic</span>
    <span style="margin-left:auto" id="log-ts">–</span>
  </div>
</div>

<!-- TAB: SD ────────────────────────────────── -->
<div class="tab-panel" id="lpanel-sd">
  <div class="flex-row mb8 flex-between">
    <span class="text-dim text-tiny" id="sd-diskinfo">–</span>
    <button class="btn btn-sm" onclick="sdRefresh()">↻ Osveži</button>
  </div>
  <div class="card" style="padding:0;overflow:hidden">
    <table class="tbl" id="sd-table">
      <thead><tr>
        <th>Pot</th><th class="right">Velikost</th><th class="right">Datum</th><th></th>
      </tr></thead>
      <tbody id="sd-body">
        <tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>
      </tbody>
    </table>
  </div>
</div>
`;
  }

  // ── Log rendering ────────────────────────────────────────
  function _colorLine(line) {
    // Pričakovani format: [timestamp] [NIVO:MODUL] sporočilo
    let cls = 'll-info';
    if (line.includes(':ERROR]')) cls = 'll-error';
    else if (line.includes(':WARN]'))  cls = 'll-warn';
    else if (line.includes(':DEBUG]')) cls = 'll-debug';

    // Označi timestamp ([ ... ])
    const escaped = line.replace(/&/g,'&amp;').replace(/</g,'&lt;');
    const formatted = escaped
      .replace(/^(\[[^\]]+\])/,   '<span class="ll-ts">$1</span>')
      .replace(/\[([A-Z]+:[^\]]+)\]/, '<span class="ll-mod">[$1]</span>');

    return `<div class="${cls}">${formatted}</div>`;
  }

  function _applyFilter(lines) {
    return lines.filter(l => {
      if (_filterLvl !== 'ALL') {
        const lvls = { ERROR: [':ERROR]'], WARN: [':ERROR]',':WARN]'],
                        INFO:  [':ERROR]',':WARN]',':INFO]'], DEBUG: [] };
        const required = lvls[_filterLvl];
        if (required && required.length > 0 && !required.some(t => l.includes(t))) return false;
      }
      if (_filterMod && !l.toLowerCase().includes(_filterMod.toLowerCase())) return false;
      return true;
    });
  }

  async function _pollLogs() {
    try {
      const url = '/api/logs?lines=400' + (_filterLvl !== 'ALL' ? '&level=' + _filterLvl : '');
      const d   = await api.get(url);
      const lines = Array.isArray(d) ? d : (d.lines || []);
      _lastLines = lines;
      _renderLogs();
      const el = document.getElementById('log-ts');
      if (el) el.textContent = new Date().toLocaleTimeString('sl-SI');
    } catch(e) {
      const el = document.getElementById('log-ts');
      if (el) el.textContent = 'napaka: ' + e.message;
    }
  }

  function _renderLogs() {
    const out = document.getElementById('log-output');
    if (!out) return;
    const filtered = _applyFilter(_lastLines);
    out.innerHTML = filtered.length
      ? filtered.map(_colorLine).join('')
      : '<div class="empty-state">Ni logov</div>';
    const cnt = document.getElementById('log-count');
    if (cnt) cnt.textContent = filtered.length + ' vrstic';
    if (_autoScroll) out.scrollTop = out.scrollHeight;
  }

  // ── SD ───────────────────────────────────────────────────
  async function _loadSd(dir) {
    const tbody = document.getElementById('sd-body');
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="4" class="empty-state loading">Nalagam…</td></tr>';
    try {
      const d = await api.get('/files' + (dir ? '?dir=' + encodeURIComponent(dir) : ''));
      const diskEl = document.getElementById('sd-diskinfo');
      if (diskEl) diskEl.textContent = d.disk_free_mb + ' MB prosto / ' + d.disk_total_mb + ' MB skupaj';
      const files = d.files || [];
      if (files.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty-state">Brez datotek</td></tr>';
        return;
      }
      tbody.innerHTML = files.map(f => `
        <tr>
          <td class="mono" style="word-break:break-all">${f.path}</td>
          <td class="right dim">${fmt.bytes(f.size_bytes)}</td>
          <td class="right dim" style="white-space:nowrap">${f.date || '–'}</td>
          <td class="right" style="white-space:nowrap">
            <a href="/files?path=${encodeURIComponent(f.path)}"
               class="btn btn-sm" style="text-decoration:none" download>↓</a>
            <button class="btn btn-sm btn-danger" style="margin-left:4px"
                    onclick="sdDelete('${f.path.replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="4" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
    }
  }

  // ── Globalne funkcije (dostopne iz HTML) ─────────────────
  window.logsTab = function(tab) {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    document.getElementById('ltab-' + tab).classList.add('active');
    document.getElementById('lpanel-' + tab).classList.add('active');
    if (tab === 'sd') {
      clearPoller('logs');
      _loadSd();
    } else {
      registerPoller('logs', _pollLogs, 2000);
    }
  };

  window.logFilterLvl = function(v) { _filterLvl = v; _renderLogs(); };
  window.logFilterMod = function(v) { _filterMod = v; _renderLogs(); };
  window.logAutoScroll = function(v) { _autoScroll = v; };

  window.logFlush = async function() {
    try {
      await api.post('/api/logs/flush', {});
      const el = document.getElementById('log-ts');
      if (el) el.textContent = 'Flushed: ' + new Date().toLocaleTimeString('sl-SI');
    } catch(e) { alert('Napaka: ' + e.message); }
  };

  window.logClear = function() {
    const out = document.getElementById('log-output');
    if (out) out.innerHTML = '';
    _lastLines = [];
    const cnt = document.getElementById('log-count');
    if (cnt) cnt.textContent = '0 vrstic';
  };

  window.sdRefresh = function() { _loadSd(); };

  window.sdDelete = function(path) {
    showConfirm('Briši datoteko', path, async () => {
      try {
        await api.del('/files?path=' + encodeURIComponent(path));
        _loadSd();
      } catch(e) { alert('Napaka pri brisanju: ' + e.message); }
    }, 'Briši');
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_logs = function () {
    if (!document.getElementById('log-output')) _render();
    // Privzeto: aktiven log tab
    registerPoller('logs', _pollLogs, 2000);
  };
})();
