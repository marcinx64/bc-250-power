// SPDX-License-Identifier: GPL-3.0-or-later
// Appearance card: Auto, Light, Dark and five gaming themes, kept in this browser, applied before the page paints.
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => (w.scrollTo = () => {}, Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = String(v); }, removeItem: k => { delete mem[k]; } } }));
  let d = await open(B + "/#settings", pre), w = d.window, $ = id => w.document.getElementById(id);
  const root = () => w.document.documentElement.dataset.theme;
  const btn = t => w.document.querySelector(`#ths [data-t=${t}]`);
  const pressed = () => [...w.document.querySelectorAll("#ths [data-t]")].filter(b => b.getAttribute("aria-pressed") === "true").map(b => b.dataset.t).join();
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "themepass1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && !$("appear").hidden, 5000, "settings view");
  ok(root() === undefined && pressed() === "auto", "default: Auto, no data-theme");
  btn("dark").click();
  ok(root() === "dark" && mem.bc250_theme === "dark" && pressed() === "dark", "Dark: applied and remembered");
  btn("light").click();
  ok(root() === "light" && mem.bc250_theme === "light" && pressed() === "light", "Light: applied and remembered");
  const names = [...w.document.querySelectorAll("#ths .th")].map(b => b.textContent).join();
  ok(names === "Auto,Light,Dark,Synthwave,Toxic,Ember,Glacier,Arcade", "eight tiles: " + names);
  ok(btn("auto").querySelectorAll(".pv>span").length === 2 && btn("neon").querySelector(".pv>span").dataset.theme === "neon", "previews use the themes' own colours");
  for (const t of ["neon", "toxic", "ember", "frost", "arcade"]) { btn(t).click(); if (!(root() === t && mem.bc250_theme === t && pressed() === t)) ok(false, t + " applied"); }
  ok(root() === "arcade", "gaming themes applied and remembered");
  w.close();
  mem.bc250_theme = "bogus";
  d = await open(B + "/#settings", pre); w = d.window;
  ok(w.document.documentElement.dataset.theme === undefined, "unknown stored theme ignored");
  w.close();
  mem.bc250_theme = "light";
  d = await open(B + "/#settings", pre); w = d.window; $ = id => w.document.getElementById(id);
  ok(w.document.documentElement.dataset.theme === "light", "reload: Light set in the head, before any state");
  ok(pressed() === "light", "reload: Light shown as chosen");
  btn("auto").click();
  ok(root() === undefined && !("bc250_theme" in mem) && pressed() === "auto", "Auto: follows the browser, choice removed");
  w.location.hash = "#dashboard";
  await until(() => $("appear").hidden, 3000, "card only on settings");
  ok(true, "card hidden outside Settings");
  ok(d.errors.length === 0, "no script errors " + d.errors.join(" | "));
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
