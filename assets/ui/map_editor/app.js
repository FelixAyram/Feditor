'use strict';

/** Hot-reload UI — edit this file / panel.css / index.html and press F5. No C++ rebuild. */

const TOOLS = [
  { id: 0, label: 'Tile', short: 'Tile', group: 'Mapa' },
  { id: 1, label: 'Objetos', short: 'Obj', group: 'Mapa' },
  { id: 2, label: 'Splines', short: 'Spl', group: 'Mapa' },
  { id: 3, label: 'Terreno', short: 'Terr', group: 'Mundo' },
  { id: 4, label: 'Tráfico', short: 'Traf', group: 'Mundo' },
  { id: 5, label: 'Vías', short: 'Vías', group: 'Mundo' },
  { id: 6, label: 'Estaciones', short: 'Est', group: 'Red' },
  { id: 7, label: 'Spl-Exp', short: 'Exp', group: 'Red' },
  { id: 8, label: 'Señales', short: 'Sig', group: 'Red' },
];

const TERRAIN_MODES = [
  { id: 0, label: 'Subir' },
  { id: 1, label: 'Bajar' },
  { id: 2, label: 'Altura fija' },
  { id: 3, label: 'Pintar capa' },
  { id: 4, label: 'Borrar capa' },
];

let state = {
  tool: 2,
  mapName: '',
  status: '',
  selection: '',
  stats: { tiles: 0, objects: 0, splines: 0 },
  catalog: [],
  catalogSel: -1,
  catalogFilter: '',
  catalogFocus: '',
  instanceFilter: '',
  instances: [],
  instanceSel: -1,
  instanceId: 0,
  instanceFocus: '',
  instanceRange: '',
  instanceOffset: 0,
  instanceTotal: 0,
  spline: null,
  object: null,
  terrain: {
    mode: 0,
    brushSize: 20,
    speed: 3,
    setHeight: 0,
    layerSel: 0,
    layers: [],
  },
  spawn: {
    showSplineEnds: true,
    showPathEnds: false,
    showPathRails: false,
    autofillEnds: false,
    stickTerrain: false,
    spawnMeshes: false,
    alignPitchBank: false,
    spawnRandom: false,
    spawnRandomMode: 0,
    spawnRandomFavCat: 0,
    spawnRandomFolder: '',
    spawnRandomFolderCount: 0,
  },
  favCats: [],
  openSections: {
    instances: true,
    transform: true,
    geometry: true,
    actions: true,
    options: false,
    terrBrush: true,
    terrLayers: true,
    terrHelp: false,
  },
};

function post(type, data) {
  if (typeof omsiPost === 'function') omsiPost(JSON.stringify({ type, ...data }));
}

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

function section(id, title, bodyHtml) {
  const open = state.openSections[id] !== false;
  return `<section class="section" data-section="${id}">
    <button type="button" class="section-h" data-toggle-section="${id}">
      <span>${open ? '▾' : '▸'}</span><span>${title}</span>
    </button>
    <div class="section-b${open ? '' : ' hidden'}">${bodyHtml}</div>
  </section>`;
}

function btn(label, action, kind) {
  const cls = kind === 'primary' ? ' primary' : (kind === 'ghost' ? ' ghost' : '');
  return `<button type="button" class="btn${cls}" data-action="${action}">${label}</button>`;
}

/** Local cache; authoritative source is map_root/.omsi_f4_editor.cfg [menu_steps] via C++. */
const fieldSteps = (() => {
  try { return JSON.parse(localStorage.getItem('omsiFieldSteps') || '{}') || {}; }
  catch (_) { return {}; }
})();

function getStep(id, def) {
  const n = Number(fieldSteps[id]);
  return Number.isFinite(n) && n > 0 ? n : (Number(def) || 0.1);
}

function setStep(id, v, persist) {
  const n = Number(v);
  if (!Number.isFinite(n) || n <= 0) return;
  fieldSteps[id] = n;
  try { localStorage.setItem('omsiFieldSteps', JSON.stringify(fieldSteps)); } catch (_) {}
  if (persist !== false) post('set_field_step', { field: id, step: n });
}

function mergeFieldSteps(src) {
  if (!src || typeof src !== 'object') return;
  Object.keys(src).forEach((k) => {
    const n = Number(src[k]);
    if (Number.isFinite(n) && n > 0) fieldSteps[k] = n;
  });
  try { localStorage.setItem('omsiFieldSteps', JSON.stringify(fieldSteps)); } catch (_) {}
}

function postFieldLive(id, value) {
  post('field', { field: id, value: Number(value) });
}

