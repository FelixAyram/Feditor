(function () {
  'use strict';

  var state = {
    snap: 4,
    pencil: false,
    marquee: false,
    drawTex: true,
    drawPath: true,
    sliText: ''
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

  function setToggle(name, on) {
    var nodes = document.querySelectorAll('[data-cmd="' + name + '"]');
    for (var i = 0; i < nodes.length; i++) {
      if (on) nodes[i].classList.add('active');
      else nodes[i].classList.remove('active');
    }
  }

  function applyState(next) {
    if (!next) return;
    merge(state, next);
    var snap = document.getElementById('snap');
    if (snap) snap.value = String(state.snap != null ? state.snap : 4);
    setToggle('pencil', state.pencil);
    setToggle('marquee', state.marquee);
    setToggle('drawTex', state.drawTex);
    setToggle('drawPath', state.drawPath);
    var ta = document.getElementById('sli-text');
    if (ta && state.sliText != null && document.activeElement !== ta) ta.value = state.sliText;
  }

  var snapEl = document.getElementById('snap');
  if (snapEl) {
    snapEl.addEventListener('change', function (ev) {
      post('snap', { index: parseInt(ev.target.value, 10) });
    });
  }

  function closest(el, sel) {
    while (el && el !== document.body) {
      if (el.matches && el.matches(sel)) return el;
      el = el.parentNode;
    }
    return null;
  }

  document.body.addEventListener('click', function (ev) {
    var cmd = closest(ev.target, '[data-cmd]');
    if (!cmd) return;
    post('cmd', { name: cmd.getAttribute('data-cmd') });
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
    var ta = document.getElementById('sli-text');
    if (ta && ta.scrollHeight > ta.clientHeight) ta.scrollTop += dy;
  };
  post('ready', {});
})();
