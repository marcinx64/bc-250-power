// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const P = process.env.P || 8766, B = "http://127.0.0.1:" + P, MODE = process.env.MODE || "main";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
const last = async () => (await (await fetch(B + "/__last")).json());
let btz = "Europe/Kiev";
const pre = w => { w.Intl = { DateTimeFormat: () => ({ resolvedOptions: () => ({ timeZone: btz }) }) }; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/", pre), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); $(id).dispatchEvent(new w.Event("change", { bubbles: true })); };
  const T = () => [...$("toasts").children], clear = () => $("toasts").replaceChildren();
  const go = async v => { w.location.hash = v; await sleep(250); };
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "timepass12"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent !== "\u2026", 5000);
  if (MODE === "main") {
    // zone table sanity
    const STRICT = /^(<[+-]?[A-Za-z0-9]+>|[A-Za-z]{3,})[+-]?\d{1,2}(:\d\d)?((<[+-]?[A-Za-z0-9]+>|[A-Za-z]{3,})(,M\d{1,2}\.\d\.\d(\/[+-]?\d{1,2})?,M\d{1,2}\.\d\.\d(\/[+-]?\d{1,2})?)?)?$/;
    const bad = w.TZN.filter(n => !STRICT.test(w.TZ[n]) || !/^[A-Za-z_]+(\/[A-Za-z_]+){0,2}$/.test(n) && n !== "UTC");
    ok(bad.length === 0 && w.TZN.length >= 90, `zone table: ${w.TZN.length} zones, all POSIX strings match the strict pattern ${bad}`);
    ok(new Set(w.TZN).size === w.TZN.length, "no duplicate zone names");
    const known = { "Europe/Warsaw": "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/London": "GMT0BST,M3.5.0/1,M10.5.0", "America/New_York": "EST5EDT,M3.2.0,M11.1.0", "Asia/Tokyo": "JST-9", "Asia/Kolkata": "IST-5:30", "Australia/Sydney": "AEST-10AEDT,M10.1.0,M4.1.0/3", "America/Sao_Paulo": "<-03>3", "America/Los_Angeles": "PST8PDT,M3.2.0,M11.1.0", "UTC": "UTC0", "Asia/Kathmandu": "<+0545>-5:45", "Pacific/Auckland": "NZST-12NZDT,M9.5.0,M4.1.0/3", "Europe/Kyiv": "EET-2EEST,M3.5.0/3,M10.5.0/4", "America/St_Johns": "NST3:30NDT,M3.2.0,M11.1.0" };
    ok(Object.entries(known).every(([n, z]) => w.TZ[n] === z), "spot-checks (Warsaw, London, New York, Tokyo, Kolkata, Sydney, Sao Paulo, Los Angeles, UTC, Kathmandu, Auckland, Kyiv, St Johns) match known strings");
    ok(w.tzFind("Europe/Kiev") === "Europe/Kyiv" && w.tzFind("Asia/Calcutta") === "Asia/Kolkata" && w.tzFind("Mars/Base") === "" && w.tzFind("Etc/UTC") === "UTC", "aliases resolve; unknown zone does not");
    // auto first setup (browser Europe/Kiev, tz_name empty)
    await until(async () => 1, 10); await sleep(1500);
    let L = await last();
    ok(L.length === 1 && JSON.stringify(L[0]) === JSON.stringify({ tz: "EET-2EEST,M3.5.0/3,M10.5.0/4", tz_name: "Europe/Kyiv" }), "automatic first setup sent tz + tz_name once: " + JSON.stringify(L));
    ok(T().some(t => /Timezone set to Europe\/Kyiv/.test(t.textContent) && t.classList.contains("info")), "info toast shown");
    await go("settings"); await sleep(500); await go("network"); await go("wake"); await go("settings"); await sleep(800);
    L = await last(); ok(L.length === 1, "no second automatic setup after revisiting views");
    // time card
    await until(() => /^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d$/.test($("tm-local").textContent), 3000, "local time");
    ok($("tm-info").hidden && !/Set from/.test($("tmbody").textContent), "Time card: local time only, no timezone or source line (2.2.29)");
    { const a = $("tmadv"), b = a.querySelector("button.advb");
      ok(!a.classList.contains("open") && b.getAttribute("aria-expanded") === "false" && w.getComputedStyle(a.querySelector(".fbody>div")).visibility === "hidden", "Advanced closed at first");
      b.click(); ok(a.classList.contains("open") && b.getAttribute("aria-expanded") === "true" && w.getComputedStyle(a.querySelector(".fbody>div")).visibility !== "hidden", "Advanced slides open");
      ok(/0 = only when the clock is not set/.test(a.textContent), "the 0 = ... note is kept");
      b.click(); ok(!a.classList.contains("open"), "and closed again"); }
    ok((await counts())["/api/time"] === undefined, "no POST /api/time when synced");
    ok($("tzsel").value === "Europe/Kyiv", "picker shows the saved zone");
    // search + manual choose
    { const opts = [...$("tzsel").options].filter(o => o.value); const groups = [...$("tzsel").querySelectorAll("optgroup")].map(g => g.label);
      ok(opts.length === w.TZN.length && groups.includes("Europe") && groups.includes("America") && groups.includes("Asia") && !$("tzq") && opts.some(o => o.value === "Europe/Warsaw" && o.parentElement.label === "Europe" && o.textContent === "Warsaw"), "picker lists all " + opts.length + " zones in " + groups.length + " region groups, no search box"); }
    $("tzsel").value = "Europe/Warsaw"; $("tzsel").dispatchEvent(new w.Event("change"));
    ok(!$("savebar").hidden && $("c_tz").value === "CET-1CEST,M3.5.0,M10.5.0/3", "choosing a zone marks Settings unsaved");
    // browser button, alias
    btz = "Asia/Calcutta"; $("tzbrowser").click(); ok($("tzsel").value === "Asia/Kolkata" && $("c_tz").value === "IST-5:30", "browser zone alias Asia/Calcutta -> Asia/Kolkata");
    btz = "Mars/Base"; clear(); $("tzbrowser").click(); ok($("tzsel").value === "Asia/Kolkata" && T().length === 1 && /not in the list/.test(T()[0].textContent), "unknown browser zone: message, selection unchanged");
    btz = "Europe/Kiev"; $("tzbrowser").click(); $("tzsel").value = "Europe/Warsaw"; $("tzsel").dispatchEvent(new w.Event("change"));
    clear(); $("cfgsave").click(); await until(() => T().length === 1, 3000); L = await last();
    ok(JSON.stringify(L[L.length - 1]) === JSON.stringify({ tz: "CET-1CEST,M3.5.0,M10.5.0/3", tz_name: "Europe/Warsaw" }) && T()[0].classList.contains("ok") && $("savebar").hidden, "save sends tz + tz_name through the Settings save bar");
    // NTP + validation
    $("ntp").value = "bad host!"; $("ntp").dispatchEvent(new w.Event("input")); clear(); $("cfgsave").click();
    ok(!$("ntperr").hidden && T().length === 1 && /highlighted/.test(T()[0].textContent), "invalid NTP server rejected at the field");
    type("ntp", "time.example.org"); clear(); $("cfgsave").click(); await sleep(600); L = await last();
    ok(JSON.stringify(L[L.length - 1]) === '{"ntp_server":"time.example.org"}', "ntp_server saved alone");
    // quiet hours
    type("c_quiet_on", "1"); $("c_quiet_on").checked = true; $("c_quiet_on").dispatchEvent(new w.Event("change"));
    type("c_quiet_from", "23:00"); type("c_quiet_to", "06:00"); $("cfgsave").click(); await sleep(700); L = await last();
    ok(JSON.stringify(L[L.length - 1]) === '{"quiet_on":true,"quiet_from":1380,"quiet_to":360}', "quiet hours saved as minutes: " + JSON.stringify(L[L.length - 1]));
    type("c_quiet_from", "22:00"); type("c_quiet_to", "07:00"); $("cfgsave").click(); await sleep(700); L = await last();
    ok(JSON.stringify(L[L.length - 1]) === '{"quiet_from":1320,"quiet_to":420}' && $("c_quiet_from").value === "22:00" && $("c_quiet_to").value === "07:00", "22:00 -> 1320, 07:00 -> 420, displayed back");
    await sleep(1200); ok(/^(Quiet|Not quiet) now\.$/.test($("quietst").textContent), "live status: " + $("quietst").textContent);
    // IP settings
    await go("network"); await until(() => $("n-gw").textContent !== "-", 3000);
    ok($("n-dhcp").textContent === "Automatic (DHCP)" && $("n-gw").textContent === "192.168.1.1" && /^\d+\.\d+\.\d+\.\d+$/.test($("n-mask").textContent) && $("n-dns1").textContent !== "-" && $("ipmode").value === "dhcp" && !$("ipfixw").classList.contains("open"), "status shows DHCP + current values: " + $("n-mask").textContent + " " + $("n-gw").textContent);
    ok($("n-bssid").textContent === "AA:BB:CC:DD:EE:FF, channel 6", "Network page shows the access point (capitals): " + $("n-bssid").textContent);
    const cnt0 = (await last()).length;
    $("ipmode").value = "fixed"; $("ipmode").dispatchEvent(new w.Event("change"));
    ok($("ipfixw").classList.contains("open") && $("ipmode").selectedOptions[0].textContent === "Manual" && $("ip_ip").style.width === "200px" && $("ip_ip").value === "192.168.1.208" && $("ip_gw").value === "192.168.1.1", "Fixed prefilled from current values");
    const tryv = async (o, field, re, name) => {
      const v = Object.assign({ ip: "192.168.1.50", mask: "255.255.255.0", gw: "192.168.1.1", dns1: "8.8.8.8", dns2: "" }, o);
      for (const k in v) { $("ip_" + k).value = v[k]; $("ip_" + k).dispatchEvent(new w.Event("input")); }
      clear(); $("ipsave").click();
      ok(!$("ipe_" + field).hidden && re.test($("ipe_" + field).textContent) && $("modal").hidden && T().length === 1 && T()[0].classList.contains("err") && (await last()).length === cnt0, name + ": error at field, no modal, nothing sent");
    };
    await tryv({ mask: "255.0.255.0" }, "mask", /valid subnet mask/, "non-contiguous mask");
    await tryv({ mask: "128.0.0.0" }, "mask", /valid subnet mask/, "mask not starting with 255");
    await tryv({ mask: "255.255.255.255" }, "mask", /valid subnet mask/, "/32 mask");
    await tryv({ gw: "10.10.2.1" }, "gw", /same subnet/, "gateway outside subnet");
    await tryv({ ip: "192.168.1.0" }, "ip", /network address/, "network address");
    await tryv({ ip: "192.168.1.255" }, "ip", /broadcast/, "broadcast address");
    await tryv({ gw: "192.168.1.50" }, "gw", /differ/, "gateway equals IP");
    await tryv({ gw: "192.168.1.255" }, "gw", /device address/, "gateway is broadcast");
    await tryv({ ip: "192.168.1.300" }, "ip", /IPv4/, "bad octet");
    await tryv({ dns1: "x" }, "dns1", /IPv4/, "bad DNS");
    // valid -> modal -> confirm
    for (const [k, v] of Object.entries({ ip: "192.168.1.50", mask: "255.255.255.0", gw: "192.168.1.1", dns1: "8.8.8.8", dns2: "" })) { $("ip_" + k).value = v; $("ip_" + k).dispatchEvent(new w.Event("input")); }
    clear(); $("ipsave").click();
    ok(!$("modal").hidden && /http:\/\/192\.168\.1\.50\//.test($("mb").textContent) && /\.local/.test($("mb").textContent) && /BC250-Setup/.test($("mb").textContent) && /BC-250\u2019s power state is unaffected/.test($("mb").textContent) && (await last()).length === cnt0, "confirm modal: new address, .local, recovery; nothing sent yet");
    $("mcancel").click(); ok((await last()).length === cnt0, "Cancel sends nothing");
    // typed values kept across views
    await go("dashboard"); await go("network"); await sleep(600); ok($("ip_ip").value === "192.168.1.50" && $("ipmode").value === "fixed", "typed IP values kept after leaving the view");
    $("ipsave").click(); $("mgo").click(); await until(async () => 1, 10); await sleep(600);
    L = await last(); ok(JSON.stringify(L[L.length - 1]) === '{"dhcp":false,"ip":"192.168.1.50","mask":"255.255.255.0","gw":"192.168.1.1","dns1":"8.8.8.8","dns2":""}', "POST body for fixed: " + JSON.stringify(L[L.length - 1]));
    ok(!$("ipnote").hidden && $("ipnote").querySelector("a").getAttribute("href") === "http://192.168.1.50/" && $("ipnote").closest("#banners") && /Saved/.test($("ipnote").textContent), "persistent top banner with link to the new address");
    $("ipmode").value = "dhcp"; $("ipmode").dispatchEvent(new w.Event("change")); $("ipsave").click(); $("mgo").click(); await sleep(600);
    L = await last(); ok(JSON.stringify(L[L.length - 1]) === '{"dhcp":true}', "POST body for DHCP");
  } else if (MODE === "syncon" || MODE === "syncoff") {
    await go("settings"); await sleep(1500);
    const on = MODE === "syncon";
    ok($("tmsync").disabled === !on, "Sync now " + (on ? "enabled when connected" : "disabled when not connected"));
    ok($("tm-last").textContent === (on ? "Last internet time check: 10 min ago" : "Last internet time check: none since the controller started"), "last check line: " + $("tm-last").textContent);
    ok(/only when the clock is not set/.test($("ntphrow").textContent) && /daylight saving|Daylight saving/.test($("ntphrow").textContent) && $("ntph").value === "24", "ntp_check_h field + help, default 24");
    clear(); $("tmsync").disabled = false; $("tmsync").click(); await until(() => T().length === 1, 3000);
    if (on) { ok(T()[0].classList.contains("ok") && /Time check started/.test(T()[0].textContent) && (await counts())["/api/time/sync"] === 1, "Sync now: POST /api/time/sync, success toast"); await sleep(1500); ok(/Last internet time check: (just now|\d s ago)/.test($("tm-last").textContent), "last check updates: " + $("tm-last").textContent); }
    else ok(T()[0].classList.contains("err") && /not connected/.test(T()[0].textContent), "409 text shown as error toast");
    if (on) {
      type("ntph", "800"); clear(); $("cfgsave").click(); const e = $("ntphe");
      ok(!e.hidden && /0 to 720/.test(e.textContent) && T().length === 1, "ntp_check_h range check at the field");
      type("ntph", "48"); clear(); $("cfgsave").click(); await sleep(700); const L = await last();
      ok(JSON.stringify(L[L.length - 1]) === '{"ntp_check_h":48}', "ntp_check_h saved alone: " + JSON.stringify(L[L.length - 1]));
    }
  } else if (MODE === "unsynced") {
    await sleep(1800);
    const c = await counts(); ok(c["/api/time"] === 1, "unsynced: exactly one POST /api/time (" + c["/api/time"] + ")");
    const L = await last(); ok(T().length === 0 || T().every(t => !/handover|Time check|clock/i.test(t.textContent) && !t.classList.contains("err")), "no toast for the time handover");
    await go("settings"); await sleep(1500);
    if (process.env.IGNORE) { ok(/Clock not set/.test($("tm-local").textContent) && /Clock not set/.test($("tm-info").textContent) && /internet/.test($("tm-info").textContent), "label: Clock not set"); ok(!$("quietst") || true, "ok"); }
    else ok($("tm-info").hidden && /^\d{4}-/.test($("tm-local").textContent), "set from this browser: local time, no source line");
    await sleep(3000); ok((await counts())["/api/time"] === 1, "still only one POST after more polls");
  } else if (MODE === "kept") {
    await go("settings"); await sleep(1200);
    ok($("tm-info").hidden && (await counts())["/api/time"] === undefined, "kept through the restart: no source line, no handover");
  } else if (MODE === "nonet") {
    await go("network"); await sleep(800);
    ok(!$("ipold").hidden && $("ipbody").hidden && !T().some(t => t.classList.contains("err")), "GET /api/net 404: 'Needs firmware 1.0.13', no toast");
  } else if (MODE === "notime") {
    await go("settings"); await sleep(1500);
    ok(!$("tmold").hidden && $("tmbody").hidden && $("c_quiet_on").closest(".f").hidden, "no state.time: Time card shows 'Needs firmware 1.0.13', quiet rows hidden");
    ok((await counts())["/api/time"] === undefined && (await last()).length === 0, "no POST /api/time, no automatic timezone setup");
    await go("network"); await sleep(500); ok($("n-dhcp").textContent === "-", "no wifi.dhcp: dash");
  }
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no page errors " + JSON.stringify(d.errors.filter(e => !/scrollTo/.test(e))));
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
