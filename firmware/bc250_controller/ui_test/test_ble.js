// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); };
  await until(() => !$("gset").hidden, 3000);
  type("gn1", "blepass123"); type("gn2", "blepass123"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  w.location.hash = "wake"; await until(() => !$("scanbtn").closest("section").hidden, 3000);
  ok($("dempty").hidden && $("dhid").textContent === "3 other devices hidden" || $("dhid").textContent === "", "no empty state before any scan");
  ok($("showall").closest("section").dataset.view === "debug" && !$("scanbtn").closest("section").querySelector("#showall"), "switch lives in Debug, not in the Bluetooth card");
  ok($("scanrearm").hidden, "no re-arm note before a scan");
  ok(!$("wcard").classList.contains("folded"), "BLE: list card unfolded by default");
  ok($("scancard").querySelectorAll("input[type=checkbox]").length === 0 && $("dhid").querySelector("*") === null, "no control next to hidden note");
  await until(() => $("wlist").children.length > 0, 4000, "wake list");
  { const r = $("wlist").children[0], sg = r.querySelector(".sig"); ok(sg && /%$/.test(sg.firstChild.style.width) && !/object|dBm/.test(r.textContent) && /\d+%/.test(r.querySelector(".sigp").textContent), "wake list row: signal as one bar with percentage, no stray text: " + r.textContent); }
  await w.fetch(B + "/api/ble/wake/add", { method: "POST", body: JSON.stringify({ addr: "aa:bb:cc:dd:ee:99", type: "public", name: "Away pad" }) });
  await until(() => [...$("wlist").children].some(r => /Away pad/.test(r.textContent)), 6000, "absent device row");
  { const r = [...$("wlist").children].find(r => /Away pad/.test(r.textContent)); ok(!r.querySelector(".sigw") && /not heard since restart/.test(r.textContent) && !/not seen/.test(r.textContent), "absent device: no signal, one presence tag: " + r.textContent); }
  { const r = $("wlist").children[0]; ok(/switched on/.test(r.textContent) && !/heard/.test(r.textContent), "present device: switched on, no heard tag"); }
  ok($("wlist").children[0].querySelector(".ad").textContent === "E4:00:00:00:00:10", "addresses in capitals: " + $("wlist").children[0].querySelector(".ad").textContent);
  $("scanbtn").click();
  await until(() => $("dlist").children.length > 0, 4000, "list");
  ok(!$("scanrearm").hidden && /no device starts the BC-250\. Devices arm again \d+ s after it stops/.test($("scanrearm").textContent) && $("learnrearm").hidden && !w.document.querySelector("#banners > .bn:not([hidden])"), "wake paused: said in the scan card only, no banner: " + $("scanrearm").textContent);
  const names = () => [...$("dlist").children].map(r => r.firstChild.children[0].textContent);
  const labels = () => [...$("dlist").children].map(r => r.firstChild.children[2].textContent);
  // 8BitDo is in the wake list: shown under "Already in the wake list", not here
  ok(names().join("|") === "unnamed|Stick" && /8BitDo/.test($("dknown").textContent) && !$("dknownh").hidden, "default: only controller-like, sorted ctl-first then strongest; listed one apart: " + names());
  ok(labels().join("|") === "Input device|Joystick", "kind labels: " + labels());
  ok(!$("dlist").children[0].querySelector("svg"), "no gamepad icon on an input device");
  ok($("dhid").textContent === "3 other devices hidden", "hidden count: " + $("dhid").textContent);
  ok($("dempty").hidden && /Scanning/.test($("scanmsg").textContent) && $("scanbtn").textContent === "Stop" && !$("scanhelp").hidden, "while scanning: Stop button, help text, no empty state: " + $("scanmsg").textContent);
  { const sg = $("dlist").children[0].querySelector(".sig"); ok(sg && sg.children.length === 1 && /%$/.test(sg.firstChild.style.width) && /\b(good|fair|weak)\b/.test(sg.className) && /^\d+%$/.test(sg.nextSibling.textContent) && !sg.title && !sg.parentNode.title && !/dBm/.test($("dlist").textContent), "signal shown as one bar with its percentage, no hover text"); }
  ok($("dnote").hidden, "no debug note while filtering");
  w.location.hash = "debug"; await until(() => !$("showall").closest("section").hidden, 3000);
  $("showall").checked = true; $("showall").dispatchEvent(new w.Event("change"));
  ok($("dbg").hidden, "show-all switch does not raise the spoofing banner");
  w.location.hash = "wake"; await until(() => !$("scanbtn").closest("section").hidden, 3000);
  ok(!$("dnote").hidden && $("dnote").querySelector("a").getAttribute("href") === "#debug", "note + link to Debug when show-all is on");
  const rs = [...$("dlist").children].map(r => parseInt(r.querySelector(".sigp").textContent));
  ok(names().slice(0, 2).join("|") === "unnamed|Stick" && names().length === 5 && rs[2] >= rs[3] && rs[3] >= rs[4], "show all: controllers first, rest strongest first: " + names() + " " + rs);
  ok(labels().slice(2).sort().join("|") === "Keyboard|Other|Other" && labels()[names().indexOf("Headset <b>x</b>")] === "Other", "legacy entry (no new fields) labelled Other: " + labels());
  ok(!$("dlist").querySelector("b") && $("dhid").textContent === "", "names via textContent, no hidden text");
  ok(mem.bc250_showall === "1", "switch stored");
  $("scanbtn").click();
  await until(() => $("dlist").children.length === 0 && $("dknown").children.length === 0 && $("scanbtn").textContent === "Scan for devices" && $("scanhelp").hidden, 4000, "stop clears");
  ok(true, "Stop ends the scan and its results go (as 2.4G Learn)");
  // reload persists: new page, same storage; session cookie is new so log in
  const d2 = await open(B + "/", pre), w2 = d2.window, $2 = id => w2.document.getElementById(id); w2.scrollTo = () => {};
  ok($2("showall").checked === true, "switch state restored after reload");
  // empty state (mock with no controller-like devices)
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
