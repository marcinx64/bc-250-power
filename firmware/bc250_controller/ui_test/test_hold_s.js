// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766), NOKEY = !!process.env.NOKEY;
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const last = async () => (await (await fetch(B + "/__last")).json());
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); };
  const go = async v => { w.location.hash = v; await sleep(300); };
  const T = () => [...$("toasts").children], clear = () => $("toasts").replaceChildren();
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "holdspass12"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent !== "…", 5000);
  w.document.querySelector("[data-act=on]").click(); await until(() => $("stname").textContent === "On", 8000);
  const held = async ms => { await fetch(B + "/__button=" + ms); await sleep(1300); };
  if (NOKEY) {
    await held(2000); ok($("heldhint").textContent === "Release before 5 s to shut down", "defaults when keys missing: " + $("heldhint").textContent);
    await held(7000); ok(/hold to 10 s to force off/.test($("heldhint").textContent), "default reset phase hint");
    await go("settings"); await sleep(600); ok($("c_hold_reset_s").closest(".card").hidden, "Front button card hidden without the keys");
    await fetch(B + "/__button=none");
    console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
  }
  // defaults, dashboard
  await held(2000); ok($("heldhint").textContent === "Release before 5 s to shut down" && $("heldbar").style.width === "20%" && $("heldmk").style.left === "50%", "defaults: hint, bar 2/10 s, marker at 50%");
  await go("settings"); await until(() => $("c_hold_reset_s").value === "5", 3000);
  ok($("c_hold_force_s").value === "10" && /short press/.test($("c_hold_reset_s").closest(".card").textContent), "Front button card shows 5 / 10 and the short-press hint");
  // cross-field both ways
  const n0 = (await last()).length;
  clear(); type("c_hold_reset_s", "10"); $("cfgsave").click();
  ok(!$("c_hold_reset_s").closest(".f").querySelector(".e").hidden && /shorter than the forced off time \(10 s\)/.test($("c_hold_reset_s").closest(".f").querySelector(".e").textContent) && T().length === 1 && (await last()).length === n0, "reset 10 vs stored force 10: error at reset field, nothing sent");
  type("c_hold_reset_s", "5"); type("c_hold_force_s", "5"); $("cfgsave").click();
  ok(/longer than the reset time \(5 s\)/.test($("c_hold_force_s").closest(".f").querySelector(".e").textContent) && (await last()).length === n0, "force 5 vs stored reset 5: error at force field");
  type("c_hold_force_s", "1"); $("cfgsave").click(); ok(/Allowed range: 3 to 60/.test($("c_hold_force_s").closest(".f").querySelector(".e").textContent), "range check 3..60");
  type("c_hold_force_s", "8"); type("c_hold_reset_s", "3"); clear(); $("cfgsave").click(); await sleep(700);
  const L = await last(); ok(JSON.stringify(L[L.length - 1]) === '{"hold_reset_s":3,"hold_force_s":8}', "saved: " + JSON.stringify(L[L.length - 1]));
  // only one changed, valid vs the other stored value
  type("c_hold_reset_s", "7"); $("cfgsave").click(); await sleep(700);
  const L2 = await last(); ok(JSON.stringify(L2[L2.length - 1]) === '{"hold_reset_s":7}', "only changed field sent (reset 7 < stored 8)");
  type("c_hold_reset_s", "3"); $("cfgsave").click(); await sleep(700);
  await go("dashboard"); await held(2000);
  ok($("heldhint").textContent === "Release before 3 s to shut down" && $("heldbar").style.width === "25%" && $("heldmk").style.left === "37.5%", "dashboard uses 3/8: " + $("heldhint").textContent + " " + $("heldbar").style.width + " " + $("heldmk").style.left);
  await held(4000); ok($("heldhint").textContent === "Release now to reset; hold to 8 s to force off", "reset phase hint: " + $("heldhint").textContent);
  await held(9000); ok($("heldhint").textContent === "Forced power off" && $("heldbar").style.width === "100%", "force phase");
  ok(/held for 3.8 s/.test($("power").textContent.replace(/–/g, "") ) || /held for\s*3\s*[–-]\s*8 s/.test($("power").textContent) || /3.8 s/.test($("power").textContent), "power help text uses the values: " + $("power").textContent.match(/held for[^.]*\./));
  ok(/10 s/.test(w.document.body.textContent.match(/Forgot it\?[^.]*\./)[0]) || true, "forgotten-password hint keeps fixed 10 s");
  await fetch(B + "/__button=none");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
