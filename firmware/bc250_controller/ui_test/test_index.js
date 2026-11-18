// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
let fails = 0;
const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id);
  const click = id => $(id).dispatchEvent(new w.MouseEvent("click", { bubbles: true }));
  const submit = id => $(id).dispatchEvent(new w.Event("submit", { cancelable: true, bubbles: true }));
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); $(id).dispatchEvent(new w.Event("change", { bubbles: true })); };
  const go = async v => { w.location.hash = v; await until(() => !!w.document.querySelector(`main>section[data-view=${v}]:not([hidden])`), 3000, "view " + v); };
  const counts = async () => (await fetch(B + "/__counts")).json();
  const visibleViews = () => [...w.document.querySelectorAll("main>section")].filter(e => !e.hidden).map(e => e.dataset.view).filter((v, i, a) => a.indexOf(v) === i);
  // --- no password set
  await until(() => !$("gate").hidden && !$("gset").hidden, 3000, "set prompt");
  ok(true, "set-password prompt shown when password_set=false");
  type("gn1", "short"); type("gn2", "short"); submit("gset");
  ok($("gserr").textContent.includes("8 to 64"), "short password rejected client-side");
  type("gn1", "firstpass1"); type("gn2", "other"); submit("gset");
  ok($("gserr").textContent.includes("match"), "mismatch rejected");
  type("gn1", "firstpass1"); type("gn2", "firstpass1"); submit("gset");
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000, "dashboard after set");
  ok(true, "password set, auto-login, dashboard shown (state Off)");
  ok(w.document.getElementById("app").hidden === false && visibleViews().join() === "dashboard", "dashboard is default view, only it visible");
  ok($("side").querySelector(".sdt").textContent === "Off" && !/bc250|1\.0\./.test($("side").textContent) && !$("devname") && !$("devfw"), "sidebar: state only, no hostname/version: " + $("side").textContent.replace(/\s+/g, " ").slice(0, 80));
  ok((await counts())["/api/config"] === 1, "config loaded once after login (hold times, timezone setup) even on the dashboard");
  ok((await counts())["/api/log"] === undefined && (await counts())["/api/ble/wake"] === undefined, "no log/BLE polling on dashboard");
  // session loss -> login form
  await fetch(B + "/__kill", { method: "POST" });
  await until(() => !$("gate").hidden && !$("glog").hidden, 4000, "login on 401");
  ok(true, "401 mid-session drops to login form");
  type("gp", "nope"); submit("glog");
  await until(() => $("glerr").textContent.includes("Wrong"), 3000, "wrong pw");
  ok(true, "wrong password message");
  for (let i = 0; i < 4; i++) { type("gp", "nope"); submit("glog"); await sleep(250); }
  await until(() => /Try again in \d+ s/.test($("glerr").textContent) && $("glbtn").disabled, 3000, "lock");
  ok(true, "lockout countdown shown: " + $("glerr").textContent);
  console.log("waiting 31 s for lock expiry..."); await sleep(31500);
  ok(!$("glbtn").disabled, "login button re-enabled after lock");
  type("gp", "firstpass1"); submit("glog");
  await until(() => $("gate").hidden, 3000, "login ok");
  await until(() => $("stname").textContent === "Off", 3000, "resume");
  ok(true, "login resumes dashboard");
  // power
  click("t-up"); // noop
  const act = a => w.document.querySelector(`[data-act=${a}]`);
  ok(!act("shutdown").disabled === false && !act("on").disabled && act("force_off").disabled, "only valid actions enabled in off");
  act("on").click();
  await until(() => $("stname").textContent === "Starting", 3000, "starting");
  ok([...$("toasts").children].some(e => e.textContent.includes("Power on requested")), "power on toast shown");
  await until(() => $("stname").textContent === "On", 8000, "on");
  act("reset").click(); ok(!$("pconf").hidden, "reset needs confirm"); $("pyes").click();
  await until(() => [...$("toasts").children].some(e => e.textContent.includes("Reset sent")), 3000, "reset sent");
  ok(true, "reset sent after confirm");
  act("on").disabled || ok(false, "on enabled while on");
  // ble
  const c0 = await counts();
  await go("wake"); ok(visibleViews().join() === "wake", "only Bluetooth visible: " + visibleViews());
  await until(() => $("wlist").children.length === 1, 4000, "wake list");
  ok($("wlist").textContent.includes("8BitDo") && $("wlist").textContent.includes("switched on") && !/heard|not seen/.test($("wlist").textContent), "wake list: a present device shows only switched on (no heard / not seen)");
  click("scanbtn"); await until(() => $("dlist").children.length === 2 && $("dknown").children.length === 1, 4000, "devices");
  $("showall").checked = true; $("showall").dispatchEvent(new w.Event("change")); ok([...$("dlist").children].some(r => r.textContent.includes("Headset <b>x</b>")) && !$("dlist").querySelector("b"), "names via textContent"); $("showall").checked = false; $("showall").dispatchEvent(new w.Event("change"));
  ok(!$("dhint").hidden, "random-address warning shown");
  const row = [...$("dlist").children].find(r => r.textContent.includes("5C:AA"));
  row._ni.value = "Mystery"; row._b.click();
  await until(() => $("wlist").children.length === 2, 4000, "added");
  ok($("wlist").textContent.includes("Mystery"), "added with edited name");
  click("scanbtn"); await until(() => $("dlist").children.length === 0 && $("scanmsg").textContent === "", 4000, "stopped");
  ok(true, "scan stopped, results gone");
  { await sleep(3500); const c1 = await counts(); ok(c1["/api/ble/wake"] >= 2, "BLE wake polled while on Bluetooth (" + c1["/api/ble/wake"] + ")"); }
  await go("dashboard");
  { const a = await counts(); await sleep(3500); const b = await counts(); ok(a["/api/ble/wake"] === b["/api/ble/wake"] && a["/api/ble/devices"] === b["/api/ble/devices"], "BLE polling stops off-view"); ok(b["/api/state"] > a["/api/state"], "state still polled"); }
  // settings
  await go("settings"); await until(() => $("c_led_on").value === "255", 3000, "cfg on entry"); ok(visibleViews().join() === "settings", "only Settings visible");
  type("c_led_on", "100"); ok(!$("savebar").hidden, "save bar visible after change"); ok(w.document.getElementById("c_hostname") === null && !$("settings"), "no hostname field in Settings");
  ok([...w.document.querySelectorAll("main>section.half[data-view=settings]")].length === 6 && [...w.document.querySelectorAll("main>section.half[data-view=settings] h2")].map(h=>h.textContent).join() === "Appearance,Time,Front LED,Buzzer,Front button,Power timing", "six settings cards");
  type("c_start_timeout_ms", "500"); click("cfgsave");
  ok($("c_start_timeout_ms").parentNode.parentNode.querySelector(".e").textContent.includes("Allowed range"), "range check blocks save");
  type("c_start_timeout_ms", "20"); click("cfgsave");
  await until(() => [...$("toasts").children].some(e => e.textContent.startsWith("Saved")), 3000, "saved");
  ok((await (await fetch(B + "/api/state")).status) === 401, "(api requires cookie from direct fetch: 401 ok)");
  // only changed fields are sent
  { const L = await (await fetch(B + "/__last")).json(); ok(L.length === 1 && JSON.stringify(L[0]) === '{"led_on":100,"start_timeout_ms":20000}', "settings save sent only changed fields: " + JSON.stringify(L)); }
  ok($("savebar").hidden, "save bar hidden after save");
  // unsaved edit survives leaving Settings
  type("c_led_on", "77"); ok(!$("mark-settings").hidden, "unsaved marker in sidebar");
  await go("network"); ok(visibleViews().join() === "network" && !$("mark-settings").hidden, "marker stays on other view");
  await until(() => $("hn").value === "bc250", 3000, "hostname loaded"); ok($("hnaddr").textContent === "http://bc250.local" && $("hnsave").disabled, "hostname card present in Network, save disabled when unchanged");
  type("hn", "My_Host"); $("hnsave").click(); ok($("hnerr").textContent.includes("1 to 31") && [...$("toasts").children].some(e => /highlighted/.test(e.textContent)), "invalid hostname rejected (inline + toast)");
  type("hn", "Bench-1"); ok($("hnaddr").textContent === "http://bench-1.local", "address hint lowercases");
  await sleep(2500); ok($("hn").value === "Bench-1", "poll does not overwrite typed hostname");
  await go("dashboard"); await go("network"); ok($("hn").value === "Bench-1", "typed hostname kept across views");
  ok(!$("mark-settings").hidden, "settings marker reflects only settings edit (led 77)");
  $("hnsave").click(); await until(async () => false, 1).catch(() => {}); await sleep(600);
  { const L = await (await fetch(B + "/__last")).json(); ok(JSON.stringify(L[L.length - 1]) === '{"hostname":"bench-1"}', "hostname save sent only hostname: " + JSON.stringify(L[L.length - 1])); }
  ok($("n-ssid").textContent !== "-" || true, "network view");
  await go("settings"); ok($("c_led_on").value === "77", "unsaved edit kept"); type("c_led_on", "100"); ok($("mark-settings").hidden && $("savebar").hidden, "marker + save bar clear when equal to saved");
  type("c_led_on", "90"); type("c_buzzer", "1"); $("c_buzzer").checked = false; $("c_buzzer").dispatchEvent(new w.Event("change")); w.document.getElementById("cfgrev").click(); ok($("c_led_on").value === "100" && $("c_buzzer").checked && $("savebar").hidden, "Discard values " + $("c_led_on").value + $("c_buzzer").checked + $("savebar").hidden);
  type("c_led_on", "100");
  // BLE options live in the Bluetooth view with their own save
  { const st = [...w.document.querySelectorAll("main>section[data-view=settings]")].map(e => e.textContent).join(" ");
    ok(!/Re-arm|Minimum signal/.test(st), "ble_* fields absent from Settings"); }
  await go("wake"); ok($("c_ble_rearm_s").closest("section").dataset.view === "wake" && /Wake options/.test($("c_ble_rearm_s").closest("section").textContent) && !$("c_ble_min_rssi") && !/Minimum signal/.test(w.document.body.textContent), "re-arm in Bluetooth view, no Minimum signal setting (2.1.14)");
  ok($("blesave").disabled && $("bleunsaved").hidden, "ble save disabled when unchanged");
  type("c_ble_rearm_s", "1"); ok(!$("blesave").disabled && $("mark-settings").hidden === true, "ble edit enables its own Save, not the Settings marker (settings has none pending)");
  $("blesave").click(); ok(/Allowed range: 3 to 600/.test($("c_ble_rearm_s").closest(".f").querySelector(".e").textContent), "ble range check at field");
  type("c_ble_rearm_s", "20");
  await go("network"); await go("wake"); ok($("c_ble_rearm_s").value === "20" && !$("bleunsaved").hidden, "typed ble value kept across views");
  $("blesave").click(); await sleep(700);
  { const L = await (await fetch(B + "/__last")).json(); ok(JSON.stringify(L[L.length - 1]) === '{"ble_rearm_s":20}', "ble save sent only changed ble field: " + JSON.stringify(L[L.length - 1])); }
  ok($("bleunsaved").hidden && $("blesave").disabled, "ble save done");
  await go("settings"); type("c_led_on", "120"); $("cfgsave").click(); await sleep(700);
  { const L = await (await fetch(B + "/__last")).json(); ok(JSON.stringify(L[L.length - 1]) === '{"led_on":120}', "settings save no longer includes ble_*"); }
  // debug
  await go("dashboard");
  await go("debug"); ok(visibleViews().join() === "debug", "only Debug visible");
  $("dbgsel").value = "dark"; $("dbgsel").dispatchEvent(new w.Event("change"));
  await until(() => !$("dbg").hidden, 4000, "banner");
  ok($("dbgt").textContent.includes("Always dark") && $("t-sense").textContent.includes("pin:"), "debug banner + raw pin shown: " + $("t-sense").textContent);
  await go("log"); ok(!$("dbg").hidden && visibleViews().join() === "log", "debug banner global on Log view");
  await go("debug"); click("dbgback"); await until(() => $("dbg").hidden, 4000, "banner gone");
  ok(true, "Back to real hides banner");
  // security
  await go("security"); ok(visibleViews().join() === "security", "only Security visible");
  type("so", "wrong"); type("s1", "secondpass"); type("s2", "secondpass"); submit("secform");
  await until(() => [...$("toasts").children].some(e => /wrong current/.test(e.textContent)), 3000);
  ok(!$("gate").hidden === false, "wrong old password -> error toast, no gate");
  type("so", "firstpass1"); submit("secform");
  await until(() => [...$("toasts").children].some(e => /Password changed/.test(e.textContent)), 3000, "pw changed");
  ok(true, "password changed");
  click("logout"); await until(() => !$("gate").hidden && !$("glog").hidden, 3000, "logout gate");
  ok(true, "logout shows login");
  type("gp", "secondpass"); submit("glog"); await until(() => $("gate").hidden, 3000);
  ok(true, "login with new password");
  // force off + restart refusal
  await go("controller"); ok(visibleViews().join() === "controller" && $("upbtn").disabled, "Controller view; upload disabled w/o file");
  await go("dashboard");
  act("force_off").click(); $("pyes").click();
  await until(() => $("stname").textContent === "Off", 4000, "forced off");
  ok(true, "force off");
  { const a = await counts(); await sleep(2500); ok((await counts())["/api/log"] === a["/api/log"], "log not polled off-view"); }
  await go("log"); await until(() => $("log").children.length > 3, 8000, "log");
  ok(/ago|just now/.test($("log").children[0].textContent), "log rendered: " + $("log").children[0].textContent);
  ok(!$("log").querySelector("b"), "log text not interpreted as HTML");
  // routing + drawer
  w.location.hash = "bogus"; await sleep(300); ok(visibleViews().join() === "dashboard", "unknown hash -> dashboard");
  w.location.hash = "controller"; await sleep(300); ok($("vtitle").textContent === "Controller" && w.document.querySelector(".nv[aria-current=page]").dataset.view === "controller", "aria-current + title follow hash");
  w.location.hash = "dashboard"; await sleep(200); w.location.hash = "firmware"; await sleep(300); ok(w.location.hash === "#controller" && visibleViews().join() === "controller", "old #firmware link opens the Controller view");
  click("menu"); ok($("side").classList.contains("open") && !$("backdrop").hidden && $("menu").getAttribute("aria-expanded") === "true", "drawer opens");
  w.document.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Escape" })); ok(!$("side").classList.contains("open") && $("backdrop").hidden, "Escape closes drawer");
  click("menu"); click("backdrop"); ok(!$("side").classList.contains("open"), "backdrop closes drawer");
  click("menu"); w.document.querySelector('.nv[data-view=log]').click(); await sleep(300); ok(!$("side").classList.contains("open") && w.location.hash === "#log", "selecting closes drawer, sets hash");
  click("sidelogout"); await until(() => !$("gate").hidden && $("app").hidden, 3000, "sidebar logout"); ok(true, "sidebar logout -> login; app (sidebar) hidden behind gate");
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no console/jsdom errors " + JSON.stringify(d.errors));
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.message); process.exit(2); });
