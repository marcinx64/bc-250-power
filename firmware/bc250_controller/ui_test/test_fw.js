// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
const NEEDLE = "@@" + "BC250FW|";
function img(o = {}) {
  const n = o.size ?? 4000, u = new Uint8Array(n);
  if (n) { u[0] = o.magic ?? 0xE9; if (n > 13) { u[12] = o.chip ?? 5; u[13] = 0; } }
  const put = (off, s) => { for (let i = 0; i < s.length; i++) u[off + i] = s.charCodeAt(i); };
  if (o.decoy) put(500, NEEDLE + "zz.9.x|not a date|@@" + NEEDLE + "1.2|2026-13-45 99:99:99|@@");
  if (o.ver) put(1000, NEEDLE + o.ver + "|" + (o.build ?? "2026-11-15 03:20:00") + "|@@");
  return u;
}
(async () => {
  const src = await (await fetch(B + "/")).text();
  ok(!src.includes("BC250FW|") && !src.includes("@@BC250") && !src.includes(NEEDLE), "index.html source does not contain the marker needle");
  await fetch(B + "/__reset", { method: "POST" });
  const mem = { bc250_expect: "1.0.0-mock" }; const pre = w => { Object.defineProperty(w, "localStorage", { value: { getItem: () => null, setItem() {} } }); Object.defineProperty(w, "sessionStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; }, removeItem: k => { delete mem[k]; } } }); };
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); };
  await until(() => !$("gset").hidden, 3000);
  type("gn1", "fwpass1234"); type("gn2", "fwpass1234"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  ok([...$("toasts").children].some(t => /Now running 1\.0\.0-mock/.test(t.textContent)) && mem.bc250_expect === undefined, "after update: 'Now running <version>' toast, expectation cleared");
  w.location.hash = "controller"; await until(() => !$("about").hidden && !$("ab-dl").hidden, 4000);
  const fx = []; w.XMLHttpRequest = function () { const o = { upload: {}, open(m, u) { o.m = m; o.u = u; }, send(f) { o.body = f; fx.push(o); } }; return o; };
  const pick = async (u8, name = "fw.bin") => { const f = new w.File([u8], name); Object.defineProperty($("file"), "files", { value: [f], configurable: true }); $("file").dispatchEvent(new w.Event("change")); await until(() => !$("fpanel").hidden && !$("fname").textContent.startsWith("Reading"), 3000, "analysed " + name); return f; };
  const marks = () => [...$("fchecks").children].map(li => li.children[0].textContent + ":" + li.children[1].textContent);
  const mk = (st) => marks().filter(x => x.startsWith(st));
  // newer
  await pick(img({ ver: "1.0.9" }));
  ok(/fw\.bin/.test($("fname").textContent) && /3 KB|4 KB/.test($("fname").textContent), "name + size shown: " + $("fname").textContent);
  ok(marks().length === 5 && mk("Error").length === 0 && marks().some(x => /Newer than the installed version \(1\.0\.8 → 1\.0\.9\)/.test(x)) && marks().some(x => /version 1\.0\.9, built 2026-11-15 03:20:00/.test(x)) && marks().some(x => /Valid ESP/.test(x)) && marks().some(x => /ESP32-C3/.test(x)) && marks().some(x => /Fits the update slot \(0 % of 1\.88 MB\)/.test(x)), "newer file: all checks ok: " + marks().join(" | "));
  ok(!$("upbtn").disabled && $("fany").hidden && (await counts())["/api/update"] === undefined, "Install enabled; nothing sent yet");
  // same / older
  await pick(img({ ver: "1.0.8", build: "2026-11-15 02:53:59" })); ok(marks().some(x => /^Note:Same version as installed \(1\.0\.8\)\. Same build time/.test(x)) && !$("upbtn").disabled, "same version + same build: " + mk("Note"));
  await pick(img({ ver: "1.0.8", build: "2026-11-16 09:00:00" })); ok(marks().some(x => /built 2026-11-16 09:00:00; the installed one 2026-11-15 02:53:59/.test(x)), "same version, rebuild distinguishable");
  await pick(img({ ver: "1.0.7" })); ok(mk("Warning").length === 1 && /Older/.test(mk("Warning")[0]) && !$("upbtn").disabled && $("fany").hidden, "older: warning, Install allowed, no checkbox");
  await pick(img({ ver: "1.0.10" })); ok(marks().some(x => /Newer/.test(x)), "numeric compare 1.0.10 > 1.0.8");
  // no marker
  await pick(img({})); ok(mk("Warning").some(x => /does not identify itself as BC-250 controller firmware/.test(x)) && !$("fany").hidden && $("upbtn").disabled && /Install anyway/.test($("freason").textContent), "no marker: warning, Install disabled, gate shown");
  $("anyway").checked = true; $("anyway").dispatchEvent(new w.Event("change")); ok(!$("upbtn").disabled && $("freason").textContent === "", "'Install anyway' enables Install");
  await pick(img({})); ok($("anyway").checked === false && $("upbtn").disabled, "new file resets the 'Install anyway' tick");
  // blocking errors
  await pick(img({ ver: "1.0.9", chip: 9 })); ok(mk("Error").length === 1 && /chip id 9/.test(mk("Error")[0]) && $("upbtn").disabled && /Install is disabled: Not built/.test($("freason").textContent), "wrong chip blocks");
  await pick(img({ ver: "1.0.9", magic: 0x7f })); ok(/not 0xE9|first byte/.test(mk("Error").join()) && $("upbtn").disabled, "bad magic blocks");
  await pick(img({ ver: "1.0.9", size: 2000000 })); ok(/Too large/.test(mk("Error").join()) && $("upbtn").disabled, "too large blocks");
  await pick(new Uint8Array(0)); ok(/empty/.test(mk("Error").join()) && $("upbtn").disabled, "empty blocks");
  // decoy
  await pick(img({ ver: "1.0.9", decoy: true })); ok(marks().some(x => /version 1\.0\.9, built 2026-11-15 03:20:00/.test(x)) && mk("Error").length === 0, "decoy needles with garbage fields skipped, real marker used");
  await pick(img({ decoy: true })); ok($("fany").hidden === false, "only decoys: treated as no marker");
  // clear
  await pick(img({ ver: "1.0.9" })); $("fclear").click(); ok($("fpanel").hidden && $("upbtn").disabled, "Clear resets panel");
  ok((await counts())["/api/update"] === undefined, "still nothing sent to /api/update");
  // install in off state: direct upload
  const f = await pick(img({ ver: "1.0.9" })); $("upbtn").click();
  await until(() => fx.length === 1, 2000, "xhr");
  ok(fx[0].m === "POST" && fx[0].u === "/api/update" && fx[0].body.get("firmware").name === "fw.bin", "Install (state off) uploads directly");
  fx[0].status = 200; fx[0].responseText = '{"ok":true}'; fx[0].onload(); ok(mem.bc250_expect === "1.0.9", "expected version remembered in sessionStorage");
  // on-state modal names version
  d.window.close();
  await fetch(B + "/__reset", { method: "POST" });
  const d2 = await open(B + "/", pre), w2 = d2.window, $2 = id => w2.document.getElementById(id); w2.scrollTo = () => {}; delete mem.bc250_expect;
  await until(() => !$2("gset").hidden, 3000);
  $2("gn1").value = $2("gn2").value = "fwpass1234"; $2("gset").dispatchEvent(new w2.Event("submit", { cancelable: true }));
  await until(() => $2("gate").hidden && $2("stname").textContent === "Off", 4000);
  w2.document.querySelector("[data-act=on]").click(); await until(() => $2("stname").textContent === "On", 8000);
  w2.location.hash = "controller"; await until(() => !$2("about").hidden && !$2("ab-dl").hidden, 4000);
  const fx2 = []; w2.XMLHttpRequest = function () { const o = { upload: {}, open() {}, send() { fx2.push(o); } }; return o; };
  const file2 = new w2.File([img({ ver: "1.0.9" })], "new.bin"); Object.defineProperty($2("file"), "files", { value: [file2], configurable: true }); $2("file").dispatchEvent(new w2.Event("change"));
  await until(() => !$2("upbtn").disabled, 3000, "enabled in On");
  $2("upbtn").click(); ok(!$2("modal").hidden && /install version 1\.0\.9/.test($2("mb").textContent) && $2("mgo").textContent === "Shut down and update", "modal names the version: " + $2("mb").textContent);
  ok(fx2.length === 0, "nothing uploaded while modal is open"); $2("mcancel").click();
  ok($2("fpanel").hidden === false, "file stays selected after cancel");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
