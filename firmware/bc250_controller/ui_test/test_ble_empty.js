// SPDX-License-Identifier: GPL-3.0-or-later
  const { open, until } = require("./harness.js");
const B = "http://127.0.0.1:" + process.env.P;
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "blepass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  w.location.hash = "wake"; await until(() => !$("scanbtn").closest("section").hidden, 3000);
  $("scanbtn").click();
  await until(() => !$("dempty").hidden, 20000, "empty state (15 s into the scan)");
  const r = [$("dlist").children.length === 0, /No controllers found/.test($("dempty").textContent), /classic Bluetooth/i.test($("dempty").textContent) && /Show all BLE devices/.test($("dempty").textContent) && !/Show all devices/.test($("dempty").textContent) && /Scanning/.test($("scanmsg").textContent), $("dhid").textContent === "3 other devices hidden"];
  console.log(r.every(Boolean) ? "PASS empty state shown after scan with no controller-like device, hidden count 3" : "FAIL empty state " + r);
  $("showall").checked = true; $("showall").dispatchEvent(new w.Event("change"));
  console.log($("dempty").hidden && $("dlist").children.length === 3 ? "PASS show all hides empty state, lists 3" : "FAIL show all");
  process.exit(0);
})().catch(e => { console.log("ERROR", e.message); process.exit(2); });

