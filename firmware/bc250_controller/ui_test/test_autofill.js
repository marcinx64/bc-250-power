// SPDX-License-Identifier: GPL-3.0-or-later
// Browser autofill: no text or number field takes saved form values (autocomplete="off"),
// and each password form has its own username field, so a password manager never takes another
// field (an NTP server, a name) as the username. Mock: MOCK_RADIO=2g4 on port P (both list kinds,
// Learn rows and the settings forms are built).
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8793);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/#wake", w => { w.scrollTo = () => {}; }), w = d.window, $ = id => w.document.getElementById(id);
  await until(() => !$("gset").hidden, 3000);
  const forms = ["gset", "glog", "secform"];
  ok(forms.every(f => { const u = $(f).querySelectorAll("input[autocomplete=username]"); return u.length === 1 && u[0].value && u[0].tabIndex === -1; }), "each password form has one username field");
  $("gn1").value = $("gn2").value = "autofill1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && !$("hcard").hidden, 6000, "wake page");
  $("hlearnbtn").click();
  await until(() => $("hllist").children.length === 1, 6000, "a Learn row");
  [...$("wlist").querySelectorAll("button")].find(b => b.textContent === "Rename").click();
  w.location.hash = "settings"; await until(() => $("c_ble_rearm_s"), 4000, "settings fields");
  const bad = [...w.document.querySelectorAll("input")].filter(i => /^(text|number|search|email|url|tel|)$/.test(i.type) && i.autocomplete !== "off" && i.getAttribute("autocomplete") !== "username");
  ok(bad.length === 0, "every text and number field: autocomplete off (" + w.document.querySelectorAll("input").length + " inputs) " + bad.map(i => i.id || i.outerHTML.slice(0, 80)).join(", "));
  const pw = [...w.document.querySelectorAll("input[type=password]")];
  ok(pw.every(i => /^(current|new)-password$/.test(i.autocomplete)), "password fields keep current-/new-password");
  $("hlearnbtn").click();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
