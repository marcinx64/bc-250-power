// SPDX-License-Identifier: GPL-3.0-or-later
// Needs the mock with MOCK_TEMP=absent: firmware before 2.2.0 sends no chip_temp_c, cpu
// or heap_total; the dashboard hides those rows instead of showing empty bars.
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "oldfwpass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("t-fw").textContent.length > 1, 5000, "first state");
  const hidden = id => $(id).hidden && $(id).nextElementSibling.hidden;
  ok(hidden("t-cpu-l") && hidden("t-mem-l") && $("t-temp").parentElement.hidden, "older firmware: CPU, memory and temperature rows hidden");
  ok(!$("t-up").parentElement.hidden, "other rows still shown");
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
