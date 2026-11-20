(function () {
  'use strict';

  var state = {
    profiles: [],
    textures: [],
    paths: [],
    selProfile: 0,
    selTex: 0,
    selPath: 0,
    exportCopy: true
  };

  function merge(dst, src) {
    if (!src) return dst;
    for (var k in src) {
      if (Object.prototype.hasOwnProperty.call(src, k)) dst[k] = src[k];
    }
    return dst;
  }

  function post(type, data) {
    if (typeof omsiPost !== 'function') return;
    var payload = { type: type };
    merge(payload, data);
    omsiPost(JSON.stringify(payload));
  }

  function esc(s) {
    s = s == null ? '' : String(s);
    return s.replace(/[&<>"']/g, function (c) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c];
    });
  }

  function closest(el, sel) {
    while (el && el !== document.body) {
      if (el.matches && el.matches(sel)) return el;
      el = el.parentNode;
    }
    return null;
  }

  function renderList(id, items, sel) {
    var el = document.getElementById(id);
    if (!el) return;
    if (!items || !items.length) {
      el.innerHTML = '<div class="text-slate-500 p-2 text-xs">(vacío)</div>';
      return;
    }
    var html = '';
    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      var cls = 'list-item' + (it.index === sel ? ' sel' : '');
      var inner = esc(it.label);
      if (it.thumb) {
        cls += ' tex-item';
        inner = '<img src="' + esc(it.thumb) + '" alt="">' + '<span>' + esc(it.label) + '</span>';
      }
      html += '<button type="button" class="' + cls + '" data-list="' + id + '" data-idx="' + it.index + '">' +
        inner + '</button>';
    }
    el.innerHTML = html;
  }

  function applyState(next) {
    if (!next) return;
    merge(state, next);
    renderList('profile-list', state.profiles, state.selProfile);
    renderList('tex-list', state.textures, state.selTex);
    renderList('path-list', state.paths, state.selPath);
    var chk = document.getElementById('export-copy');
    if (chk) chk.checked = !!state.exportCopy;
  }

  document.body.addEventListener('click', function (ev) {
    var cmd = closest(ev.target, '[data-cmd]');
    if (cmd) {
      post('cmd', { name: cmd.getAttribute('data-cmd') });
      return;
    }
    var item = closest(ev.target, '[data-list]');
    if (item) {
      var list = item.getAttribute('data-list');
      var idx = parseInt(item.getAttribute('data-idx'), 10);
      if (list === 'profile-list') {
        state.selProfile = idx;
        post('select', { kind: 'profile', index: idx });
      } else if (list === 'tex-list') {
        state.selTex = idx;
        post('select', { kind: 'texture', index: idx });
      } else if (list === 'path-list') {
        state.selPath = idx;
        post('select', { kind: 'path', index: idx });
      }
      applyState(state);
    }
  });

  var exportChk = document.getElementById('export-copy');
  if (exportChk) {
    exportChk.addEventListener('change', function (ev) {
      post('check', { field: 'exportCopy', value: ev.target.checked });
    });
  }

  document.body.addEventListener('dblclick', function (ev) {
    if (closest(ev.target, '[data-list="profile-list"]')) post('cmd', { name: 'wrapOpen' });
    if (closest(ev.target, '[data-list="path-list"]')) post('cmd', { name: 'pathConfig' });
  });

  window.omsiApplyState = applyState;
  window.omsiScroll = function (dy, x, y) {
    var el = document.elementFromPoint(x, y);
    while (el) {
      if (el.classList && el.classList.contains('scroll-y')) {
        el.scrollTop += dy;
        return;
      }
      el = el.parentNode;
    }
    var lists = document.querySelectorAll('.scroll-y');
    for (var i = 0; i < lists.length; i++) {
      if (lists[i].scrollHeight > lists[i].clientHeight) {
        lists[i].scrollTop += dy;
        return;
      }
    }
  };
  post('ready', {});
})();