function field(label, id, value, step, unit) {
  const st = getStep(id, step);
  return `<label class="field">
    <span>${label}</span>
    <input type="number" class="scrub-input" data-field="${id}" step="${st}" value="${value ?? 0}" title="Arrastrar = scrub · click = escribir · step ${st}">
    <span class="spin-col" title="step ${st}">
      <button type="button" data-spin="${id}" data-dir="1" tabindex="-1">▲</button>
      <button type="button" data-spin="${id}" data-dir="-1" tabindex="-1">▼</button>
    </span>
    <button type="button" class="gear-btn" data-step-gear="${id}" title="Cambiar step (ahora ${st})">⚙</button>
    <span class="unit">${unit || ''}</span>
  </label>`;
}

function filterMatch(it, needle) {
  if (!needle) return true;
  const hay = ((it.label || '') + ' ' + (it.path || '')).toLowerCase();
  return hay.includes(needle.toLowerCase());
}

function listBlock(title, items, selKey, filterKey) {
  const sel = state[selKey];
  const selId = selKey === 'instanceSel' ? state.instanceId : null;
  const needle = state[filterKey] || '';
  let rowsHtml = '';
  let countLabel = '0';

  if (selKey === 'instanceSel') {
    // C++ already windowed (~64 rows). Selected pick is forced to index 0 of this array.
    countLabel = state.instanceRange || String((items || []).length);
    rowsHtml = (items || []).map((it) => {
      const gi = (typeof it.index === 'number') ? it.index : 0;
      const on = (selId && typeof it.id === 'number') ? (it.id === selId) : (gi === sel);
      return `<button type="button" data-select="${selKey}" data-index="${gi}" data-id="${it.id || 0}" class="${on ? 'sel' : ''}">${esc(it.label || it)}</button>`;
    }).join('');
  } else {
    const filtered = [];
    (items || []).forEach((it, i) => {
      if (filterMatch(it, needle)) filtered.push({ i, it });
    });
    countLabel = String(filtered.length);
    rowsHtml = filtered.map(({ i, it }) => {
      const on = (i === sel);
      return `<button type="button" data-select="${selKey}" data-index="${i}" class="${on ? 'sel' : ''}">${esc(it.label || it)}</button>`;
    }).join('');
  }

  return `<div class="listbox" data-list="${selKey}">
    <div class="listbox-h">
      <span>${title}</span><span class="muted">${esc(countLabel)}</span>
      <input type="search" data-filter="${filterKey}" value="${esc(needle)}" placeholder="Filtrar…">
    </div>
    <div class="listbox-b scroll-y">${rowsHtml || '<div class="empty">(vacío)</div>'}</div>
  </div>`;
}

/** Pick → fill Instancia filter with id + show matching rows (secondary search, no scroll). */
window.omsiFocusInstanceSearch = function (filterText, html, range, selId) {
  if (typeof filterText === 'string') state.instanceFilter = filterText;
  if (typeof selId === 'number') state.instanceId = selId;
  const host = document.querySelector('.listbox[data-list="instanceSel"]');
  if (!host) return false;
  const inp = host.querySelector('input[data-filter="instanceFilter"]');
  if (inp) inp.value = filterText || '';
  const badge = host.querySelector('.listbox-h .muted');
  if (badge && range) badge.textContent = range;
  const box = host.querySelector('.listbox-b');
  if (box) {
    box.innerHTML = html || '<div class="empty">(vacío)</div>';
    box.scrollTop = 0;
  }
  return true;
};

window.omsiReplaceInstances = window.omsiFocusInstanceSearch; // alias

window.omsiFlashInstanceSel = function (id, idx) {
  state.instanceId = (typeof id === 'number') ? id : 0;
  state.instanceSel = (typeof idx === 'number') ? idx : -1;
  const box = document.querySelector('.listbox[data-list="instanceSel"] .listbox-b');
  if (!box) return;
  box.querySelectorAll('button[data-index]').forEach((btn) => {
    const bid = parseInt(btn.getAttribute('data-id') || '0', 10);
    const i = parseInt(btn.dataset.index, 10);
    const hit = state.instanceId ? (bid === state.instanceId) : (i === state.instanceSel);
    btn.classList.toggle('sel', hit);
  });
};

function renderTabs() {
  const strip = document.getElementById('tab-strip');
  let html = '';
  let lastGroup = '';
  TOOLS.forEach(t => {
    if (t.group !== lastGroup) {
      html += `<div class="tab-group">${t.group}</div>`;
      lastGroup = t.group;
    }
    html += `<button type="button" class="tab${state.tool === t.id ? ' active' : ''}" data-tool="${t.id}" title="${t.label}">${t.short}</button>`;
  });
  strip.innerHTML = html;
}

