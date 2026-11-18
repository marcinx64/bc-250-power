// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); $(id).dispatchEvent(new w.Event("change", { bubbles: true })); };
  const go = async v => { w.location.hash = v; await until(() => !w.document.querySelector(`main>section[data-view=${v}]:not([hidden])`) === false || v === "bluetooth", 3000, v); await sleep(150); };
  const clear = () => { $("toasts").replaceChildren(); };
  const T = () => [...$("toasts").children];
  const one = async (kind, re, what) => { await until(() => T().length >= 1, 3000, what); await sleep(200); const t = T(); ok(t.length === 1 && t[0].classList.contains(kind) && re.test(t[0].querySelector("span").textContent) && t[0].getAttribute("role") === (kind === "err" ? "alert" : "status"), `${what}: one ${kind} toast "${t.map(x => x.querySelector("span").textContent)}"`); };
  await until(() => !$("gset").hidden, 3000);
  type("gn1", "toastpass1"); type("gn2", "toastpass1"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  await until(() => T().some(t => /Password set/.test(t.textContent)), 2000, "password set toast"); ok(true, "set-password success -> toast");
  ok(["pmsg", "bmsg", "cmsg", "blemsg", "nmsg2", "secmsg", "dbgmsg", "toast"].every(i => !$(i)), "no leftover inline result elements");
  // power
  clear(); w.document.querySelector("[data-act=on]").click(); await one("ok", /Power on requested/, "power on");
  await until(() => $("stname").textContent === "On", 8000);
  // bluetooth: toggle wake off
  await go("wake");
  const cards = () => [...w.document.querySelectorAll("main>section[data-view=wake]")];
  await until(() => cards().filter(c => !c.hidden).length === 4, 3000, "4 bluetooth cards"); ok(true, "wake on: 4 cards (Wake method, Wake, Wake options, Wake list with Add)");
  type("c_ble_rearm_s", "25");
  clear(); $("blesw").checked = false; $("blesw").dispatchEvent(new w.Event("change")); await one("ok", /BLE wake off/, "wake off");
  await until(() => cards().filter(c => !c.hidden).length === 2, 3000, "only method + wake card"); ok(!$("blenote").hidden, "wake off: only Wake method and Wake cards + note");
  ok($("scanbtn").disabled, "scan disabled while off");
  const c0 = await counts(); await sleep(4500); const c1 = await counts();
  ok(c1["/api/ble/wake"] === c0["/api/ble/wake"] && c1["/api/ble/devices"] === c0["/api/ble/devices"], "no wake-list/devices requests while off");
  // failed toggle on
  await fetch(B + "/__failcfg1"); clear();
  $("blesw").checked = true; $("blesw").dispatchEvent(new w.Event("change")); await one("err", /boom/, "failed wake on");
  ok($("blesw").checked === false && cards().filter(c => !c.hidden).length === 2, "failed toggle reverts switch, cards stay hidden");
  T()[0].querySelector("button").click(); ok(T().length === 0, "error toast can be dismissed");
  await fetch(B + "/__failcfg0"); clear();
  $("blesw").checked = true; $("blesw").dispatchEvent(new w.Event("change")); await one("ok", /BLE wake on/, "wake on");
  await until(() => cards().filter(c => !c.hidden).length === 4, 3000); ok($("c_ble_rearm_s").value === "25" && !$("bleunsaved").hidden, "cards back; unsaved Wake options edit kept");
  await sleep(3500); const c2 = await counts(); ok(c2["/api/ble/wake"] > c1["/api/ble/wake"], "wake list polling resumed");
  // wake options validation + save
  type("c_ble_rearm_s", "1"); clear(); $("blesave").click(); await one("err", /highlighted/, "wake options validation");
  type("c_ble_rearm_s", "25"); clear(); $("blesave").click(); await one("ok", /^Saved/, "wake options save");
  // remove + add
  clear(); const rw = $("wlist").firstChild; [...rw.querySelectorAll("button")].find(b => b.textContent === "Remove").click(); rw.querySelector(".confirm button.dan").click(); await one("ok", /Removed/, "wake remove");
  clear(); $("scanbtn").click(); await until(() => $("dlist").children.length > 0, 4000); await until(() => T().length > 0, 2000, "scan toast"); ok(T().length === 1 && T()[0].classList.contains("info") && /^Scan started/.test(T()[0].textContent), "scan start: one info toast");
  const addb = [...$("dlist").querySelectorAll("button")].find(b => !b.disabled); clear(); addb.click(); await one("ok", /^Added /, "wake add");
  clear(); $("scanbtn").click(); await one("info", /^Scan stopped/, "scan stopped by hand");
  clear(); $("scanbtn").click(); await one("info", /^Scan started/, "scan started again");
  clear(); await fetch(B + "/__scanend"); await one("info", /^Scan stopped/, "scan stopped: time up");
  ok($("scanbtn").textContent === "Scan for devices", "scan button back after time up");
  // settings
  await go("settings"); await until(() => $("c_led_on").value !== "", 3000);
  type("c_led_on", "99"); clear(); $("cfgsave").click(); await one("ok", /^Saved$/, "settings save");
  type("c_led_on", "98"); clear(); $("cfgrev").click(); await one("info", /discarded/, "settings discard");
  type("c_start_timeout_ms", "1"); clear(); $("cfgsave").click(); await one("err", /highlighted/, "settings validation");
  type("c_start_timeout_ms", "15"); $("cfgrev").click();
  await fetch(B + "/__failcfg1"); type("c_led_on", "97"); clear(); $("cfgsave").click(); await one("err", /boom/, "settings API error"); await fetch(B + "/__failcfg0");
  $("cfgrev").click();
  // hostname
  await go("network"); await until(() => $("hn").value === "bc250", 3000);
  type("hn", "toast-1"); clear(); $("hnsave").click(); await one("ok", /Saved/, "hostname save");
  // debug + showall
  await go("debug"); clear(); $("dbgsel").value = "lit"; $("dbgsel").dispatchEvent(new w.Event("change")); await until(() => !$("dbg").hidden, 3000, "debug bar"); await sleep(300); ok(T().length === 0, "debug mode on: the yellow bar, no toast");
  clear(); $("showall").checked = true; $("showall").dispatchEvent(new w.Event("change")); await one("info", /all BLE devices/, "show-all switch");
  clear(); $("dbgback").click(); await one("ok", /Real/, "back to real");
  // security
  await go("security"); type("so", "toastpass1"); type("s1", "short"); type("s2", "short"); clear(); $("secform").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await one("err", /highlighted/, "password validation"); ok($("secerr").textContent.length > 0, "inline field message kept");
  type("so", "nope"); type("s1", "newtoast123"); type("s2", "newtoast123"); clear(); $("secform").dispatchEvent(new w.Event("submit", { cancelable: true })); await one("err", /wrong current/, "password API error");
  // power: reset refused? force off ok
  await go("dashboard"); clear(); w.document.querySelector("[data-act=force_off]").click(); $("pyes").click(); await one("ok", /Power cut/, "force off");
  // stack + timing
  clear(); w.document.querySelector("[data-act=on]").click(); await until(() => T().length === 1, 2000);
  await go("security"); type("so", "nope"); type("s1", "newtoast123"); type("s2", "newtoast123"); $("secform").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => T().length === 2, 3000); ok(T().length === 2, "toasts stack");
  await sleep(4200); ok(T().length === 1 && T()[0].classList.contains("err"), "success auto-dismissed (~3.5 s), error still shown");
  await sleep(4200); ok(T().length === 0, "error auto-dismissed after ~8 s");
  w.toast("one"); w.toast("two"); w.toast("three");
  ok(T().length === 2 && T()[0].textContent.startsWith("two") && T()[1].textContent.startsWith("three"), "at most two at a time: the oldest goes");
  clear();
  // logout
  clear(); $("sidelogout").click(); await until(() => !$("gate").hidden && [...$("gtoasts").children].some(e => e.classList.contains("info") && /Logged out/.test(e.textContent)), 3000, "logout"); ok(true, "logout banner shown above the login form");
  // late state: reload on bluetooth view with wake off, never flash cards
  await fetch(B + "/__reset", { method: "POST" });
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
