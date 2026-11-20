'use strict';

var state = { maps: [], entries: [], mapSel: 0, entrySel: 0, entryOnly: false, mapTitle: '' };

function post(type, data) {
  if (typeof omsiPost !== 'function') return;
  var payload = { type: type };
  if (data) {
    for (var k in data) {
      if (Object.prototype.hasOwnProperty.call(data, k)) payload[k] = data[k];
    }
  }
  omsiPost(JSON.stringify(payload));
}

function esc(s) {
  if (s == null) s = '';
  s = String(s);
  return s.replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

function renderMaps() {
  var box = document.getElementById('map-list');
  var cnt = document.getElementById('map-count');
  if (!box) return;
  if (cnt) cnt.textContent = String(state.maps.length);
  if (!state.maps.length) {
    box.innerHTML = '<div class="text-slate-500 p-3 text-sm">(sin mapas)</div>';
    return;
  }
  var html = '';
  for (var i = 0; i < state.maps.length; i++) {
    var m = state.maps[i];
    html += '<button type="button" class="list-item' + (m.index === state.mapSel ? ' sel' : '') +
      '" data-map="' + m.index + '">' + esc(m.label) + '</button>';
  }
  box.innerHTML = html;
}

function renderEntries() {
  var box = document.getElementById('entry-list');
  var cnt = document.getElementById('ep-count');
  if (!box) return;
  if (cnt) cnt.textContent = String(state.entries.length);
  if (!state.entries.length) {
    box.innerHTML = '<div class="text-slate-500 p-3 text-sm">(sin entrypoints)</div>';
    return;
  }
  var html = '';
  for (var j = 0; j < state.entries.length; j++) {
    var e = state.entries[j];
    html += '<button type="button" class="list-item' + (e.index === state.entrySel ? ' sel' : '') +
      '" data-ep="' + e.index + '">' + esc(e.label) + '</button>';
  }
  box.innerHTML = html;
}

function mergeState(base, next) {
  var out = {};
  var key;
  for (key in base) out[key] = base[key];
  for (key in next) out[key] = next[key];
  return out;
}

function applyState(next) {
  if (!next) return;
  state = mergeState(state, next);
  if (state.entryOnly) {
    var mb = document.getElementById('map-block');
    if (mb) mb.className = (mb.className + ' hidden').replace(/^\s+/, '');
    var sub = document.getElementById('subtitle');
    if (sub) sub.textContent = state.mapTitle || 'Elegir entrypoint';
    var ok = document.getElementById('btn-ok');
    if (ok) ok.textContent = 'Ir';
  }
  renderMaps();
  renderEntries();
}

function findAttr(el, attr) {
  while (el && el !== document) {
    if (el.getAttribute && el.getAttribute(attr) != null) return el;
    el = el.parentNode;
  }
  return null;
}

function findById(el, id) {
  while (el && el !== document) {
    if (el.id === id) return el;
    el = el.parentNode;
  }
  return null;
}

document.addEventListener('click', function (ev) {
  var mapBtn = findAttr(ev.target, 'data-map');
  if (mapBtn) {
    var midx = parseInt(mapBtn.getAttribute('data-map'), 10);
    if (!isFinite(midx)) return;
    state.mapSel = midx;
    renderMaps();
    post('select', { kind: 'map', index: midx });
    return;
  }
  var epBtn = findAttr(ev.target, 'data-ep');
  if (epBtn) {
    var eidx = parseInt(epBtn.getAttribute('data-ep'), 10);
    state.entrySel = eidx;
    renderEntries();
    post('select', { kind: 'entry', index: eidx });
    return;
  }
  if (findById(ev.target, 'btn-ok')) {
    post('action', { name: 'ok' });
    return;
  }
  if (findById(ev.target, 'btn-cancel')) {
    post('action', { name: 'cancel' });
  }
});

document.addEventListener('dblclick', function (ev) {
  if (findAttr(ev.target, 'data-ep')) post('action', { name: 'ok' });
});

document.addEventListener('keydown', function (ev) {
  if (ev.key === 'Enter') { ev.preventDefault(); post('action', { name: 'ok' }); }
  if (ev.key === 'Escape') { ev.preventDefault(); post('action', { name: 'cancel' }); }
});

window.omsiApplyState = applyState;
(function bootFromNative() {
  console.log('omsi boot: typeof omsiGetState=' + typeof omsiGetState);
  if (typeof omsiGetState !== 'function') return;
  var raw = omsiGetState();
  console.log('omsi boot: raw len=' + (raw ? raw.length : -1));
  if (!raw) return;
  try {
    var parsed = JSON.parse(raw);
    console.log('omsi boot: parsed maps=' + (parsed.maps ? parsed.maps.length : -1));
    applyState(parsed);
  } catch (e) {
    console.log('omsi boot: JSON parse error ' + e.message);
  }
})();
