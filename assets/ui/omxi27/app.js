/* omxi27 start UI — state injected by host via applyState(JSON) when Ultralight/WebView is wired.
   Native Win32 shell mirrors the same layout/labels in Spanish. */
(function () {
  const state = { maps: [], sits: [], eps: [], mapIdx: 0, sitIdx: 0, epIdx: 0, mode: "new", root: "" };

  function el(id) { return document.getElementById(id); }

  function renderList(node, items, key, sel, onPick) {
    if (!node) return;
    node.innerHTML = "";
    items.forEach((it, i) => {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "list-item" + (i === sel ? " sel" : "");
      b.textContent = typeof it === "string" ? it : (it[key] || it.name || "?");
      b.onclick = () => onPick(i);
      node.appendChild(b);
    });
  }

  function refresh() {
    renderList(el("map-list"), state.maps, "name", state.mapIdx, (i) => { state.mapIdx = i; refresh(); });
    renderList(el("sit-list"), state.sits, "name", state.sitIdx, (i) => { state.sitIdx = i; refresh(); });
    renderList(el("ep-list"), state.eps, "name", state.epIdx, (i) => { state.epIdx = i; refresh(); });
    if (el("map-count")) el("map-count").textContent = String(state.maps.length);
    if (el("ep-count")) el("ep-count").textContent = String(state.eps.length);
    if (el("omsi-root")) el("omsi-root").textContent = state.root || "";
    const m = state.maps[state.mapIdx];
    if (el("map-info")) el("map-info").textContent = m ? (m.info || m.path || m.name) : "Seleccionar un mapa";
  }

  window.applyState = function (json) {
    try {
      const s = typeof json === "string" ? JSON.parse(json) : json;
      Object.assign(state, s);
      refresh();
    } catch (e) {
      if (el("status")) el("status").textContent = "Estado inválido";
    }
  };

  function post(cmd, payload) {
    if (window.omsiPost) window.omsiPost(cmd, JSON.stringify(payload || {}));
    else if (el("status")) el("status").textContent = cmd;
  }

  el("btn-start")?.addEventListener("click", () => post("start", state));
  el("btn-options")?.addEventListener("click", () => post("options", {}));
  el("btn-quit")?.addEventListener("click", () => post("quit", {}));
  document.querySelectorAll('input[name="mode"]').forEach((r) => {
    r.addEventListener("change", () => { if (r.checked) state.mode = r.value; });
  });

  refresh();
})();
