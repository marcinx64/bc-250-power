// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id);
  w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); };
  const go = async v => { w.location.hash = v; await until(() => !!w.document.querySelector(`main>section[data-view=${v}]:not([hidden])`), 3000, v); };
  const state = () => $("stname").textContent;
  await until(() => !$("gset").hidden, 3000);
  type("gn1", "flowpass12"); type("gn2", "flowpass12"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && state() === "Off", 4000, "login");
  const powerOn = async () => { w.document.querySelector("[data-act=on]").click(); await until(() => state() === "On", 8000, "on"); };
  const modalOpen = () => !$("modal").hidden;
  // off: restart works as before (inline confirm, no modal)
  await go("controller");
  ok(!$("restart").disabled && $("rnote").textContent === "Restarting cuts power to the BC-250." && $("fwnote").textContent === "Updating cuts power to the BC-250.", "off: restart enabled, each card names its own effect on the power: " + $("rnote").textContent);
  $("restart").click(); ok(!modalOpen() && !$("rconf").hidden, "off: inline confirm only, no modal");
  $("rconf").querySelectorAll("button")[1].click();
  // starting -> disabled with reason
  await go("dashboard"); w.document.querySelector("[data-act=on]").click();
  await until(() => state() === "Starting", 3000); await go("controller");
  ok($("restart").disabled && $("upbtn").disabled && /Wait until the BC-250 is on or off/.test($("rnote").textContent), "starting: disabled with reason: " + $("rnote").textContent);
  await until(() => state() === "On", 8000);
  ok(!$("restart").disabled && /shut down cleanly first/.test($("rnote").textContent), "on: restart enabled");
  // cancel at ask
  $("restart").focus(); $("restart").click();
  ok(modalOpen() && /cannot restart/.test($("mt").textContent) && $("mgo").textContent === "Shut down and restart" && $("app").inert, "on: modal explains, buttons right, background inert");
  ok(w.document.activeElement === $("mgo"), "focus moved into modal");
  w.document.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
  ok(!modalOpen() && w.document.activeElement === $("restart"), "Escape cancels, focus returns to trigger");
  await sleep(1500); ok(state() === "On" && !(await counts())["/api/restart"], "cancel at ask: nothing requested");
  // success path: restart
  $("restart").click(); $("mgo").click();
  await until(() => /Waiting for the BC-250/.test($("mb").textContent), 3000, "wait phase");
  ok(!$("pend").hidden && $("restart").disabled && $("upbtn").disabled && /Keep this page open/.test($("mb").textContent) && /does not stop the shutdown/.test($("mb").textContent), "wait phase text + pending indicator + controls locked");
  await go("dashboard"); ok(modalOpen() && !$("pend").hidden && w.document.querySelector("[data-act=on]").disabled, "modal/pending stay across views; Power on disabled");
  await sleep(1500); ok(/\d+ s \(state: /.test($("mb").firstChild.textContent), "elapsed + state shown: " + $("mb").firstChild.textContent);
  await until(() => /Restarting/.test($("mt").textContent), 12000, "restart phase");
  const c = await counts(); ok(c["/api/restart"] === 1 && c["/api/power"] >= 2, "after state off, POST /api/restart sent once");
  ok($("mcancel").hidden, "no Cancel during restart phase");
  await until(() => /Back online/.test($("mb").textContent), 15000, "back online");
  ok(true, "restart success path ends in 'Back online. Reloading'");
  // restart left a pending pend object (page would reload); reset page state for next tests
  w.location.hash = "dashboard";
  // failure path
  await fetch(B + "/__reset", { method: "POST" }); 
  const d2 = await open(B + "/"), w2 = d2.window, $2 = id => w2.document.getElementById(id); w2.scrollTo = () => {};
  const type2 = (id, v) => { $2(id).value = v; $2(id).dispatchEvent(new w2.Event("input", { bubbles: true })); };
  await until(() => !$2("gset").hidden, 3000);
  type2("gn1", "flowpass12"); type2("gn2", "flowpass12"); $2("gset").dispatchEvent(new w2.Event("submit", { cancelable: true }));
  await until(() => $2("gate").hidden && $2("stname").textContent === "Off", 4000);
  w2.document.querySelector("[data-act=on]").click(); await until(() => $2("stname").textContent === "On", 8000);
  await fetch(B + "/__fail1");
  w2.location.hash = "controller"; await sleep(300);
  const before = await counts();
  $2("restart").click(); $2("mgo").click();
  await until(() => $2("mt").textContent === "Not done", 20000, "failure dialog");
  ok(/did not shut down/.test($2("mb").textContent) && /Nothing was restarted/.test($2("mb").textContent) && !$2("mgo").hidden === false && $2("mcancel").textContent === "Close", "failure: message + Close only: " + $2("mb").textContent);
  const after = await counts();
  ok((after["/api/restart"] || 0) === (before["/api/restart"] || 0) && $2("stname").textContent === "On", "failure: no restart sent, state back On, no force_off");
  $2("mcancel").click(); ok($2("modal").hidden && !$2("restart").disabled, "Close dismisses; controls usable again");
  // cancel during wait
  const b2 = await counts();
  $2("restart").click(); $2("mgo").click(); await until(() => /Waiting/.test($2("mb").textContent), 3000);
  $2("mcancel").click(); ok($2("modal").hidden && $2("pend").hidden, "cancel during wait closes dialog, clears indicator");
  await fetch(B + "/__fail0"); await sleep(9000);
  ok(((await counts())["/api/restart"] || 0) === (b2["/api/restart"] || 0), "cancelled: restart never sent even after BC-250 went off (state " + $2("stname").textContent + ")");
  // update path (needs state On again)
  w2.document.querySelector("[data-act=on]").click; 
  w2.location.hash = "dashboard"; await sleep(200);
  w2.document.querySelector("[data-act=on]").click(); await until(() => $2("stname").textContent === "On", 8000);
  w2.location.hash = "controller"; await sleep(300);
  const fx = []; w2.XMLHttpRequest = function () { const o = { upload: {}, open(m, u) { o.m = m; o.u = u; }, send(f) { o.body = f; fx.push(o); } }; return o; };
  ok($2("upbtn").disabled, "upload disabled without file");
  const NE = "@@" + "BC250FW|"; const u = new Uint8Array(1000); u[0] = 0xE9; u[12] = 5; const mkb = NE + "1.0.9|2026-11-15 03:20:00|@@"; for (let i = 0; i < mkb.length; i++) u[100 + i] = mkb.charCodeAt(i);
  const f = new w2.File([u], "fw.bin");
  Object.defineProperty($2("file"), "files", { value: [f], configurable: true }); $2("file").dispatchEvent(new w2.Event("change"));
  await until(() => !$2("upbtn").disabled, 3000, "install enabled");
  ok(!$2("upbtn").disabled, "install enabled while On with valid file");
  $2("upbtn").click(); ok(!$2("modal").hidden && $2("mgo").textContent === "Shut down and update", "update dialog");
  $2("mgo").click();
  await until(() => $2("mt").textContent === "Updating", 12000, "updating");
  ok(fx.length === 1 && fx[0].m === "POST" && fx[0].u === "/api/update" && fx[0].body.get("firmware").name === "fw.bin", "after off: XHR POST /api/update with field 'firmware' (fake XHR)");
  fx[0].status = 409; fx[0].responseText = '{"ok":false,"error":"not off"}'; fx[0].onload();
  await until(() => $2("mt").textContent === "Not done", 2000);
  ok(/Update refused: not off/.test($2("mb").textContent), "409 on update shows API error in dialog");
  $2("mcancel").click();
  // restart while off: the progress is in the top banner, not in the Update firmware card
  await fetch(B + "/__reset", { method: "POST" });
  const d3 = await open(B + "/"), w3 = d3.window, $3 = id => w3.document.getElementById(id); w3.scrollTo = () => {};
  const type3 = (id, v) => { $3(id).value = v; $3(id).dispatchEvent(new w3.Event("input", { bubbles: true })); };
  await until(() => !$3("gset").hidden, 3000);
  type3("gn1", "flowpass12"); type3("gn2", "flowpass12"); $3("gset").dispatchEvent(new w3.Event("submit", { cancelable: true }));
  await until(() => $3("gate").hidden && $3("stname").textContent === "Off", 4000);
  w3.location.hash = "controller"; await until(() => !$3("restart").disabled, 3000, "restart enabled");
  $3("restart").click(); $3("rconf").querySelectorAll("button")[0].click();
  await until(() => !$3("pend").hidden && /waiting for the controller to come back/.test($3("pend").textContent), 3000, "restart banner");
  ok($3("pend").closest("#banners") && $3("upmsg").textContent === "", "restart while off: progress in the top banner, Update firmware card untouched");
  await until(() => /Back online/.test($3("pend").textContent), 15000, "back online in the banner");
  ok(true, "restart while off ends with 'Back online. Reloading' in the banner");
  // until it restarts, the controller still answers, logged in: that is not "back"
  const d4 = await open(B + "/"), w4 = d4.window, $4 = id => w4.document.getElementById(id); w4.scrollTo = () => {};
  await until(() => !$4("glog").hidden, 3000, "login form");
  $4("gp").value = "flowpass12"; $4("glog").dispatchEvent(new w4.Event("submit", { cancelable: true }));
  await until(() => $4("gate").hidden && $4("stname").textContent === "Off", 4000, "logged in");
  await new Promise(r => setTimeout(r, 2500));   // some uptime before the wait starts
  w4.waitBack();
  await new Promise(r => setTimeout(r, 4000));
  ok(/waiting for the controller to come back/.test($4("pend").textContent), "the old start still answering is not taken as back: " + $4("pend").textContent);
  await w4.fetch("/api/restart", { method: "POST", body: "{}" });   // the restart happens now
  await until(() => /Back online/.test($4("pend").textContent), 4000, "back after the restart");
  ok(true, "back once the controller has started again");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
