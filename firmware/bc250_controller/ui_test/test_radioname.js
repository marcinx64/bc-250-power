// SPDX-License-Identifier: GPL-3.0-or-later
// Menu shows a neutral "Wake" before the state arrives, then the method's name; a reload shows the remembered name at once.
const { open, until, sleep } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8793);
  await fetch(B + "/__reset", { method: "POST" });
  const html = await (await fetch(B + "/")).text();
  ok(/<span id="nv-ble">Wake<\/span>/.test(html) && !/>Bluetooth</.test(html.slice(html.indexOf("<nav"), html.indexOf("</nav>"))), "static menu entry is neutral: Wake");
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  let d = await open(B + "/#wake", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  ok(w.location.hash === "#wake", "view address is #wake");
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "blepass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("nv-ble").textContent === "2.4G/BT wake", 5000, "named after state");
  ok(mem.bc250_radio === "2g4", "method remembered");
  w.close();
  d = await open(B + "/#wake", pre); w = d.window; $ = id => w.document.getElementById(id);
  ok($("nv-ble").textContent === "2.4G/BT wake" && /circle/.test(w.document.querySelector(".nv[data-view=wake] svg").innerHTML) && $("vtitle").textContent === "2.4G/BT wake", "reload: 2.4G name, icon and title before any state");
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
