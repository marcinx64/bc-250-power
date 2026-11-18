// SPDX-License-Identifier: GPL-3.0-or-later
// The wake-method switch on the wake page (P = a plain mock's port).
const { open, until, sleep } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
async function boot(B) {
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "blepass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  return { d, w, $ };
}
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8795), o = await boot(B), $ = o.$, w = o.w;
  w.location.hash = "wake"; await until(() => !$("radiocard").hidden && $("rm-sw").textContent, 5000, "card");
  { const secs = [...w.document.querySelectorAll("main>section[data-view=wake]")].filter(e => !e.hidden);
    ok(secs[0] === $("radiocard"), "wake method is the first card on the wake page"); }
  ok($("rm-sw").textContent === "Switch to 2.4G/BT Classic" && !$("rmcur") && $("rm-ble").classList.contains("cur") && $("rm-ble").getAttribute("aria-current") === "true" && !$("rm-2g4").classList.contains("cur") && /2\.4G USB dongle/.test($("rm-2g4").textContent) && !/calls the host it is paired with/.test($("rm-2g4").textContent) && !/any device that calls/.test($("rm-2g4").textContent) && w.document.querySelectorAll("#radiocard button:not(.confirm button)").length === 1, "one button: Switch to 2.4G (dongle mode)");
  ok(!w.document.querySelector("section[data-view=settings]#radiocard"), "not on the Settings page");
  await until(() => o.w.document.getElementById("rm-sw") && true, 100);
  $("rm-sw").click();
  ok(!$("rmask").hidden && /Switch to 2\.4G\/BT wake\? The controller restarts/.test($("rmask").textContent) && !$("rmdel") && !$("rmask").querySelector("input"), "asks to restart, no delete-list option: " + $("rmask").textContent);
  $("rmask").querySelectorAll("button")[1].click(); ok($("rmask").hidden, "cancel closes it");
  $("rm-sw").click(); $("rmask").querySelectorAll("button")[0].click();
  await until(() => /restarting/i.test($("rmmsg").textContent) && !$("rmmsg").hidden, 4000, "restarting");
  const i = await (await w.fetch(B + "/api/info")).json();
  ok(i.radio === "2g4", "switch posted: controller now 2g4");
  ok(!/"delete"/.test(JSON.stringify(await (await w.fetch(B + "/__last")).json().catch(() => ({})))), "switch posts no delete flag");
  // back to Bluetooth over the API: the kept list returns
  const sw = async (r) => (await w.fetch(B + "/api/radio", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ radio: r }) })).json();
  const list = async () => (await (await w.fetch(B + "/api/ble/wake")).json()).list || [];
  await sw("ble"); ok((await list()).length === 1, "Bluetooth list kept across a switch and back");
  // a reload straight on the wake page shows the card (it follows the state);
  // in 2.4G mode the button offers Bluetooth
  await sw("2g4");
  o.w.close();
  const o2 = await (async () => { const mem = {}; const pre = x => Object.defineProperty(x, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } }); const d = await open(B + "/#wake", pre); return d; })();
  const $2 = id => o2.window.document.getElementById(id);
  await until(() => !$2("gate").hidden || !$2("radiocard").hidden, 5000, "page up");
  if (!$2("gate").hidden && !$2("glog").hidden) { $2("gp").value = "blepass123"; $2("glog").dispatchEvent(new o2.window.Event("submit", { cancelable: true })); }
  await until(() => !$2("radiocard").hidden && $2("rm-sw").textContent === "Switch to BLE", 6000, "card after reload on #wake");
  ok($2("rm-2g4").classList.contains("cur") && !$2("rm-ble").classList.contains("cur"), "card shown after a reload on the wake page; in 2.4G it offers Switch to BLE");
  // Firmware page: a file shows its version, nothing about wake methods
  const w0 = w; void w0; return finish(o2);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
async function finish(o2) {
  const w = o2.window, $ = id => w.document.getElementById(id);
  w.location.hash = "controller"; await until(() => !$("ab-dl").hidden, 4000);
  const u = new Uint8Array(4000); u[0] = 0xE9; u[12] = 5; const mk = "@@" + "BC250FW|2.2.0|2026-11-16 13:00:00|@@"; for (let k = 0; k < mk.length; k++) u[1000 + k] = mk.charCodeAt(k);
  Object.defineProperty($("file"), "files", { value: [new w.File([u], "fw.bin")], configurable: true }); $("file").dispatchEvent(new w.Event("change"));
  await until(() => !$("fpanel").hidden && /version 2\.2\.0/.test($("fchecks").textContent), 4000, "file");
  ok(!$("fswap") && !/wake list|Wakes the BC-250/.test($("fchecks").textContent), "a firmware file brings no wake-method warning");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
}
