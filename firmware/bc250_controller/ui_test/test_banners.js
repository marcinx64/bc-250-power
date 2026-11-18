// SPDX-License-Identifier: GPL-3.0-or-later
// All status messages are banners at the top: lasting ones as full-width bars in the page flow, short-lived ones as narrow cards floating in #toasts under them.
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/", w => { w.scrollTo = () => {}; }), w = d.window, $ = id => w.document.getElementById(id);
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "bannerpass1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 5000, "dashboard");
  const order = [...$("banners").children].map(e => e.id).join(",");
  ok(order === "pend,rmmsg,frmsg,ipnote,offline,dbg,toasts", "lasting messages in order of importance, short-lived ones last: " + order);
  const cs = id => w.getComputedStyle($(id));
  ok(cs("banners").position === "sticky" && ["", "0px", "auto"].includes(cs("banners").minHeight) && cs("toasts").position === "absolute", "lasting messages in the page flow, no space kept free; short-lived ones float");
  const shown = id => cs(id).display !== "none";
  $("offline").hidden = false; $("dbg").hidden = false;
  ok(shown("offline") && shown("dbg"), "several lasting messages show together");
  $("pend").hidden = false;
  ok(shown("pend") && !shown("offline") && shown("dbg"), "a restart in progress explains the lost connection: connection lost waits, debug stays");
  $("pend").hidden = true; $("offline").hidden = true; $("dbg").hidden = true;
  await until(() => [...$("toasts").children].some(e => /Password set/.test(e.textContent)), 3000, "password set banner");
  const b = $("toasts").firstElementChild;
  ok(b.classList.contains("bn") && b.classList.contains("ok") && b.querySelector(".x"), "success banner: banner style, ok colour, dismiss button");
  b.querySelector(".x").click();
  ok($("toasts").children.length === 0, "dismiss removes it");
  ["offline", "pend", "dbg", "rmmsg", "frmsg", "ipnote"].forEach(id => { if (!$(id).classList.contains("bn") || $(id).parentElement !== $("banners")) ok(false, id + " is a top banner"); });
  ok(true, "fixed banners share the banner style");
  ok(!w.document.querySelector("main #rmmsg, main #frmsg, main #ipnote"), "no status lines left inside the cards");
  $("sidelogout").click();
  await until(() => !$("gate").hidden && [...$("gtoasts").children].some(e => /Logged out/.test(e.textContent)), 3000, "logged out banner on the login screen");
  ok(true, "on the login screen the banner shows above the form");
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no script errors " + d.errors.join(" | "));
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