function renderCatalogActions() {
  const spline = state.tool === 2;
  const cells = spline
    ? [
        ['Cargar archivo .sli', 'load_catalog_file'],
        ['Escanear carpeta', 'load_catalog_directory'],
        ['Abrir carpeta', 'open_catalog_folder'],
        ['Ver en .map', 'open_instance_map'],
        [spline ? 'Editar archivo .sli' : 'Editar archivo .sco', 'edit_catalog_file'],
        ['Recargar catálogo', 'reload_catalog_file'],
      ]
    : [
        ['Cargar archivo .sco', 'load_catalog_file'],
        ['Escanear carpeta', 'load_catalog_directory'],
        ['Abrir carpeta', 'open_catalog_folder'],
        ['Ver en .map', 'open_instance_map'],
        ['Editar archivo', 'edit_catalog_file'],
        ['Recargar catálogo', 'reload_catalog_file'],
      ];
  let html = `<div class="cat-grid">${cells.map(([lab, act]) =>
    `<button type="button" class="cat-cell" data-action="${act}" title="${lab}">${lab}</button>`
  ).join('')}`;
  if (spline) {
    html += `<button type="button" class="btn ghost cat-spline" data-action="open_spline_creator">Abrir creador de splines</button>`;
  }
  return html + '</div>';
}

function axisRow(title, triples, step, unit) {
  const cells = triples.map(([lab, id, val]) => {
    const st = getStep(id, step);
    return `<div class="axis-cell">
      <span>${lab}</span>
      <input type="number" class="scrub-input" data-field="${id}" step="${st}" value="${val ?? 0}" title="Arrastrar · step ${st}">
      <span class="spin-col"><button type="button" data-spin="${id}" data-dir="1" tabindex="-1">▲</button><button type="button" data-spin="${id}" data-dir="-1" tabindex="-1">▼</button></span>
    </div>`;
  }).join('');
  return `<div class="axis-block"><div class="axis-h">${title}${unit ? ` <span class="unit">${unit}</span>` : ''}</div><div class="axis-row">${cells}</div></div>`;
}

function actionRow(label, fieldId, value, action, step) {
  const st = getStep(fieldId, step || '1');
  return `<div class="act-row">
    <span class="act-lab">${label}</span>
    <input type="number" class="scrub-input" data-field="${fieldId}" step="${st}" value="${value ?? 0}">
    <span class="spin-col"><button type="button" data-spin="${fieldId}" data-dir="1" tabindex="-1">▲</button><button type="button" data-spin="${fieldId}" data-dir="-1" tabindex="-1">▼</button></span>
    <button type="button" class="act-go" data-action="${action}" title="${label}">→</button>
  </div>`;
}

function renderObjectSpawnCard() {
  const s = state.spawn;
  const chk = (label, key) =>
    `<label class="chk"><input type="checkbox" data-spawn="${key}" ${s[key] ? 'checked' : ''}> ${label}</label>`;
  const posStep = getStep('object.x', 0.05);
  return `<div class="spawn-float" id="spawn-float">
    ${chk('Pegar a terreno', 'stickTerrain')}
    ${chk('Generar en meshes', 'spawnMeshes')}
    ${chk('Rieles de path', 'showPathRails')}
    <div class="step-h">Step: X</div>
    <div class="step-val">${posStep}</div>
    <div class="step-presets">
      ${[0.01, 0.1, 1, 5].map((n) =>
        `<button type="button" class="step-pre${Number(posStep) === n ? ' on' : ''}" data-step-preset="${n}">${n}</button>`
      ).join('')}
    </div>
  </div>`;
}

