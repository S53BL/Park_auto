// ============================================================
// vehicles.js — Stran: Vozila
// GET  /api/vehicles?place=A|B             → seznam modelov
// GET  /api/vehicles?place=A&profile=m_001 → profil modela (za prihodnost)
// POST /api/vehicles/rename  { place, id, name }
// DELETE /api/vehicles?id=X&place=A
// ============================================================

(function () {
  const DIV = 'page-vehicles';
  let _place  = 'A';
  let _models = [];

  // ── Pomožna: formatiraj unix timestamp ──────────────────
  function _fmtDate(ts) {
    if (!ts || ts === 0) return '–';
    try {
      const d = new Date(ts * 1000);
      const pad = n => String(n).padStart(2, '0');
      return d.getFullYear() + '-' + pad(d.getMonth()+1) + '-' + pad(d.getDate())
           + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
    } catch(e) { return '–'; }
  }

  // ── Pomožna: vr_state besedilo ───────────────────────────
  function _vrStateLabel(state) {
    switch (state) {
      case 0: return '<span class="badge badge-green">kalibrirano</span>';
      case 1: return '<span class="badge badge-amber">ni kalibr.</span>';
      case 2: return '<span class="badge badge-gray">neznan</span>';
      case 3: return '<span class="badge badge-green">prepoznan</span>';
      default: return '';
    }
  }

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Vozila</h1>
  <span class="subtitle" id="veh-status">–</span>
</div>

<!-- Mesto A / B selector ───────────────────── -->
<div class="flex-row mb12">
  <button class="btn btn-primary" id="veh-btn-a" onclick="vehPlace('A')">Mesto A</button>
  <button class="btn"             id="veh-btn-b" onclick="vehPlace('B')">Mesto B</button>
  <button class="btn btn-sm" style="margin-left:auto" onclick="vehLoad()">↻ Osveži</button>
</div>

<!-- Stanje mesta ────────────────────────────── -->
<div id="veh-place-info" class="card" style="padding:10px 14px;margin-bottom:10px;display:none">
  <div class="flex-row flex-between">
    <span id="veh-place-state-badge"></span>
    <span class="text-dim text-tiny" id="veh-baseline-info"></span>
  </div>
  <div id="veh-cur-vehicle" style="margin-top:4px;display:none">
    Trenutno na mestu: <strong id="veh-cur-name"></strong>
    <span class="text-dim text-tiny" id="veh-cur-dtw"></span>
  </div>
</div>

<!-- Tabela modelov ──────────────────────────── -->
<div class="card" style="padding:0;overflow:hidden">
  <table class="tbl" id="veh-table">
    <thead><tr>
      <th style="width:60px">ID</th>
      <th>Ime</th>
      <th class="right" style="width:80px">Parkiranj</th>
      <th class="right" style="width:130px">Zadnjič</th>
      <th class="right" style="width:60px">DTW</th>
      <th style="width:120px"></th>
    </tr></thead>
    <tbody id="veh-body">
      <tr><td colspan="6" class="empty-state loading">Nalagam…</td></tr>
    </tbody>
  </table>
</div>

<!-- Prazno stanje ───────────────────────────── -->
<div id="veh-empty" style="display:none" class="empty-state mt20">
  Ni modelov za mesto <strong id="veh-empty-place">A</strong>.<br>
  <span class="text-dim" style="font-size:11px">
    Modeli se ustvarijo samodejno ob prvem parkiranju.
  </span>
</div>
`;
  }

  // ── Naloži seznam modelov ─────────────────────────────────
  async function _load() {
    const tbody = document.getElementById('veh-body');
    const st    = document.getElementById('veh-status');
    const empty = document.getElementById('veh-empty');
    const info  = document.getElementById('veh-place-info');
    if (!tbody) return;

    tbody.innerHTML = '<tr><td colspan="6" class="empty-state loading">Nalagam…</td></tr>';
    if (empty) empty.style.display = 'none';
    if (info)  info.style.display  = 'none';

    try {
      const d = await api.get('/api/vehicles?place=' + _place);
      _models = d.models || [];

      // Prikaz stanja mesta
      if (info) {
        info.style.display = 'block';
        const badge = document.getElementById('veh-place-state-badge');
        const blInfo = document.getElementById('veh-baseline-info');
        if (badge) badge.innerHTML = _vrStateLabel(d.vr_state);
        if (blInfo) blInfo.textContent =
          d.baseline_valid ? 'Baseline: kalibriran' : 'Baseline: ni kalibriran';
        // Trenutno vozilo
        const curDiv = document.getElementById('veh-cur-vehicle');
        if (curDiv) {
          if (d.vehicle_name && d.vehicle_name.length > 0) {
            curDiv.style.display = 'block';
            const curName = document.getElementById('veh-cur-name');
            const curDtw  = document.getElementById('veh-cur-dtw');
            if (curName) curName.textContent = d.vehicle_name;
          } else {
            curDiv.style.display = 'none';
          }
        }
      }

      if (st) st.textContent = 'Mesto ' + _place + ' — ' + _models.length + ' modelov';

      if (_models.length === 0) {
        tbody.innerHTML = '';
        if (empty) {
          empty.style.display = 'block';
          const ep = document.getElementById('veh-empty-place');
          if (ep) ep.textContent = _place;
        }
        return;
      }

      tbody.innerHTML = _models.map((m, idx) => {
        const onPlace = m.on_place
          ? ' style="background:var(--card-bg2,rgba(52,211,153,.08))"' : '';
        const dtwStr = (m.lastDtw !== undefined && m.lastDtw !== null)
          ? m.lastDtw.toFixed(1) : '–';
        return `<tr id="veh-row-${idx}"${onPlace}>
          <td class="mono dim">${m.id || idx}</td>
          <td id="veh-name-${idx}">
            <span class="veh-label">${m.name || 'neznan'}</span>
            ${m.on_place ? ' <span class="badge badge-green" style="font-size:10px">tu</span>' : ''}
          </td>
          <td class="right dim">${m.repetitions !== undefined ? m.repetitions : '–'}</td>
          <td class="right dim" style="white-space:nowrap">${_fmtDate(m.lastSeen)}</td>
          <td class="right dim">${dtwStr}</td>
          <td class="right" style="white-space:nowrap">
            <button class="btn btn-sm"
                    onclick="vehRenameStart(${idx},'${(m.name||'').replace(/'/g,"\\'").replace(/"/g,"&quot;")}')">
              ↩ Preimenuj
            </button>
            <button class="btn btn-sm btn-danger" style="margin-left:4px"
                    onclick="vehDelete('${(m.id||idx).toString().replace(/'/g,"\\'")}')">
              ✕
            </button>
          </td>
        </tr>`;
      }).join('');

    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="6" class="text-dim" style="padding:10px">
        Napaka: ${e.message}</td></tr>`;
      if (st) st.textContent = 'Napaka';
    }
  }

  // ── Globalne funkcije ────────────────────────────────────
  window.vehLoad = function () { _load(); };

  window.vehPlace = function(p) {
    _place = p;
    document.getElementById('veh-btn-a').className =
      'btn' + (p === 'A' ? ' btn-primary' : '');
    document.getElementById('veh-btn-b').className =
      'btn' + (p === 'B' ? ' btn-primary' : '');
    _load();
  };

  window.vehRenameStart = function(idx, currentName) {
    const cell = document.getElementById('veh-name-' + idx);
    if (!cell) return;
    cell.innerHTML = `
      <div class="inline-edit">
        <input type="text" id="veh-rename-input-${idx}"
               value="${currentName}"
               maxlength="31"
               onkeydown="if(event.key==='Enter')vehRenameSubmit(${idx});
                          if(event.key==='Escape')vehLoad()">
        <button class="btn btn-sm btn-primary"
                onclick="vehRenameSubmit(${idx})">OK</button>
        <button class="btn btn-sm" onclick="vehLoad()">×</button>
      </div>`;
    const inp = document.getElementById('veh-rename-input-' + idx);
    if (inp) { inp.focus(); inp.select(); }
  };

  window.vehRenameSubmit = async function(idx) {
    const inp = document.getElementById('veh-rename-input-' + idx);
    if (!inp) return;
    const newName = inp.value.trim();
    if (!newName) return;
    const model = _models[idx];
    const st = document.getElementById('veh-status');
    try {
      if (st) st.textContent = 'Preimenujem…';
      const r = await api.post('/api/vehicles/rename', {
        id:    model ? model.id : String(idx),
        place: _place,
        name:  newName
      });
      if (st) st.textContent = r.ok ? 'Preimenovano ✓' : ('Napaka: ' + (r.error || '?'));
      _load();
    } catch(e) {
      if (st) st.textContent = 'Napaka: ' + e.message;
    }
  };

  window.vehDelete = function(id) {
    showConfirm(
      'Izbriši model',
      'Model ID: ' + id + ' — brisanje je trajno. Raw profili na SD ostanejo.',
      async () => {
        const st = document.getElementById('veh-status');
        try {
          if (st) st.textContent = 'Brišem…';
          const r = await api.del(
            '/api/vehicles?id=' + encodeURIComponent(id) + '&place=' + _place);
          if (st) st.textContent = r.ok ? 'Izbrisano ✓' : ('Napaka: ' + (r.error || '?'));
          _load();
        } catch(e) {
          if (st) st.textContent = 'Napaka: ' + e.message;
        }
      },
      'Izbriši'
    );
  };

  // ── Init (kliče ga router) ───────────────────────────────
  window.page_vehicles = function () {
    if (!document.getElementById('veh-table')) _render();
    _load();
  };
})();
