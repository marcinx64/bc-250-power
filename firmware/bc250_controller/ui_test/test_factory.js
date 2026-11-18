// SPDX-License-Identifier: GPL-3.0-or-later
// 2.2.1: Factory reset card on the Firmware view, Scan time field in Wake options.
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "factory12345"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("t-fw").textContent.length > 1, 5000, "logged in");
  await sleep(1500);
  const sc = w.document.querySelector('[data-k="scan_s"], #f-scan_s') || [...w.document.querySelectorAll("input")].find(i => i.closest(".gen") && /Scan time/.test(i.closest(".gen").textContent) && i.value === "120");
  ok(!!sc, "Scan time field shown with 120 s");
  w.document.querySelector('.nv[data-view="controller"]').click(); await sleep(300);
  ok(!$("factory").hidden && !$("frbtn").disabled, "factory reset card shown, button enabled while off");
  $("frbtn").click();
  ok(!$("frask").hidden && /BC250-Setup/.test($("frask").textContent) && /wake lists/.test($("factory").textContent), "confirm explains what is erased and how to set up again");
  const before = (await counts())["/api/factory_reset"] || 0;
  $("frask").querySelectorAll("button")[1].click();
  ok($("frask").hidden && ((await counts())["/api/factory_reset"] || 0) === before, "Cancel sends nothing");
  $("frbtn").click(); $("frask").querySelectorAll("button")[0].click();
  await until(() => /setup mode/.test($("frmsg").textContent), 4000, "done message");
  ok(((await counts())["/api/factory_reset"] || 0) === before + 1 && $("frbtn").disabled, "sent once, button stays disabled: " + $("frmsg").textContent);
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