function renderSpawn(opts) {
  const extrasOnly = !!(opts && opts.extrasOnly);
  const s = state.spawn;
  const chk = (label, key) =>
    `<label class="chk"><input type="checkbox" data-spawn="${key}" ${s[key] ? 'checked' : ''}> ${label}</label>`;
  const extras = `
      ${chk('Flechas spline', 'showSplineEnds')}
      ${chk('Flechas path', 'showPathEnds')}
      ${chk('Completar extremos', 'autofillEnds')}
      ${chk('Alinear pitch/bank', 'alignPitchBank')}`;
  const core = extrasOnly ? '' : `
      ${chk('Rieles de path', 'showPathRails')}
      ${chk('Pegar a terreno', 'stickTerrain')}
      ${chk('Generar en meshes', 'spawnMeshes')}`;
  const cars = Array.isArray(state.busCars) ? state.busCars : [];
  const last = state.lastOmsiBus || (cars[0] && cars[0].path) || '';
  const carOpts = cars.map((c) => `<option value="${esc(c.path)}" ${c.path === last ? 'selected' : ''}>${esc(c.label)}</option>`).join('');
  const vc = `
    <div class="row" style="margin-top:8px;gap:6px;flex-wrap:wrap">
      <input id="vc-car-filter" placeholder="Filtrar vehículo…" style="min-width:8rem" oninput="window.omsiFilterVcCars&&window.omsiFilterVcCars()">
      <select id="vc-car-sel">${carOpts}</select>
      ${btn('Generar OMSI bus', 'spawn_omsi_bus', 'ghost')}
    </div>`;
  const objectTool = state.tool === 1;
  const mode = typeof s.spawnRandomMode === 'number' ? s.spawnRandomMode : 0;
  const favCats = Array.isArray(state.favCats) ? state.favCats : [];
  const favOpts = favCats.map((c, i) =>
    `<option value="${i}" ${i === (s.spawnRandomFavCat || 0) ? 'selected' : ''}>${esc(c.name || ('Lista ' + (i + 1)))} (${c.count || 0})</option>`
  ).join('');
  const folderHint = s.spawnRandomFolder
    ? `${esc(s.spawnRandomFolder)} · ${s.spawnRandomFolderCount || 0} SCO`
    : 'Sin carpeta escaneada';
  const randomUi = objectTool ? `
    <div style="margin-top:8px;display:flex;flex-direction:column;gap:6px">
      ${chk('Generar al azar', 'spawnRandom')}
      ${s.spawnRandom ? `
        <label class="muted">Fuente al pulsar N</label>
        <select data-spawn-random-mode class="field-select">
          <option value="0" ${mode === 0 ? 'selected' : ''}>Al azar del catálogo</option>
          <option value="1" ${mode === 1 ? 'selected' : ''}>Lista de favoritos</option>
          <option value="2" ${mode === 2 ? 'selected' : ''}>Escanear carpeta</option>
        </select>
        ${mode === 1 ? `
          <label class="muted">Lista de favoritos</label>
          <select data-spawn-random-fav class="field-select">
            ${favOpts || '<option value="0">(sin listas)</option>'}
          </select>` : ''}
        ${mode === 2 ? `
          <div class="muted">${folderHint}</div>
          ${btn('Escanear carpeta', 'spawn_random_scan_folder', 'ghost')}` : ''}
      ` : ''}
    </div>` : '';
  return section('options', 'Vista y spawn', `
    <div>${extras}${core}</div>
    ${randomUi}
    ${vc}
    <div class="row">${btn('Guardar preferencias', 'save_ui_settings', 'ghost')}</div>
  `);
}

function renderSelectionCard() {
  const id = state.instanceId || 0;
  if (!id) return '';
  const obj = state.object || {};
  const sp = state.spline || {};
  const name = (state.tool === 2)
    ? (sp.name || sp.sli_name || '')
    : (obj.name || obj.sco_name || '');
  const focus = state.instanceFocus || (`#${id} ${name}`);
  return `<div class="sel-card" id="instance-sel-card">
    <div class="k">Instancia (siempre visible)</div>
    <div class="v">${esc(focus)}</div>
  </div>`;
}

function renderCatalogCard() {
  const focus = state.catalogFocus || '';
  if (!focus && !(state.catalogSel >= 0)) return '';
  const label = focus || ((state.catalog && state.catalog[state.catalogSel] && state.catalog[state.catalogSel].label) || '');
  if (!label) return '';
  return `<div class="sel-card" id="catalog-sel-card">
    <div class="k">Catálogo / Ítem (siempre visible)</div>
    <div class="v">${esc(label)}</div>
  </div>`;
}

