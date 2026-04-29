// ============================================================
// vehicles.js — Stran: Vozila
// GET /api/vehicles?place=A|B
// POST /api/vehicles/rename  { id, place, name }
// DELETE /api/vehicles?id=X&place=A
// ============================================================

(function () {
  const DIV = 'page-vehicles';
  let _place = 'A';
  let _models = [];

  // ── Skeleton ─────────────────────────────────────────────
  function _render() {
    document.getElementById(DIV).innerHTML = `
<div class="page-header">
  <h1>Vozila</h1>
  <span class="subtitle" id="veh-status">–</span>
</div>

<!-- Selector A/B ────────────────────────────── -->
<div class="flex-row mb12">
  <button class="btn btn-primary" id="veh-btn-a" onclick="vehPlace('A')">Mesto A</button>
  <button class="btn"             id="veh-btn-b" onclick="vehPlace('B')">Mesto B</button>
  <button class="btn btn-sm" style="margin-left:auto" onclick="vehLoad()">↻ Osveži</button>
</div>

<!-- Tabela modelov ──────────────────────────── -->
<div class="card" style="padding:0;overflow:hidden">
  <table class="tbl" id="veh-table">
    <thead><tr>
      <th>ID</th>
      <th>Ime</th>
      <th class="right">Parkiranj</th>
      <th class="right">Zadnjič viden</th>
      <th></th>
    </tr></thead>
    <tbody id="veh-body">
      <tr><td colspan="5" class="empty-state loading">Nalagam…</td></tr>
    </tbody>
  </table>
</div>

<!-- Prazno stanje ───────────────────────────── -->
<div id="veh-empty" style="display:none" class="empty-state mt20">
  Ni modelov za mesto <strong id="veh-empty-place">A</strong>.<br>
  <span class="text-dim" style="font-size:11px">Modeli se ustvarijo samodejno ob prvem parkiranju.</span>
</div>
`;
  }

  // ── Naloži seznam modelov ─────────────────────────────────
  async function _load() {
    const tbody = document.getElementById('veh-body');
    const st    = document.getElementById('veh-status');
    const empty = document.getElementById('veh-empty');
    if (!tbody) return;

    tbody.innerHTML = '<tr><td colspan="5" class="empty-state loading">Nalagam…</td></tr>';
    if (empty) empty.style.display = 'none';

    try {
      const d = await api.get('/api/vehicles?place=' + _place);
      _models = d.models || [];
      if (st) st.textContent = d._stub ? 'Stub — vehicle_recog ni implementiran' : 'Mesto ' + _place;

      if (_models.length === 0) {
        tbody.innerHTML = '';
        if (empty) {
          empty.style.display = 'block';
          const ep = document.getElementById('veh-empty-place');
          if (ep) ep.textContent = _place;
        }
        return;
      }

      tbody.innerHTML = _models.map((m, idx) => `
        <tr id="veh-row-${idx}">
          <td class="mono dim">${m.id || idx}</td>
          <td id="veh-name-${idx}">
            <span class="veh-label">${m.name || 'neznan'}</span>
          </td>
          <td class="right dim">${m.park_count !== undefined ? m.park_count : '–'}</td>
          <td class="right dim" style="white-space:nowrap">${m.last_seen || '–'}</td>
          <td class="right" style="white-space:nowrap">
            <button class="btn btn-sm" onclick="vehRenameStart(${idx},'${(m.name||'').replace(/'/g,"\\'")}')">↩ Preimenuj</button>
            <button class="btn btn-sm btn-danger" style="margin-left:4px"
                    onclick="vehDelete('${(m.id||idx).toString().replace(/'/g,"\\'")}')">✕</button>
          </td>
        </tr>`).join('');
    } catch(e) {
      tbody.innerHTML = `<tr><td colspan="5" class="text-dim" style="padding:10px">Napaka: ${e.message}</td></tr>`;
      if (st) st.textContent = 'Napaka';
    }
  }

  // ── Globalne funkcije ────────────────────────────────────
  window.vehLoad = function () { _load(); };

  window.vehPlace = function(p) {
    _place = p;
    document.getElementById('veh-btn-a').className = 'btn' + (p === 'A' ? ' btn-primary' : '');
    document.getElementById('veh-btn-b').className = 'btn' + (p === 'B' ? ' btn-primary' : '');
    _load();
  };

  window.vehRenameStart = function(idx, currentName) {
    const cell = document.getElementById('veh-name-' + idx);
    if (!cell) return;
    cell.innerHTML = `
      <div class="inline-edit">
        <input type="text" id="veh-rename-input-${idx}" value="${currentName}"
               onkeydown="if(event.key==='Enter')vehRenameSubmit(${idx});if(event.key==='Escape')vehLoad()">
        <button class="btn btn-sm btn-primary" onclick="vehRenameSubmit(${idx})">OK</button>
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
        id: model ? model.id : idx, place: _place, name: newName
      });
      if (st) st.textContent = r._stub ? 'Stub — rename ni implementiran' : 'Preimenovano ✓';
      _load();
    } catch(e) {
      if (st) st.textContent = 'Napaka: ' + e.message;
    }
  };

  window.vehDelete = function(id) {
    showConfirm('Izbriši model', 'Model ID: ' + id + ' — brisanje je trajno.', async () => {
      const st = document.getElementById('veh-status');
      try {
        if (st) st.textContent = 'Brišem…';
        const r = await api.del('/api/vehicles?id=' + encodeURIComponent(id) + '&place=' + _place);
        if (st) st.textContent = r._stub ? 'Stub — delete ni implementiran' : 'Izbrisano ✓';
        _load();
      } catch(e) {
        if (st) st.textContent = 'Napaka: ' + e.message;
      }
    }, 'Izbriši');
  };

  // ── Init ─────────────────────────────────────────────────
  window.page_vehicles = function () {
    if (!document.getElementById('veh-table')) _render();
    _load();
  };
})();
