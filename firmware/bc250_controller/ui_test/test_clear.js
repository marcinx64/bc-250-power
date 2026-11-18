// SPDX-License-Identifier: GPL-3.0-or-later
// "Clear wake lists" card (Controller page): the BLE list and, from 2.2.21, the two kinds of the
// 2.4G/BT list (2.4G Dongle devices, Bluetooth Classic hosts) cleared separately, whichever method runs.
// Needs the mock with MOCK_OTHER=1 (the unused method's list holds entries) on port P; once with
// MOCK_RADIO=2g4 too.
const { open, until } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "clearpass1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  const lists = async () => (await w.fetch(B + "/api/wake/lists")).json();
  const l0 = await lists(), g2 = l0.radio === "2g4", cur = g2 ? ["dongle", "bt"] : ["ble"];
  ok(l0.ble > 0 && l0.dongle > 0 && !("2g4" in l0), "mock: lists have entries " + JSON.stringify(l0));
  w.location.hash = "controller";
  await until(() => !$("clearc").hidden && $("cl-ble").textContent !== "-", 5000, "clear card");
  { const c = [...w.document.querySelectorAll("main>section[data-view=controller]")].map(e => e.id);
    ok(c.indexOf("clearc") === c.indexOf("factory") - 2 && c.indexOf("cfgreset") === c.indexOf("factory") - 1, "on the Controller page: Clear, Reset settings, Factory reset: " + c.join()); }
  const names = [...$("clearc").querySelectorAll(".nm")].map(e => e.textContent);
  ok(names.join("|") === "Bluetooth Low Energy devices wake list|2.4G Dongle devices wake list|Bluetooth Classic hosts wake list", "three lists: " + names.join(" | "));
  const want = k => { const n = l0[k], u = k === "bt" ? "host" : "device"; return n ? n + " " + u + (n === 1 ? "" : "s") : "Empty"; };
  ok(["ble", "dongle", "bt"].every(k => $("cl-" + k).textContent === want(k)), "counts, no in-use marker: " + ["ble", "dongle", "bt"].map(k => $("cl-" + k).textContent).join(" / "));
  // one list of the method not running; the others stay
  const other = g2 ? "ble" : "bt";
  $("cl-" + other + "-b").click();
  ok(!$("clask").hidden && /cannot be undone/.test($("clask").textContent) && new RegExp($("clearc").querySelector("#cl-" + other).previousElementSibling.textContent.replace(" wake list", "")).test($("clask").textContent), "asks first, names the list: " + $("clask").textContent);
  $("clask").querySelectorAll("button")[1].click(); ok($("clask").hidden && (await lists())[other] === l0[other], "cancel keeps the list");
  $("cl-" + other + "-b").click(); $("clask").querySelectorAll("button")[0].click();
  await until(() => /^Empty/.test($("cl-" + other).textContent), 4000, "other cleared");
  const l1 = await lists();
  ok(l1[other] === 0 && ["ble", "dongle", "bt"].filter(k => k !== other).every(k => l1[k] === l0[k]) && $("cl-" + other + "-b").disabled, "only that list cleared: " + JSON.stringify(l1));
  // the running method's (2.4G Dongle devices or BLE) list
  const run = g2 ? "dongle" : "ble";
  $("cl-" + run + "-b").click(); $("clask").querySelectorAll("button")[0].click();
  await until(() => /^Empty/.test($("cl-" + run).textContent), 4000, "running cleared");
  ok((await lists())[run] === 0, "running list cleared");
  w.location.hash = "wake"; await until(() => !$("wempty").hidden, 5000, "wake page empty");
  ok(!$("wempty").hidden, "the wake page then shows the list empty");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