function renderSplineTool() {
  const sp = state.spline || {};
  const browser = `${renderCatalogActions()}${renderCatalogCard()}${renderSelectionCard()}<div class="browser">
      ${listBlock('Ítem', state.catalog, 'catalogSel', 'catalogFilter')}
      ${listBlock('Instancia', state.instances, 'instanceSel', 'instanceFilter')}
    </div>`;

  if (!sp.valid) {
    return browser + `<div class="row" style="margin:8px 0">${btn('Seleccionar archivo SLI…', 'sli_bake_pick', 'ghost')}</div>` +
      `<div class="empty">Ninguna instancia seleccionada</div>` + renderSpawn();
  }

  const header = `<div class="hdr"><span class="pill">#${sp.id}</span><span class="name">${esc(sp.name || '')}</span></div>`;
  const transform = section('transform', 'Transformación',
    field('Z', 'spline.z', sp.z, '0.05', 'm') +
    field('Rot', 'spline.rot', sp.rot, '0.2', '°') +
    field('Skew inicio', 'spline.skew_start', sp.skew_start, '0.05', '') +
    field('Skew fin', 'spline.skew_end', sp.skew_end, '0.05', ''));
  const geometry = section('geometry', 'Geometría',
    field('Largo', 'spline.length', sp.length, '0.1', 'm') +
    field('Radio', 'spline.radius', sp.radius, '0.1', 'm') +
    field('Grad inicio %', 'spline.start_grad', sp.start_grad, '0.05', '%') +
    field('Grad fin %', 'spline.end_grad', sp.end_grad, '0.05', '%') +
    `<label class="chk"><input type="checkbox" data-check="spline.use_delta_h" ${sp.use_delta_h ? 'checked' : ''}> Delta H</label>` +
    (sp.use_delta_h ? field('Delta H', 'spline.delta_h', sp.delta_h, '0.05', 'm') : '') +
    field('Cant inicio', 'spline.cant_start', sp.cant_start, '0.01', '') +
    field('Cant fin', 'spline.cant_end', sp.cant_end, '0.01', '') +
    `<label class="chk"><input type="checkbox" data-check="spline.mirror" ${sp.mirror ? 'checked' : ''}> Reflejar</label>` +
    field('R max completar', 'spline.max_radius', sp.max_radius ?? 200, '1', 'm'));
  const actions = section('actions', 'Acciones',
    `<div class="row">
      ${btn('Aplicar spline', 'apply_spline', 'primary')}
      ${btn('Reflejar grupo', 'apply_spline_mirror')}
      ${btn('Completar cadena', 'spline_complete')}
      ${btn('Dividir spline', 'spline_split')}
      ${btn('Alinear inicio', 'spline_align_start')}
      ${btn('Alinear fin', 'spline_align_end')}
      ${btn('Seleccionar archivo SLI…', 'sli_bake_pick', 'ghost')}
    </div>`);
  return browser + header + transform + geometry + actions + renderSpawn();
}

function renderObjectTool() {
  const obj = state.object || {};
  const sco = (obj.name || obj.sco_name || state.catalogFocus || '—').replace(/^.*[\\/]/, '');
  const itemCard = `<div class="item-card">
    <div class="item-k">item</div>
    <div class="item-v" title="${esc(obj.name || '')}">${esc(sco || '—')}</div>
    <div class="item-id">${obj.valid ? '#' + obj.id : ''}</div>
  </div>`;
  let body = renderCatalogActions() + itemCard +
    section('instances', 'desplegar instancias',
      listBlock('Ítem', state.catalog, 'catalogSel', 'catalogFilter') +
      listBlock('Instancia', state.instances, 'instanceSel', 'instanceFilter'));
  if (!obj.valid) {
    body += `<div class="empty">Sin objeto seleccionado — clic en el 3D o en la lista</div>`;
  } else {
    body += axisRow('Posición', [
      ['X', 'object.x', obj.x],
      ['Y', 'object.y', obj.y],
      ['Z', 'object.z', obj.z],
    ], '0.05', 'm');
    body += axisRow('Rotación', [
      ['X', 'object.pitch', obj.pitch],
      ['Y', 'object.rot', obj.rot],
      ['Z', 'object.bank', obj.bank],
    ], '0.2', '°');
    body += `<div class="act-stack">
      ${actionRow('Vincular a…', 'object.parent', obj.parent, 'parent_to_object', '1')}
      ${actionRow('Adjuntar objeto', 'object.attach', obj.attach, 'attach_to_object', '1')}
      ${actionRow('Adjuntar spline', 'object.attach', obj.attach, 'attach_to_spline', '1')}
      <div class="row" style="margin-top:6px">${btn('Aplicar objeto', 'apply_object', 'primary')}</div>
    </div>`;
  }
  return `<div class="obj-layout">${body}${renderSpawn({ extrasOnly: true })}</div>${renderObjectSpawnCard()}`;
}

