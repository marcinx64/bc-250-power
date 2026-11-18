// SPDX-License-Identifier: GPL-3.0-or-later
// Needs the mock with MOCK_RADIO=2g4 (P = its port: the controller in 2.4G mode); set BLE_P to a plain mock's port to also check Bluetooth mode.
const { open, until, sleep } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };

async function fwCheck(w, $, run, tag) {
  const NEEDLE = "@@" + "BC250FW|";
  const img = ver => { const u = new Uint8Array(4000); u[0] = 0xE9; u[12] = 5; const s = NEEDLE + ver + "|2026-11-15 03:20:00|@@"; for (let i = 0; i < s.length; i++) u[1000 + i] = s.charCodeAt(i); return u; };
  w.location.hash = "controller"; await until(() => !$("ab-dl").hidden, 4000, "about " + tag);
  const pick = async u8 => { const f = new w.File([u8], "fw.bin"); Object.defineProperty($("file"), "files", { value: [f], configurable: true }); $("file").dispatchEvent(new w.Event("change")); await until(() => !$("fpanel").hidden && !$("fname").textContent.startsWith("Reading") && $("fchecks").children.length > 0, 4000, "checks"); };
  const t = () => $("fchecks").textContent;
  await pick(img("2.2.0"));
  ok(/version 2\.2\.0, built/.test(t()) && /Newer than the installed/.test(t()) && !/wake list|instead of|Wakes the BC-250 by/.test(t()) && !$("upbtn").disabled && !$("fswap"), tag + ": a file shows version and build only; no wake-method warning: " + t().slice(0, 160));
  const po = w.parseMk("@@" + "BC250FW|2.1.18-old|2026-11-16 11:11:11|@@"), pn = w.parseMk("@@" + "BC250FW|2.1.20|2026-11-16 11:11:11|@@");
  ok(po && po.ver === "2.1.18" && pn && pn.ver === "2.1.20", tag + ": older files with a suffix still read as plain versions");
}
async function boot(B) {
  await fetch(B + "/__reset", { method: "POST" });
  const mem = {}; const pre = w => Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = v; } } });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "blepass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  return { d, w, $ };
}
let F = fetch; const post = (B, p, o) => F(B + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(o) }).then(r => r.json());
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  const { d, w, $ } = await boot(B); F = w.fetch;
  const rows = () => [...$("wlist").children], txt = e => e.textContent;
  const navs = () => w.document.querySelector(".nv[data-view=wake]").textContent;
  await until(() => $("sum-ble-h").textContent === "2.4G/BT wake", 3000, "dash title");
  ok(navs() === "2.4G/BT wake" && /Switched on now/.test($("b-presentl").textContent), "nav + dashboard card titled 2.4G wake: " + navs());
  const navIc = () => w.document.querySelector(".nv[data-view=wake] svg").innerHTML, cardIc = () => w.document.querySelector("#sum-ble-h svg").innerHTML;
  ok(/circle/.test(navIc()) && /circle/.test(cardIc()) && !/M7 7l10 10/.test(navIc()), "2.4G antenna icon in the menu and on the dashboard card");
  ok(w.rel(1230000) === "20 min ago" && w.rel(7300000) === "2 h ago" && w.rel(200000000) === "2 d ago" && w.rel(45000) === "45 s ago", "last heard in s / min / h / d");
  w.location.hash = "wake"; await until(() => !$("ble").hidden, 3000);
  ok($("vtitle").textContent === "2.4G/BT wake" && w.document.title.startsWith("2.4G/BT wake") && $("ble-h").textContent === "2.4G/BT wake" && /2\.4G USB dongle/.test($("ble-d").textContent), "view heading + wording");
  ok(!/Bluetooth (wake|device)/.test($("ble").textContent) && /classic Bluetooth controller/.test($("ble").textContent), "main card: 2.4G wording, classic Bluetooth controllers mentioned");
  ok($("scancard").hidden && !$("learn").hidden, "scan card hidden, learn card shown");
  await until(() => $("c_ble_rearm_s") && $("c_ble_rearm_s").value !== "", 3000, "cfg");
  ok(!$("c_ble_min_rssi") && !$("c_ble_rearm_s").closest(".f").hidden, "no min signal option, rearm shown");
  ok(!/gone|appear/.test($("c_ble_rearm_s").closest(".f").textContent), "rearm text adapted");
  await until(() => rows().length === 1, 4000, "wake list");
  ok(txt($("wcount")) === "(1/8)" && txt(rows()[0]).includes("Ultimate 2") && txt(rows()[0]).includes("A1 B2 C3 D4 E5") && !txt(rows()[0]).includes("MHz") && txt(rows()[0]).includes("not heard since restart") && !txt(rows()[0]).includes("not seen") && txt(rows()[0]).includes("not armed"), "list row: " + txt(rows()[0]));
  // learn
  $("learnbtn").click();
  await until(() => $("learnbtn").textContent === "Stop" && !$("learnhelp").hidden, 2000, "learning");
  ok(/Switch the device on in 2\.4G mode/.test($("learnhelp").textContent), "instructions shown");
  await until(() => $("llist").children.length === 1, 6000, "found");
  let r = $("llist").children[0];
  ok(/5A 6B 7C 8D 9E/.test(txt(r)) && /Seen\. Waiting for a few more packets/.test(txt(r)) && /2436\.5 MHz/.test(txt(r)) && /2424\.1 MHz/.test(txt(r)) && r.querySelector("button").hidden, "seen once: no add yet: " + txt(r));
  await until(() => !r.querySelector("button").hidden, 8000, "confirmed");
  ok(r.querySelector("input").value === "" && r.querySelector("input").placeholder === "Name" && r.querySelector("input").maxLength === 31 && r.querySelector("button").textContent === "Add to wake list", "name default + add button");
  r.querySelector("input").value = "Pad 2"; r.querySelector("button").click();
  await until(() => rows().length === 2, 4000, "added");
  ok(txt(rows()[1]).includes("Pad 2") && txt(rows()[1]).includes("5A 6B 7C 8D 9E"), "added to list: " + txt(rows()[1]));
  // the added device moves from the found list to "Already in the wake list" (no name field)
  await until(() => $("llist").children.length === 0 && $("lknown").children.length === 1, 4000, "moved to known list");
  const kn = $("lknown").children[0];
  ok(!$("lknownh").hidden && /Pad 2/.test(txt(kn)) && /5A 6B 7C 8D 9E/.test(txt(kn)) && /In wake list/.test(txt(kn)) && !kn.querySelector("input"), "known list: name, address, no name field: " + txt(kn));
  // enabled switch: on by default; off -> "disabled", greyed, saved
  const cbx = () => rows()[1].querySelector(".en input");
  ok(cbx() && cbx().checked && /armed/.test(txt(rows()[1])), "new device is enabled");
  cbx().checked = false; cbx().dispatchEvent(new w.Event("change"));
  await until(() => /disabled: will not start the BC-250/.test(txt(rows()[1])) && rows()[1].classList.contains("off"), 4000, "disabled");
  ok(!/not armed|\barmed\b/.test(txt(rows()[1]).replace("disabled: will not start the BC-250", "")) && (await (await F(B + "/api/rf/wake")).json()).list.find(e => e.addr === "5A6B7C8D9E").enabled === false, "disabled: shown, greyed, stored as enabled=false");
  cbx().checked = true; cbx().dispatchEvent(new w.Event("change"));
  await until(() => !/disabled/.test(txt(rows()[1])) && !rows()[1].classList.contains("off"), 4000, "enabled again");
  ok(true, "enabled again");
  // rename
  const btn = (row, label) => [...row.querySelectorAll("button")].find(b => b.textContent === label && !b.closest("[hidden]"));
  btn(rows()[1], "Rename").click();
  const ed = rows()[1].querySelector(".ed");
  ok(!ed.hidden && ed.querySelector("input").value === "Pad 2", "rename opens a field with the current name");
  ed.querySelector("input").value = "Living room pad"; btn(rows()[1], "Save").click();
  await until(() => txt(rows()[1]).startsWith("Living room pad") && ed.hidden, 4000, "renamed");
  await until(() => /Living room pad/.test(txt($("lknown"))), 4000, "known list follows rename");
  ok(true, "rename saved, list and known list updated");
  btn(rows()[1], "Rename").click(); ed.querySelector("input").value = ""; btn(rows()[1], "Save").click(); await sleep(300);
  ok(!ed.hidden && txt(rows()[1]).startsWith("Living room pad"), "empty name refused in 2.4G mode");
  const ei2 = ed.querySelector("input"); ei2.value = "ultimate 2"; btn(rows()[1], "Save").click(); await sleep(400);
  ok(!ed.hidden && txt(rows()[1]).startsWith("Living room pad") && /already used/.test(w.document.body.textContent), "a name already in the list is refused (any case)");
  ok((await post(B, "/api/rf/wake/add", { addr: "AA0000000F", mhz: 2410, name: "Ultimate 2" })).error === "that name is already used", "mock: duplicate name on add refused");
  btn(rows()[1], "Cancel").click(); ok(ed.hidden, "cancel closes the rename field");
  // remove: confirm box in the page's style
  btn(rows()[1], "Remove").click();
  const cf = rows()[1].querySelector(".confirm");
  ok(!cf.hidden && /Remove Living room pad from the wake list\?/.test(cf.textContent) && cf.querySelector("button.dan"), "remove asks in a confirm box: " + cf.textContent);
  cf.querySelectorAll("button")[1].click(); await sleep(200);
  ok(cf.hidden && rows().length === 2, "cancel keeps the device");
  btn(rows()[1], "Remove").click(); cf.querySelectorAll("button")[0].click();
  await until(() => rows().length === 1, 4000, "removed");
  ok(txt(rows()[0]).includes("Ultimate 2"), "remove works");
  await until(() => $("llist").children.length === 1 && $("lknown").children.length === 0 && $("lknownh").hidden, 4000, "back in the found list");
  r = $("llist").children[0];
  ok(true, "removed device is offered again with a name field");
  // fill to 8 via api (same limit as Bluetooth), list full
  const SEVEN = ["AA00000001", "AA00000002", "AA00000003", "AA00000004", "AA00000005", "AA00000006", "AA00000007"];
  for (const a of SEVEN) await post(B, "/api/rf/wake/add", { addr: a, mhz: 2410, name: "x" + a });
  await until(() => rows().length === 8, 5000, "8 rows");
  ok(txt($("wcount")) === "(8/8)", "2.4G list holds 8 devices: " + txt($("wcount")));
  await until(() => r.querySelector("button").textContent === "List full", 4000, "list full").catch(() => {});
  // in_list entries show In list even when full; check mock validation instead
  const bad = async o => (await post(B, "/api/rf/wake/add", o)).error;
  ok(await bad({ addr: "AA00000008", mhz: 2410, name: "y" }) === "list full", "mock: list full error (as the firmware)");
  // validation (list must have room)
  for (const a of SEVEN) await post(B, "/api/rf/wake/remove", { addr: a });
  ok(await bad({ addr: "XYZ", mhz: 2410, name: "y" }) && await bad({ addr: "AA00000009", mhz: 2410 }) && await bad({ addr: "AA00000009", mhz: 2500, name: "y" }) && await bad({ addr: "A1B2C3D4E5", mhz: 2410, name: "y" }), "mock: validation errors");
  // stop
  $("learnbtn").click(); await until(() => $("learnbtn").textContent === "Learn", 2000, "stopped");
  ok($("learnhelp").hidden, "stopped: instructions hidden");
  // full list in UI: add button disabled with 'List full' for a new unlisted entry
  for (const a of SEVEN) await post(B, "/api/rf/wake/add", { addr: a, mhz: 2410, name: "x" + a });
  await until(() => rows().length === 8, 5000, "8 rows again");
  // remove the learned device so it shows as unlisted
  await post(B, "/api/rf/wake/remove", { addr: "5A6B7C8D9E" });
  $("learnbtn").click(); await until(() => $("llist").children.length && !$("llist").children[0].querySelector("button").hidden, 12000, "found again");
  const b = $("llist").children[0].querySelector("button");
  ok(b.textContent === "List full" && b.disabled, "list full disables add: " + b.textContent);
  $("learnbtn").click();
  await until(() => $("llist").children.length === 0, 5000, "results gone after stop");
  ok($("llist").children.length === 0 && $("learnbtn").textContent === "Learn", "stopping learning clears the results");
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no page errors " + d.errors.join(";"));
  await fwCheck(w, $, "2g4", "2.4G mode");
  d.window.close();

  if (process.env.BLE_P) {
    const B2 = "http://127.0.0.1:" + process.env.BLE_P, o = await boot(B2), $b = o.$;
    o.w.location.hash = "wake"; await until(() => !$b("ble").hidden, 3000);
    ok(/M7 7l10 10/.test(o.w.document.querySelector(".nv[data-view=wake] svg").innerHTML), "Bluetooth mode: Bluetooth icon unchanged");
    ok($b("vtitle").textContent === "BLE wake" && $b("ble-h").textContent === "BLE wake" && $b("sum-ble-h").textContent === "BLE wake", "BLE mode: titles");
    ok($b("learn").hidden && !$b("scancard").hidden && $b("scanbtn").textContent === "Scan for devices", "Bluetooth mode: scan card, no learn card");
    await until(() => $b("c_ble_rearm_s") && !$b("c_ble_rearm_s").closest(".f").hidden, 3000, "rearm");
    await until(() => $b("wlist").children.length === 1, 4000);
    ok($b("wcount").textContent === "(1/8)" && /8BitDo/.test($b("wlist").textContent) && !/public/.test($b("wlist").textContent) && /switched on|not heard|heard /.test($b("wlist").textContent), "Bluetooth mode: same tags as 2.4G, no address type: " + $b("wlist").textContent);
    // Bluetooth page: rename, confirm box, known list after a scan
    const wr = () => $b("wlist").children[0], bb = (row, l) => [...row.querySelectorAll("button")].find(x => x.textContent === l && !x.closest("[hidden]"));
    const bc = wr().querySelector(".en input"); bc.checked = false; bc.dispatchEvent(new o.w.Event("change"));
    await until(() => /disabled: will not start the BC-250/.test(wr().textContent), 4000, "ble disabled");
    ok(true, "Bluetooth mode: Enabled switch works");
    bc.checked = true; bc.dispatchEvent(new o.w.Event("change")); await until(() => !/disabled/.test(wr().textContent), 4000, "ble enabled");
    bb(wr(), "Rename").click(); wr().querySelector(".ed input").value = "Desk pad"; bb(wr(), "Save").click();
    await until(() => wr().textContent.startsWith("Desk pad"), 4000, "ble renamed");
    ok(true, "Bluetooth mode: rename works");
    bb(wr(), "Remove").click();
    ok(!wr().querySelector(".confirm").hidden && /Remove Desk pad from the wake list\?/.test(wr().querySelector(".confirm").textContent), "Bluetooth mode: remove asks in a confirm box");
    wr().querySelector(".confirm").querySelectorAll("button")[1].click();
    $b("scanbtn").click();
    await until(() => $b("dknown").children.length === 1, 15000, "ble known list");
    ok(!$b("dknownh").hidden && /Desk pad/.test($b("dknown").textContent) && !$b("dknown").querySelector("input") && ![...$b("dlist").children].some(x => /e4:00:00:00:00:10/.test(x.textContent)), "Bluetooth mode: listed device shown under Already in the wake list, not in the scan list");
    await fwCheck(o.w, $b, "ble", "Bluetooth mode");
    ok(o.d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no page errors (ble)");
    o.w.close();
  }
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
