// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until } = require("./harness.js");
const B = "http://127.0.0.1:8766";
(async () => {
  const d = await open(B + "/setup"), w = d.window; w.scrollTo = () => {}; const $ = id => w.document.getElementById(id);
  const submit = id => $(id).dispatchEvent(new w.Event("submit", { cancelable: true }));
  $("start").click(); await until(() => !$("alog").hidden, 3000); console.log("PASS login form when password set");
  $("lp").value = "bad"; submit("alog"); await until(() => $("alerr").textContent.includes("Wrong"), 3000); console.log("PASS wrong pw msg");
  $("lp").value = "setuppass1"; submit("alog"); await until(() => !$("s2").hidden && $("nets").children.length > 0, 8000); console.log("PASS login -> scan list");
  await fetch(B + "/__kill", { method: "POST" });
  $("rescan").click(); await until(() => !$("s0").hidden && !$("alog").hidden, 8000); console.log("PASS 401 during scan -> login step");
  process.exit(0);
})().catch(e => { console.log("ERROR", e.message); process.exit(2); });