/** OMSI Terrain tab: height brush + Paint/Rubber on [groundtex] layers. */
function renderTerrainTool() {
  const t = state.terrain || {};
  const mode = typeof t.mode === 'number' ? t.mode : 0;
  const layerSel = typeof t.layerSel === 'number' ? t.layerSel : 0;
  const layers = Array.isArray(t.layers) ? t.layers : [];
  const modesHtml = TERRAIN_MODES.map((m) =>
    `<button type="button" class="mode-btn${mode === m.id ? ' active' : ''}" data-terrain-mode="${m.id}">${m.label}</button>`
  ).join('');
  const layerRows = layers.map((lab, i) =>
    `<button type="button" data-select="groundLayer" data-index="${i}" class="${i === layerSel ? 'sel' : ''}">${esc(lab)}</button>`
  ).join('');
  const brush = section('terrBrush', 'Pincel',
    `<div class="mode-row">${modesHtml}</div>` +
    field('Tamaño', 'terrain.brush_size', t.brushSize ?? 20, '0.5', 'm') +
    field('Velocidad', 'terrain.speed', t.speed ?? 3, '0.1', 'm/s') +
    field('Altura fija', 'terrain.set_height', t.setHeight ?? 0, '0.05', 'm') +
    `<div class="muted" style="margin-top:6px">LMB arrastrar en el terreno · Paint/Rubber escriben &lt;tile&gt;.layers + bake BMP local</div>`);
  const layerBox = section('terrLayers', 'Capas de suelo ([groundtex])',
    `<div class="listbox" data-list="groundLayer">
      <div class="listbox-h"><span>Capa</span><span class="muted">${layers.length}</span></div>
      <div class="listbox-b scroll-y" style="max-height:160px">${layerRows || '<div class="empty">(sin capas — Crear capa)</div>'}</div>
    </div>
    <div class="row" style="margin-top:6px">
      ${btn('Crear capa', 'ground_create_layer', 'ghost')}
      ${btn('Editar capa', 'ground_edit_layer')}
      ${btn('Borrar capa', 'ground_delete_layer')}
    </div>`);
  const help = section('terrHelp', 'global.cfg',
    `<div class="muted">
      <b>Crear capa</b> añade un bloque <code>[groundtex]</code> al <code>global.cfg</code> del workspace (<code>*_actual</code>, no el original).<br>
      Formato OMSI: diffuse, detail (opcional), flags, escala (p.ej. 60).<br>
      <b>Pintar / Borrar capa</b> no tocan global.cfg: pintan el índice en <code>&lt;tile.map&gt;.layers</code> y generan <code>.ground_paint.bmp</code> por tile.<br>
      Capa 0 = base (pasto). El orden de capas es el de [groundtex] en el cfg.
    </div>`);
  return brush + layerBox + help + renderSpawn();
}

function renderToolContent() {
  switch (state.tool) {
    case 1: return renderObjectTool();
    case 2: return renderSplineTool();
    case 3: return renderTerrainTool();
    default:
      return `<div class="empty"><b>${TOOLS[state.tool]?.label || 'Herramienta'}</b><br>Pestaña en desarrollo — usá Archivo / Herramientas.</div>` + renderSpawn();
  }
}

function renderAll() {
  renderTabs();
  document.getElementById('tool-content').innerHTML = renderToolContent();
  document.getElementById('map-name').textContent = state.mapName || '(sin mapa)';
  const st = state.stats;
  document.getElementById('stats-pill').textContent = `T ${st.tiles} · O ${st.objects} · S ${st.splines}`;
  document.getElementById('status-bar').textContent =
    [state.selection, state.status].filter(Boolean).join('  ·  ') || 'Listo — F5 recarga UI sin recompilar';
}

function applyState(next) {
  if (!next) return;
  const keepFilter = { openSections: state.openSections };
  // Host drives instanceFilter + catalogFilter (secondary search). Don't keep stale local filters.
  state = { ...state, ...next, ...keepFilter };
  if (typeof next.instanceFilter === 'string') state.instanceFilter = next.instanceFilter;
  if (typeof next.catalogFilter === 'string') state.catalogFilter = next.catalogFilter;
  if (next.spawn) state.spawn = { ...state.spawn, ...next.spawn };
  if (Array.isArray(next.favCats)) state.favCats = next.favCats;
  if (next.spline) state.spline = { ...state.spline, ...next.spline };
  if (next.object) state.object = { ...state.object, ...next.object };
  if (next.terrain) state.terrain = { ...(state.terrain || {}), ...next.terrain };
  if (next.fieldSteps) mergeFieldSteps(next.fieldSteps);
  if (scrub) {
    document.getElementById('map-name').textContent = state.mapName || '(sin mapa)';
    const st = state.stats;
    document.getElementById('stats-pill').textContent = `T ${st.tiles} · O ${st.objects} · S ${st.splines}`;
    document.getElementById('status-bar').textContent =
      [state.selection, state.status].filter(Boolean).join('  ·  ') || 'Listo — F5 recarga UI sin recompilar';
    return;
  }
  renderAll();
  if (typeof window.omsiFlashInstanceSel === 'function') {
    window.omsiFlashInstanceSel(state.instanceId || 0, state.instanceSel);
  }
}

/** ImGui-style horizontal drag scrub on number fields (keeps typing + ▲/▼). */
let scrub = null;

