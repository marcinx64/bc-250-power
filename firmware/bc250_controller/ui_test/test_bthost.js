// SPDX-License-Identifier: GPL-3.0-or-later
// 2.4G mode with Bluetooth hosts: 2.4G devices and Bluetooth hosts in separate lists, each with
// its own Learn button (one learns at a time); hosts are added without a frequency, by Learn or typed in;
// at most 2 hosts. Mock: MOCK_RADIO=2g4 on port P.
const { open, until } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8793);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/#wake", w => { w.scrollTo = () => {}; }), w = d.window, $ = id => w.document.getElementById(id);
  const txt = e => e.textContent.replace(/\s+/g, " ");
  const rows = id => [...$(id).children];
  const rfList = async () => (await w.fetch(B + "/api/rf/wake")).json().then(j => j.list);
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "bthostpass1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && !$("hcard").hidden && !$("learn").hidden, 6000, "cards");

  // layout: two lists, two Learn cards
  await until(() => $("wcount").textContent !== "", 6000, "list loaded");
  ok(/any device that calls a listed host wakes the BC-250/.test($("hcard").querySelector(".fbody p").textContent), "hosts card: the note below its heading");
  ok($("wtitle").textContent === "2.4G Dongle devices" && /\(1\/8\)/.test($("wcount").textContent), "2.4G device list: title and count " + $("wcount").textContent);
  ok(/Bluetooth Classic hosts/.test($("hcard").querySelector("h2").textContent) && $("hwcount").textContent === "(0/2)" && !$("hwempty").hidden, "host list: empty, limit 2");
  ok(rows("wlist").length === 1 && /A1 B2 C3 D4 E5/.test(txt(rows("wlist")[0])), "the dongle stays in the 2.4G list");
  ok($("learn").parentNode === $("wlist").parentNode && $("hlearn").closest("section") === $("hcard"), "each Learn is in its list's card");
  ok(/Learn a 2\.4G Dongle device/.test(txt($("learn"))) && /Learn a Bluetooth Classic host/.test(txt($("hlearn"))), "Learn sections inside their list cards");

  // fold: 2.4G/BT starts with both cards folded; the heading or anywhere on a folded card toggles;
  // remembered per wake method in this browser
  const fb = $("hcard").querySelector("h2 button.fold"), body = $("hcard").querySelector(".fbody>div"), shown = () => w.getComputedStyle(body).visibility !== "hidden";
  ok(fb && $("wcard").querySelector("h2 button.fold"), "^ button in both list card headings");
  ok($("hcard").classList.contains("folded") && $("wcard").classList.contains("folded") && fb.getAttribute("aria-expanded") === "false" && !shown(), "2.4G/BT: both cards folded by default");
  ok(body.contains($("hwlist")) && body.contains($("hlearn")) && !body.contains($("hcard").querySelector("h2")), "the body (list and Learn) folds, the heading stays");
  $("hcard").querySelector("h2 span").click();
  ok(!$("hcard").classList.contains("folded") && shown() && fb.getAttribute("aria-label") === "Hide", "a click on the heading text unfolds");
  ok(JSON.parse(w.localStorage.getItem("bc250_fold"))["2g4:hcard"] === false, "remembered for 2.4G/BT in this browser");
  fb.click();
  ok($("hcard").classList.contains("folded"), "the ^ button folds");
  $("hcard").click();
  ok(!$("hcard").classList.contains("folded"), "a click anywhere on a folded card unfolds");
  $("hwempty").click();
  ok(!$("hcard").classList.contains("folded"), "a click in the open body does not fold");
  fb.click();   // folded again: Learn opens it, without saving that
  // Bluetooth Learn: only hosts, the other Learn button is disabled meanwhile
  $("hlearnbtn").click();
  ok(!$("hcard").classList.contains("folded") && JSON.parse(w.localStorage.getItem("bc250_fold"))["2g4:hcard"] === true, "Learn unfolds its card (not saved)");
  await until(() => [...$("toasts").children].some(t => t.classList.contains("info") && /^Learning started/.test(t.textContent)), 3000, "learning started toast");
  ok(!$("hlearnrearm").hidden && /Devices arm again/.test($("hlearnrearm").textContent) && $("learnrearm").hidden, "re-arm note in the host Learn card only: " + $("hlearnrearm").textContent);
  ok($("hlearnbtn").textContent === "Stop" && $("learnbtn").disabled && !$("hlearnhelp").hidden && $("learnhelp").hidden, "host Learn running, 2.4G Learn disabled");
  await until(() => rows("hllist").length === 1, 6000, "host found");
  ok(rows("llist").length === 0, "nothing in the 2.4G Learn list");
  const row = () => rows("hllist")[0];
  ok(/…:9A:8B:7C/.test(txt(row())) && !/MHz/.test(txt(row())), "host row: partial address, no frequency: " + txt(row()));
  await until(() => !row().querySelector("button").hidden, 6000, "host ready");
  ok(/A controller is calling this host/.test(txt(row())), "ready host explains what it is");
  ok(row().querySelector("input").placeholder === "BC-250" && row().querySelector("input").value === "", "name field: BC-250 as the default, shown as a placeholder");
  row().querySelector("input").dispatchEvent(new w.KeyboardEvent("keydown", { key: "Enter", bubbles: true }));   // Enter adds; empty name: the default
  await until(() => rows("hwlist").some(r => /BC-250/.test(txt(r))), 4000, "added");
  const e1 = (await rfList()).find(e => e.name === "BC-250");
  ok(e1 && e1.addr === "9A8B7C" && e1.kind === "bt" && !("mhz" in e1), "stored as a Bluetooth host, LAP only: " + JSON.stringify(e1));
  ok(rows("wlist").length === 1 && $("hwcount").textContent === "(1/2)", "host in the host list only");
  await until(() => rows("hlknown").some(r => /BC-250/.test(txt(r))), 4000, "host in the known list");
  $("hlearnbtn").click();
  await until(() => $("hlearnbtn").textContent === "Learn" && !$("learnbtn").disabled, 3000, "stopped");
  await until(() => [...$("toasts").children].some(t => t.classList.contains("info") && /^Learning stopped/.test(t.textContent)), 3000, "learning stopped toast");
  ok(rows("hllist").length === 0 && rows("hlknown").length === 0 && $("hlearnrearm").hidden, "results and the re-arm note gone after stop");

  // the bluetoothctl command: its own box below the text, with a copy button (plain HTTP: textarea copy)
  ok($("bth-cmd").textContent === "bluetoothctl show" && $("bth-cmd").closest(".cmd").contains($("bth-cp")) && !/bluetoothctl/.test($("bth-cmd").closest(".cmd").previousElementSibling.textContent), "command in its own box with a copy button, not inside the text");
  let copied = null; w.document.execCommand = c => { const ta = w.document.querySelector("body>textarea"); copied = c === "copy" && ta ? ta.value : null; return true; };
  $("bth-cp").click();
  await until(() => [...$("toasts").children].some(e => /Copied: bluetoothctl show/.test(e.textContent)), 3000, "copied banner");
  ok(copied === "bluetoothctl show", "copies the command: " + copied);

  // 2.4G Learn: only dongle devices
  $("learnbtn").click();
  ok($("hlearnbtn").disabled, "host Learn disabled while 2.4G Learn runs");
  await until(() => rows("llist").length === 1, 6000, "dongle found");
  ok(rows("hllist").length === 0 && /5A 6B 7C 8D 9E/.test(txt(rows("llist")[0])), "2.4G Learn shows the dongle only");
  $("toasts").replaceChildren(); await w.fetch(B + "/__scanend");   // time up
  await until(() => $("learnbtn").textContent === "Learn", 6000, "learning ended");
  await until(() => [...$("toasts").children].some(t => t.classList.contains("info") && /^Learning stopped/.test(t.textContent)), 3000, "learning stopped toast after time up");

  // typed in: full Bluetooth address, LAP kept; at most 2 hosts
  $("bth-a").value = "12:34"; $("bth-b").click();
  ok(/XX:XX/.test($("bth-e").textContent), "bad address rejected in the page");
  ok($("bth-n").placeholder === "BC-250-2", "typed host: next free default name " + $("bth-n").placeholder);
  $("bth-a").value = "a0:b1:c2:d3:e4:f5"; $("bth-n").value = "Second host";
  $("bth-n").dispatchEvent(new w.KeyboardEvent("keydown", { key: "Enter", bubbles: true }));   // Enter adds
  await until(() => rows("hwlist").some(r => /Second host/.test(txt(r))), 4000, "typed host added");
  const e2 = (await rfList()).find(e => e.name === "Second host");
  { const r = rows("hwlist").find(r => /Second host/.test(txt(r))), tags = [...r.querySelectorAll(".tag")].map(t => t.textContent);
    ok(r.querySelector(".ad").textContent === "…:D3:E4:F5", "typed host shown like a learned one, in capitals: " + r.querySelector(".ad").textContent);
    ok(/armed/.test(tags[0]) && /heard/.test(tags[1]), "armed tag first: " + tags.join(" | ")); }
  ok(/…:9A:8B:7C/.test(txt(rows("hwlist").find(r => /BC-250/.test(txt(r))))), "a learned host shows the part it heard");
  ok(e2 && e2.addr === "D3E4F5" && e2.kind === "bt", "typed address kept as the 3 bytes a controller sends: " + JSON.stringify(e2));
  ok($("bth-a").value === "" && $("hwcount").textContent === "(2/2)", "form cleared, list at 2/2");
  $("bth-a").value = "11:22:33:44:55:66"; $("bth-b").click();
  ok(/full/.test($("bth-e").textContent), "a third host is refused in the page: " + $("bth-e").textContent);
  const r3 = await (await w.fetch(B + "/api/rf/wake/add", { method: "POST", body: JSON.stringify({ addr: "445566", name: "Third" }) })).json();
  ok(!r3.ok && /full/.test(r3.error), "and by the controller: " + JSON.stringify(r3));

  // remove a host
  const hr = rows("hwlist").find(r => /Second host/.test(txt(r)));
  [...hr.querySelectorAll("button")].find(b => b.textContent === "Remove").click();
  hr.querySelector(".confirm button.dan").click();
  await until(() => !rows("hwlist").some(r => /Second host/.test(txt(r))), 4000, "removed");
  ok($("hwcount").textContent === "(1/2)", "host removed");
  w.location.hash = "dashboard";
  const dt = id => $(id).textContent;
  await until(() => dt("b-hosts") === "1 host", 4000, "dashboard host count");
  ok(dt("b-countl") === "2.4G Dongle devices" && dt("b-count") === "1 device" && !$("b-hosts").parentElement.hidden, "dashboard: 2.4G devices and Bluetooth hosts on their own lines: " + dt("b-count") + " / " + dt("b-hosts"));
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no script errors " + d.errors.join(" | "));
  w.close();
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
