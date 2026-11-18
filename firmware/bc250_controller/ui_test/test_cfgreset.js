// SPDX-License-Identifier: GPL-3.0-or-later
// "Reset settings" card (Controller page): settings back to the firmware defaults (re-arm 30 s,
// scan 60 s, holds 3 / 5 s), hostname and wake switch kept; unsaved edits dropped. Security card full
// width with fields as wide as the others. Mock: MOCK_CONN=1 on port P.
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/#controller", w => { w.scrollTo = () => {}; w.Intl.DateTimeFormat = () => ({ resolvedOptions: () => ({ timeZone: "Europe/Kiev" }) }); }), w = d.window, $ = id => w.document.getElementById(id);
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "cfgreset1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && !$("cfgreset").hidden && !$("crbtn").disabled, 6000, "reset card");
  const cfg = async () => (await w.fetch(B + "/api/config")).json();
  await w.fetch(B + "/api/config", { method: "POST", body: JSON.stringify({ ble_rearm_s: 99, scan_s: 300, hold_reset_s: 7, hold_force_s: 12, hostname: "bench", ble_wake: false }) });
  { const c = [...w.document.querySelectorAll("main>section[data-view=controller]")].map(e => e.id);
    ok(c.indexOf("cfgreset") === c.indexOf("factory") - 1, "just above Factory reset: " + c.join()); }
  $("crbtn").click();
  ok(!$("crask").hidden && /defaults/.test($("crask").textContent), "asks first: " + $("crask").textContent);
  $("crask").querySelectorAll("button")[1].click();
  ok($("crask").hidden && (await cfg()).ble_rearm_s === 99, "cancel changes nothing");
  $("crbtn").click(); $("crask").querySelectorAll("button")[0].click();
  await until(() => [...$("toasts").children].some(e => /Settings reset to defaults/.test(e.textContent)), 4000, "done banner");
  const c1 = await cfg();
  ok(c1.ble_rearm_s === 30 && c1.scan_s === 60 && c1.hold_reset_s === 3 && c1.hold_force_s === 5, "defaults: re-arm 30, scan 60, holds 3 / 5: " + JSON.stringify([c1.ble_rearm_s, c1.scan_s, c1.hold_reset_s, c1.hold_force_s]));
  ok(c1.hostname === "bench" && c1.ble_wake === false, "hostname and wake switch kept");
  for (let i = 0; i < 40 && (await cfg()).tz_name !== "Europe/Kyiv"; i++) await new Promise(r => setTimeout(r, 100));
  { const c2 = await cfg(); ok(c2.tz === "EET-2EEST,M3.5.0/3,M10.5.0/4", "no timezone after the reset: the browser's is set, not Custom: UTC0: " + c2.tz); }
  await until(() => $("c_ble_rearm_s").value === "30", 4000, "fields reloaded");
  ok($("c_hold_reset_s").value === "3", "the page shows the defaults");
  // Security: half card, fields like the others
  w.location.hash = "security"; await until(() => !$("sec").hidden, 3000, "security");
  ok(!$("sec").classList.contains("half") && $("so").style.width === "" && w.getComputedStyle($("so")).width === "200px", "Security card full width, 200 px fields: " + w.getComputedStyle($("so")).width);
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