document.addEventListener('pointerdown', (ev) => {
  if (ev.button !== 0) return;
  const inp = ev.target.closest('input.scrub-input[data-field]');
  if (!inp) return;
  // Allow normal text edit when already focused (click inside to select/caret).
  if (document.activeElement === inp) return;
  scrub = {
    id: inp.dataset.field,
    startX: ev.clientX,
    startVal: parseFloat(inp.value) || 0,
    step: getStep(inp.dataset.field, inp.step || 0.1),
    inp,
    moved: false,
    pointerId: ev.pointerId,
  };
  try { inp.setPointerCapture(ev.pointerId); } catch (_) {}
});

document.addEventListener('pointermove', (ev) => {
  if (!scrub || scrub.pointerId !== ev.pointerId) return;
  const dx = ev.clientX - scrub.startX;
  if (!scrub.moved && Math.abs(dx) < 3) return;
  scrub.moved = true;
  ev.preventDefault();
  scrub.inp.classList.add('scrubbing');
  const nv = scrub.startVal + dx * scrub.step;
  scrub.inp.value = String(nv);
  postFieldLive(scrub.id, nv);
});

function endScrub(ev) {
  if (!scrub || (ev && scrub.pointerId !== ev.pointerId)) return;
  const s = scrub;
  scrub = null;
  s.inp.classList.remove('scrubbing');
  if (!s.moved) {
    // Click without drag → focus for typing.
    try { s.inp.focus(); s.inp.select(); } catch (_) {}
  }
}

document.addEventListener('pointerup', endScrub);
document.addEventListener('pointercancel', endScrub);

document.addEventListener('click', (ev) => {
  const preset = ev.target.closest('[data-step-preset]');
  if (preset) {
    const n = Number(preset.dataset.stepPreset);
    ['object.x', 'object.y', 'object.z'].forEach((id) => setStep(id, n, true));
    renderAll();
    return;
  }
  const gear = ev.target.closest('[data-step-gear]');
  if (gear) {
    const id = gear.dataset.stepGear;
    const cur = getStep(id, 0.1);
    const next = window.prompt('Step para ' + id, String(cur));
    if (next != null && next !== '') {
      setStep(id, next, true);
      const inp = document.querySelector(`[data-field="${id}"]`);
      if (inp) inp.step = String(getStep(id, cur));
      gear.title = 'Cambiar step (ahora ' + getStep(id, cur) + ')';
    }
    return;
  }
  const spin = ev.target.closest('[data-spin]');
  if (spin) {
    const id = spin.dataset.spin;
    const dir = parseFloat(spin.dataset.dir || '1');
    const inp = document.querySelector(`[data-field="${id}"]`);
    if (inp) {
      const step = getStep(id, inp.step || 0.1);
      const v = (parseFloat(inp.value) || 0) + dir * step;
      inp.value = String(v);
      postFieldLive(id, v);
    }
    return;
  }
  const tm = ev.target.closest('[data-terrain-mode]');
  if (tm) {
    const id = parseInt(tm.dataset.terrainMode, 10);
    if (!state.terrain) state.terrain = {};
    state.terrain.mode = id;
    postFieldLive('terrain.mode', id);
    document.querySelectorAll('[data-terrain-mode]').forEach((b) => {
      b.classList.toggle('active', parseInt(b.dataset.terrainMode, 10) === id);
    });
    return;
  }
  const tog = ev.target.closest('[data-toggle-section]');
  if (tog) {
    const id = tog.dataset.toggleSection;
    state.openSections[id] = !(state.openSections[id] !== false);
    renderAll();
    return;
  }
  const t = ev.target.closest('[data-tool]');
  if (t) {
    const id = parseInt(t.dataset.tool, 10);
    if (state.tool !== id) {
      state.tool = id;
      renderTabs();
      document.getElementById('tool-content').innerHTML = renderToolContent();
    }
    post('tool', { id });
    return;
  }
  const act = ev.target.closest('[data-action]');
  if (act) {
    if (act.dataset.action === 'spawn_omsi_bus') {
      const sel = document.getElementById('vc-car-sel');
      const dff = sel && sel.value ? sel.value : (state.lastOmsiBus || '');
      const cars = Array.isArray(state.busCars) ? state.busCars : [];
      const hit = cars.find((c) => c.path === dff) || cars[0];
      if (hit) post('action', { name: 'spawn_omsi_bus', path: hit.path });
      return;
    }
    post('action', { name: act.dataset.action });
    return;
  }
  const sel = ev.target.closest('[data-select]');
  if (sel) {
    if (sel.dataset.select === 'groundLayer') {
      const idx = parseInt(sel.dataset.index, 10);
      if (!state.terrain) state.terrain = {};
      state.terrain.layerSel = idx;
      const box = document.querySelector('.listbox[data-list="groundLayer"] .listbox-b');
      if (box) {
        box.querySelectorAll('button[data-index]').forEach((b) => {
          b.classList.toggle('sel', parseInt(b.dataset.index, 10) === idx);
        });
      }
    }
    post('select', { kind: sel.dataset.select, index: parseInt(sel.dataset.index, 10) });
    return;
  }
  const menu = ev.target.closest('[data-menu]');
  if (menu) { post('menu', { name: menu.dataset.menu }); return; }
});

