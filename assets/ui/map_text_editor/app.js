(function () {
  'use strict';

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

  function closest(el, sel) {
    while (el && el !== document.body) {
      if (el.matches && el.matches(sel)) return el;
      el = el.parentNode;
    }
    return null;
  }

  function lineFromOffset(text, offset) {
    var line = 1;
    var limit = Math.min(offset, text.length);
    for (var i = 0; i < limit; i++) {
      if (text.charAt(i) === '\n') line++;
    }
    return line;
  }

  var editorEl = document.getElementById('editor');
  var lineLbl = document.getElementById('line-indicator');
  var statusEl = document.getElementById('status-msg');
  var fileNameEl = document.getElementById('file-name');
  var findBar = document.getElementById('find-bar');
  var findInput = document.getElementById('find-input');
  var findCount = document.getElementById('find-count');
  var ctxMenu = document.getElementById('ctx-menu');
  var dirtyPosted = false;
  var findFrom = 0;

  function updateLineIndicator() {
    if (!editorEl || !lineLbl) return;
    lineLbl.textContent = 'Linea: ' + lineFromOffset(editorEl.value, editorEl.selectionStart);
  }

  function setStatus(msg, cls) {
    if (!statusEl) return;
    statusEl.textContent = msg || '';
    statusEl.className = 'text-xs status-msg' + (cls ? ' ' + cls : '');
  }

  function selectedText() {
    if (!editorEl) return '';
    return editorEl.value.substring(editorEl.selectionStart, editorEl.selectionEnd);
  }

  function replaceSelection(text) {
    if (!editorEl) return;
    var start = editorEl.selectionStart;
    var end = editorEl.selectionEnd;
    var v = editorEl.value;
    editorEl.value = v.substring(0, start) + text + v.substring(end);
    var caret = start + text.length;
    editorEl.setSelectionRange(caret, caret);
    editorEl.dispatchEvent(new Event('input'));
    updateLineIndicator();
  }

  function nativeClipboard(op) {
    window._omsiClip = selectedText();
    post('clipboard', { op: op });
  }

  function doCopy() {
    var t = selectedText();
    if (!t) return;
    try {
      if (document.execCommand) {
        editorEl.focus();
        document.execCommand('copy');
      }
    } catch (e) {}
    nativeClipboard('copy');
  }

  function doCut() {
    if (!selectedText()) return;
    doCopy();
    replaceSelection('');
  }

  function doPaste() {
    post('clipboard', { op: 'paste' });
  }

  function doSelectAll() {
    if (!editorEl) return;
    editorEl.focus();
    editorEl.setSelectionRange(0, editorEl.value.length);
    updateLineIndicator();
  }

  function showFind(on) {
    if (!findBar) return;
    if (on) {
      findBar.classList.remove('hidden');
      if (findInput) {
        findInput.focus();
        findInput.select();
      }
    } else {
      findBar.classList.add('hidden');
      if (editorEl) editorEl.focus();
    }
  }

  function runFind(back) {
    if (!editorEl || !findInput) return;
    var q = findInput.value;
    if (!q) {
      if (findCount) findCount.textContent = '';
      return;
    }
    var text = editorEl.value;
    var qn = q.toLowerCase();
    var hay = text.toLowerCase();
    var idx;
    if (back) {
      var from = Math.max(0, editorEl.selectionStart - 1);
      idx = hay.lastIndexOf(qn, from);
      if (idx < 0) idx = hay.lastIndexOf(qn);
    } else {
      var start = Math.max(findFrom, editorEl.selectionEnd);
      idx = hay.indexOf(qn, start);
      if (idx < 0) idx = hay.indexOf(qn);
    }
    if (idx < 0) {
      if (findCount) findCount.textContent = '0';
      return;
    }
    editorEl.focus();
    editorEl.setSelectionRange(idx, idx + q.length);
    findFrom = idx + q.length;
    var count = 0, p = 0;
    while ((p = hay.indexOf(qn, p)) >= 0) {
      count++;
      p += q.length;
    }
    var nth = 1;
    p = 0;
    while ((p = hay.indexOf(qn, p)) >= 0 && p < idx) {
      nth++;
      p += q.length;
    }
    if (findCount) findCount.textContent = nth + '/' + count;
    var lh = parseFloat(getComputedStyle(editorEl).lineHeight) || 16;
    var line = lineFromOffset(text, idx);
    editorEl.scrollTop = Math.max(0, (line - 6) * lh);
    updateLineIndicator();
  }

  function hideCtx() {
    if (ctxMenu) ctxMenu.classList.add('hidden');
  }

  function showCtx(x, y) {
    if (!ctxMenu) return;
    ctxMenu.classList.remove('hidden');
    ctxMenu.style.left = x + 'px';
    ctxMenu.style.top = y + 'px';
  }

  function gotoLine(n) {
    if (!editorEl || !n || n < 1) return;
    var text = editorEl.value;
    var idx = 0, line = 1;
    while (line < n) {
      var nl = text.indexOf('\n', idx);
      if (nl < 0) { idx = text.length; break; }
      idx = nl + 1;
      line++;
    }
    var end = text.indexOf('\n', idx);
    if (end < 0) end = text.length;
    editorEl.focus();
    editorEl.setSelectionRange(idx, end);
    var lh = parseFloat(getComputedStyle(editorEl).lineHeight) || 16;
    editorEl.scrollTop = Math.max(0, (n - 6) * lh);
    updateLineIndicator();
  }

  function applyState(next) {
    if (!next) return;
    if (next.fileName != null && fileNameEl) fileNameEl.textContent = next.fileName;
    if (next.text != null && editorEl) {
      editorEl.value = next.text;
      dirtyPosted = false;
      setStatus('', '');
      if (next.gotoLine) gotoLine(next.gotoLine);
      else updateLineIndicator();
    }
    if (next.status != null) setStatus(next.status, next.statusClass);
    if (next.pasteText != null) {
      editorEl.focus();
      replaceSelection(String(next.pasteText));
    }
  }

  if (editorEl) {
    editorEl.addEventListener('input', function () {
      if (!dirtyPosted) {
        dirtyPosted = true;
        post('dirty', { value: true });
      }
      setStatus('Sin guardar', 'dirty');
    });
    editorEl.addEventListener('keyup', updateLineIndicator);
    editorEl.addEventListener('click', updateLineIndicator);
    editorEl.addEventListener('select', updateLineIndicator);
    editorEl.addEventListener('contextmenu', function (ev) {
      ev.preventDefault();
      showCtx(ev.clientX, ev.clientY);
    });
  }

  document.addEventListener('keydown', function (ev) {
    var ctrl = ev.ctrlKey || ev.metaKey;
    if (ctrl && ev.key === 'c') {
      ev.preventDefault();
      doCopy();
    } else if (ctrl && ev.key === 'x') {
      ev.preventDefault();
      doCut();
    } else if (ctrl && ev.key === 'v') {
      ev.preventDefault();
      doPaste();
    } else if (ctrl && ev.key === 'a') {
      ev.preventDefault();
      doSelectAll();
    } else if (ctrl && ev.key === 'f') {
      ev.preventDefault();
      showFind(true);
    } else if (ev.key === 'F3') {
      ev.preventDefault();
      runFind(ev.shiftKey);
    } else if (ev.key === 'Escape' && findBar && !findBar.classList.contains('hidden')) {
      ev.preventDefault();
      showFind(false);
    }
  });

  document.body.addEventListener('click', function (ev) {
    hideCtx();
    var edit = closest(ev.target, '[data-edit]');
    if (edit) {
      var name = edit.getAttribute('data-edit');
      if (name === 'copy') doCopy();
      else if (name === 'cut') doCut();
      else if (name === 'paste') doPaste();
      else if (name === 'selectAll') doSelectAll();
      else if (name === 'find') showFind(true);
      else if (name === 'undo') {
        try { document.execCommand('undo'); } catch (e) {}
      } else if (name === 'redo') {
        try { document.execCommand('redo'); } catch (e) {}
      }
      return;
    }
    var cmd = closest(ev.target, '[data-cmd]');
    if (!cmd) return;
    var n = cmd.getAttribute('data-cmd');
    if (n === 'find') {
      showFind(true);
      return;
    }
    post('cmd', { name: n });
  });

  if (document.getElementById('find-next'))
    document.getElementById('find-next').addEventListener('click', function () { runFind(false); });
  if (document.getElementById('find-prev'))
    document.getElementById('find-prev').addEventListener('click', function () { runFind(true); });
  if (document.getElementById('find-close'))
    document.getElementById('find-close').addEventListener('click', function () { showFind(false); });
  if (findInput)
    findInput.addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        runFind(ev.shiftKey);
      }
    });

  window.omsiApplyState = applyState;
  window.omsiSetStatus = setStatus;
  window.omsiInsertPaste = function (t) { applyState({ pasteText: t }); };
  post('ready', {});
})();
