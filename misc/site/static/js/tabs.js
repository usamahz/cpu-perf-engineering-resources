// ARIA tabs over sections the page already shows one after another.
for (const wrap of document.querySelectorAll("[data-tabs]")) {
  const list = wrap.querySelector("[role=tablist]");
  const tabs = Array.from(list.querySelectorAll("[role=tab]"));
  const panels = tabs.map((t) => document.getElementById(t.getAttribute("aria-controls")));
  const select = (i, focus) => {
    tabs.forEach((t, j) => {
      const on = i === j;
      t.setAttribute("aria-selected", String(on));
      t.tabIndex = on ? 0 : -1;
      panels[j].hidden = !on;
    });
    if (focus) tabs[i].focus();
  };
  list.hidden = false;
  for (const h of wrap.querySelectorAll(".tab-heading")) h.classList.add("sr-only");
  tabs.forEach((t, i) => {
    t.addEventListener("click", () => { select(i); history.replaceState(null, "", `#${panels[i].id}`); });
    t.addEventListener("keydown", (e) => {
      const d = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
      if (d) { e.preventDefault(); select((i + d + tabs.length) % tabs.length, true); }
    });
  });
  const fromHash = panels.findIndex((p) => `#${p.id}` === location.hash);
  select(fromHash >= 0 ? fromHash : 0);
}
