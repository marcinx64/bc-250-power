// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
const NOHOLD = !!process.env.NOHOLD;
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
const last = async () => (await (await fetch(B + "/__last")).json());
const NE = "@@" + "BC250FW|";
const img = () => { const u = new Uint8Array(1000); u[0] = 0xE9; u[12] = 5; const s = NE + "1.0.9|2026-11-15 03:20:00|@@"; for (let i = 0; i < s.length; i++) u[100 + i] = s.charCodeAt(i); return u; };
const mem = {};
const pre = w => { Object.defineProperty(w, "localStorage", { value: { getItem: () => null, setItem() {} } }); Object.defineProperty(w, "sessionStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; }, removeItem: k => { delete mem[k]; } } }); };
async function page(first) {
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  if (first) { await until(() => !$("gset").hidden, 3000); $("gn1").value = $("gn2").value = "holdpass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true })); }
  else { await until(() => !$("glog").hidden, 3000); $("gp").value = "holdpass123"; $("glog").dispatchEvent(new w.Event("submit", { cancelable: true })); }
  await until(() => $("gate").hidden && $("stname").textContent !== "…", 5000);
  const go = async v => { w.location.hash = v; await until(() => !w.document.querySelector(`main>section[data-view=${v}]:not([hidden])`) === false || true, 500); await sleep(200); };
  return { d, w, $, go };
}
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const { w, $, go } = await page(true);
  const T = () => [...$("toasts").children], clear = () => $("toasts").replaceChildren();
  const sw = () => { };
  w.document.querySelector("[data-act=on]").click(); await until(() => $("stname").textContent === "On", 8000);
  if (NOHOLD) {
    await go("controller"); $("restart").click(); ok(!$("modal").hidden && /cannot restart/.test($("mt").textContent) && true, "no hold field: classic shut-down-first modal");
    $("mcancel").click(); ok(/cuts power to the BC-250/.test($("fwnote").textContent), "fallback note keeps the old wording");
    console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
  }
  const fileText = () => $("fwnote").textContent + " " + $("rnote").textContent;
  await go("debug");
  ok(!w.document.getElementById("holdsw") && [...w.document.querySelectorAll("main>section[data-view=debug]")].length === 2, "Debug view: only Bluetooth scan + sense cards");
  await go("controller"); await sleep(1300);
  ok(!/experimental|unproven|BIOS/i.test(w.document.body.textContent) && !w.document.querySelector(".holdtag"), "no experimental/unproven wording or tag");
  ok(/power state is unaffected; the controller is unavailable/.test($("fwnote").textContent) && /power state is unaffected; the controller is unavailable/.test($("rnote").textContent), "Update firmware and Restart cards: power state unaffected");
  const c0 = await counts();
  $("restart").click();
  ok(!$("modal").hidden && $("mt").textContent === "Restart the controller?" && $("mgo").textContent === "Restart" && !$("mcancel").hidden && !w.document.getElementById("msec") && /The BC-250\u2019s power state is unaffected/.test($("mb").textContent) && /do not respond for a few seconds/.test($("mb").textContent), "on+hold: plain confirm: " + $("mb").textContent);
  $("mcancel").click(); ok($("modal").hidden && (await counts())["/api/restart"] === undefined, "Cancel sends nothing");
  // 409 shown in modal: device setting turned off right before confirming
  $("restart").click(); await w.fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: '{"pson_hold":false}' });
  $("mgo").click(); await until(() => $("mt").textContent === "Not done", 3000);
  ok(/Restart refused: Power the BC-250 off before restarting/.test($("mb").textContent), "409 text shown in the modal"); $("mcancel").click();
  await w.fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: '{"pson_hold":true}' }); await sleep(1300);
  // install with hold (fake XHR)
  const fx = []; w.XMLHttpRequest = function () { const o = { upload: {}, open(m, u) { o.m = m; o.u = u; }, send() { fx.push(o); } }; return o; };
  const f = new w.File([img()], "fw.bin"); Object.defineProperty($("file"), "files", { value: [f], configurable: true }); $("file").dispatchEvent(new w.Event("change"));
  await until(() => !$("upbtn").disabled, 3000);
  $("upbtn").click(); ok($("mt").textContent === "Install 1.0.9?" && $("mgo").textContent === "Install", "install dialog names version: " + $("mt").textContent);
  ok(fx.length === 0, "nothing uploaded yet"); $("mgo").click(); await until(() => fx.length === 1, 2000);
  ok(fx[0].u === "/api/update" && (await counts())["/api/power"] === c0["/api/power"], "Install -> upload directly, no shutdown requested");
  fx[0].status = 200; fx[0].responseText = '{"ok":true}'; fx[0].onload();
  ok(mem.bc250_keepon === "1" && mem.bc250_expect === "1.0.9", "expectations remembered (keep-on + version)");
  // simulate the reload: new page, same storage
  await w.fetch("/api/restart", { method: "POST" }); mem.bc250_expect = "1.0.0-mock";   // fake XHR never hit the mock: do the boot server-side
  const p2 = await page(false); const T2 = () => [...p2.$("toasts").children];
  await until(() => T2().length >= 1, 3000);
  ok(T2().length === 1 && T2()[0].classList.contains("ok") && /Controller restarted, BC-250 kept on\. Now running 1\.0\.0-mock/.test(T2()[0].textContent), "after reload: one success toast with version: " + T2().map(t => t.textContent));
  ok(mem.bc250_keepon === undefined, "keep-on expectation cleared");
  // restart with keep-on, then failure
  p2.w.location.hash = "controller"; await sleep(1300);
  await until(() => !p2.$("ab-dl").hidden, 4000); ok(/\(BC-250 kept on\)/.test(p2.$("ab-rst").textContent), "About: Started after ... (BC-250 kept on): " + p2.$("ab-rst").textContent);
  p2.$("restart").click(); ok(p2.$("mt").textContent === "Restart the controller?", "dialog again");
  await fetch(B + "/__failresume1"); p2.$("mgo").click(); await until(() => mem.bc250_keepon === "1", 3000);
  ok((await counts())["/api/restart"] >= 1, "restart request sent");
  const p3 = await page(false); const T3 = () => [...p3.$("toasts").children];
  await until(() => T3().length >= 1, 3000);
  ok(T3().length === 1 && T3()[0].classList.contains("err") && /The BC-250 was not kept on/.test(T3()[0].textContent), "resume failed: one error toast, none while it was undecided: " + T3().map(t => t.textContent));
  // Started after: the outcome, not just "it was a resume"
  p3.w.location.hash = "controller"; await sleep(1300);
  await until(() => !p3.$("ab-dl").hidden, 4000);
  ok(/\(BC-250 not kept on\)/.test(p3.$("ab-rst").textContent) && !/\(BC-250 kept on\)/.test(p3.$("ab-rst").textContent) && p3.$("ab-rst").className === "warn", "About: Started after ... (BC-250 not kept on), as a warning: " + p3.$("ab-rst").textContent);
  const keepS = p3.w.S; p3.w.S = null; const rr = j => p3.w.rrInfo(Object.assign({ reset_reason: "restart from the web page", resumed: true }, j));
  ok(/\(checking whether the BC-250 stayed on\)$/.test(rr({ resume: "pending" }).t) && !rr({ resume: "pending" }).w && /\(BC-250 kept on\)$/.test(rr({ resume: "kept" }).t) && /\(BC-250 not kept on\)$/.test(rr({ resume: "lost" }).t) && rr({ resume: "lost" }).w && /\(BC-250 kept on\)$/.test(rr({}).t), "Started after: pending / kept / lost, and older firmware without the outcome");
  p3.w.S = keepS;
  await fetch(B + "/__failresume0");
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