document.addEventListener('change', (ev) => {
  const chk = ev.target.closest('[data-check]');
  if (chk) { post('check', { field: chk.dataset.check, value: chk.checked }); return; }
  const sp = ev.target.closest('[data-spawn]');
  if (sp) {
    const key = sp.dataset.spawn;
    const on = !!sp.checked;
    if (state.spawn) state.spawn[key] = on;
    post('spawn', { key, value: on });
    if (key === 'spawnRandom') {
      document.getElementById('tool-content').innerHTML = renderToolContent();
    }
    return;
  }
  const modeSel = ev.target.closest('[data-spawn-random-mode]');
  if (modeSel) {
    const mode = parseInt(modeSel.value, 10) || 0;
    if (state.spawn) {
      state.spawn.spawnRandom = true;
      state.spawn.spawnRandomMode = mode;
    }
    post('spawn_random', { mode, favCat: (state.spawn && state.spawn.spawnRandomFavCat) || 0 });
    document.getElementById('tool-content').innerHTML = renderToolContent();
    return;
  }
  const favSel = ev.target.closest('[data-spawn-random-fav]');
  if (favSel) {
    const favCat = parseInt(favSel.value, 10) || 0;
    if (state.spawn) state.spawn.spawnRandomFavCat = favCat;
    post('spawn_random', { mode: 1, favCat });
    return;
  }
  const fld = ev.target.closest('[data-field]');
  if (fld) { postFieldLive(fld.dataset.field, parseFloat(fld.value)); return; }
});

document.addEventListener('input', (ev) => {
  const f = ev.target.closest('[data-filter]');
  if (f) {
    state[f.dataset.filter] = f.value;
    if (f.dataset.filter === 'instanceFilter') {
      post('instance_filter', { text: f.value });
      return;
    }
    if (f.dataset.filter === 'catalogFilter') {
      post('catalog_filter', { text: f.value });
      return;
    }
    document.getElementById('tool-content').innerHTML = renderToolContent();
    return;
  }
  const fld = ev.target.closest('[data-field]');
  if (fld && !scrub) postFieldLive(fld.dataset.field, parseFloat(fld.value));
});

window.omsiScroll = function (dyPx, x, y) {
  const px = Number(dyPx) || 0;
  if (!px) return false;
  const cx = Math.max(0, Number(x) || 0);
  const cy = Math.max(0, Number(y) || 0);
  let el = document.elementFromPoint(cx, cy);
  while (el && el !== document.documentElement) {
    if (el.classList && el.classList.contains('listbox-b')) {
      const list = el.closest('.listbox[data-list="instanceSel"]');
      if (list) {
        const step = Math.max(1, Math.round(Math.abs(px) / 18));
        const dir = px > 0 ? -1 : 1;
        post('instance_win', { delta: dir * step });
        return true;
      }
    }
    el = el.parentElement;
  }
  el = document.elementFromPoint(cx, cy);
  while (el && el !== document.documentElement) {
    if (el.scrollHeight > el.clientHeight + 1) {
      const st = window.getComputedStyle(el);
      const oy = st.overflowY;
      if (oy === 'auto' || oy === 'scroll' || oy === 'overlay' || st.overflow === 'auto' || st.overflow === 'scroll') {
        el.scrollTop += px;
        return true;
      }
    }
    el = el.parentElement;
  }
  const nodes = document.querySelectorAll('.scroll-y, #tool-content');
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (n.scrollHeight > n.clientHeight + 1) {
      n.scrollTop += px;
      return true;
    }
  }
  const main = document.getElementById('tool-content');
  if (main) { main.scrollTop += px; return true; }
  return false;
};

window.omsiFilterVcCars = function () {
  const q = ((document.getElementById('vc-car-filter') || {}).value || '').toLowerCase();
  const sel = document.getElementById('vc-car-sel');
  if (!sel) return;
  for (let i = 0; i < sel.options.length; i++) {
    const o = sel.options[i];
    o.hidden = !!(q && o.value.toLowerCase().indexOf(q) < 0);
  }
};

window.omsiApplyState = applyState;
(function boot() {
  renderAll();
  var b = document.getElementById('btn-reload-ui');
  if (b) b.addEventListener('click', function () { post('action', { name: 'reload_ui' }); });
})();
