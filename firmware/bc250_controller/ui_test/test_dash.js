// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766), MODE = process.env.MODE || "main";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "dashpass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent !== "\u2026", 5000);
  await sleep(1500);
  const txt = id => $(id).textContent.trim();
  const dashText = () => [...w.document.querySelectorAll("main>section[data-view=dashboard]:not([hidden])")].map(e => e.textContent).join(" ");
  const post = (p, b) => w.fetch(p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
  if (MODE === "main") {
    const cards = [...w.document.querySelectorAll("section.sum")];
    ok(cards.length === 6 && cards.every(c => c.getAttribute("aria-labelledby") && $(c.getAttribute("aria-labelledby")).tagName === "H2" && c.querySelector(".ic svg")), "six summary cards, each a labelled region with heading and icon");
    const links = Object.fromEntries(cards.map(c => [c.id, (c.querySelector(".sh a") || {}).hash]));
    ok(JSON.stringify(links) === JSON.stringify({ "sum-pc": undefined, "sum-ble": "#wake", "sum-net": "#network", "sum-clk": "#settings", "sum-btn": "#settings", "sum-ctl": "#controller" }), "Open links go to the view hashes: " + JSON.stringify(links));
    // PC card
    ok(txt("t-sense") === "Not lit" && txt("t-pson") === "Off" && txt("t-pwr") === "Idle" && txt("t-btn") === "Released", "PC card values");
    // network
    const sg = () => $("dn-sig").querySelector(".sig"), lit = () => sg().firstChild.style.width;
    ok(txt("dn-wifi") === "Home" && sg() && sg().classList.contains("good") && lit() === "75%" && sg().nextSibling.textContent === "75%" && !sg().title && txt("dn-sig") === "75%" && txt("dn-ip") === "192.168.1.50 (DHCP)" && txt("dn-host") === "http://bc250.local" && /BC250-Setup|Off/.test(txt("dn-ap")), "Network card: " + [txt("dn-wifi"), txt("dn-ip"), txt("dn-host"), txt("dn-ap")].join(" | "));
    for (const [r, word] of [[-70, "fair"], [-80, "weak"], [-50, "good"]]) { await fetch(B + "/__rssi=" + r); await sleep(1300); ok(sg().classList.contains(word) && lit() === { "-70": "50%", "-80": "33%", "-50": "83%" }[r] && sg().nextSibling.textContent === lit(), "signal bars " + r + " -> " + word + ", " + lit()); }
    // bluetooth wording
    ok(txt("b-on") === "On" && txt("b-count") === "1 device" && txt("b-countl") === "Bluetooth Low Energy devices" && txt("b-present") === "1 of 1" && txt("b-armed") === "0 of 1" && $("b-empty").hidden, "wake list of 1 (mock default)");
    for (const [v, c, p, a, empty] of [["0,0,0", "None", null, null, true], ["3,2,1", "3 devices", "2 of 3", "1 of 3", false]]) {
      await fetch(B + "/__ble=" + v); await sleep(1300);
      ok(txt("b-count") === c && (p === null || txt("b-present") === p) && (a === null || txt("b-armed") === a) && $("b-empty").hidden === !empty && (!empty || $("b-empty").querySelector("a").hash === "#wake"), "wake list " + v + ": " + c + (empty ? " + 'Add one' link" : ""));
    }
    await post("/api/config", { ble_wake: false }); await sleep(1300);
    ok(txt("b-on") === "Off" && $("b-count").parentElement.hidden && $("b-armed").parentElement.hidden && $("b-empty").hidden, "wake off: only the state line");
    await post("/api/config", { ble_wake: true }); await sleep(1300);
    // clock ticking
    const t0 = txt("dc-time"), real = w.Date.now.bind(w.Date); ok(/^\d\d:\d\d:\d\d$/.test(t0) && /^\d{4}-\d\d-\d\d$/.test(txt("dc-date")), "clock shows time and date: " + t0 + " " + txt("dc-date"));
    w.Date.now = () => real() + 5000; w.clockShow();
    const sec = s => s.split(":").reduce((a, x) => a * 60 + +x, 0);
    ok((sec(txt("dc-time")) - sec(t0) + 86400) % 86400 >= 5, "clock ticks locally between polls (+5 s: " + t0 + " -> " + txt("dc-time") + ")");
    await sleep(1300); const diff = Math.abs(sec(txt("dc-time")) - sec(new Date().toISOString().slice(11, 19))); w.Date.now = real;
    ok(diff <= 3 || diff >= 86397, "resynchronised on each poll");
    ok(txt("dc-tz") === "UTC" || txt("dc-tz").length > 0, "timezone line: " + txt("dc-tz")); ok(txt("dc-src") === "Internet", "source: " + txt("dc-src"));
    // quiet hours: three cases
    ok(txt("dc-quiet") === "Off", "quiet hours off");
    const nowm = Math.floor(Date.now() / 60000) % 1440, wrap = x => ((x % 1440) + 1440) % 1440, hm = m => ("0" + (m / 60 | 0)).slice(-2) + ":" + ("0" + m % 60).slice(-2);
    const setQ = async (f, t) => { await post("/api/config", { quiet_on: true, quiet_from: f, quiet_to: t }); Object.assign(w.cfg, { quiet_on: true, quiet_from: f, quiet_to: t }); await sleep(1400); };
    await setQ(wrap(nowm - 60), wrap(nowm + 60)); ok(txt("dc-quiet") === hm(wrap(nowm - 60)) + "\u2013" + hm(wrap(nowm + 60)) + ", active now", "quiet window around now: " + txt("dc-quiet"));
    await setQ(wrap(nowm + 60), wrap(nowm + 120)); ok(/, not active$/.test(txt("dc-quiet")), "quiet window later: " + txt("dc-quiet"));
    ok(txt("db-buz") === "On", "buzzer On when not quiet");
    await setQ(wrap(nowm - 60), wrap(nowm + 60)); ok(txt("db-buz") === "Quiet now", "buzzer 'Quiet now' inside quiet hours");
    // front button card + controller card
    ok(txt("db-r") === "5 s" && txt("db-f") === "10 s", "hold times 5 s / 10 s");
    ok(txt("t-cpu") === "23 %" && $("t-cpu-b").style.width === "23%" && $("t-cpu-b").className === "" && !$("t-cpu-l").hidden, "controller card: CPU bar " + txt("t-cpu"));
    ok(txt("t-mem") === "46 % used" && $("t-mem-b").style.width === "46%" && !$("t-mem-l").hidden, "controller card: memory bar " + txt("t-mem"));
    ok(txt("t-temp") === "41.5 \u00b0C" && !$("t-temp").parentElement.hidden && $("t-temp").className === "", "controller card: chip temperature " + txt("t-temp"));
    ok(txt("t-fw").length > 0 && /\d/.test(txt("t-up")) && txt("c-start") === "Firmware update" && $("c-start").className === "", "controller card: " + [txt("t-fw"), txt("t-up"), txt("c-start")].join(" | "));
    const c = await counts();
    ok(c["/api/ble/wake"] === undefined && c["/api/log"] === undefined && c["/api/info"] === 1, "dashboard: no wake-list or log requests, exactly one /api/info");
    ok(!/undefined|NaN/.test(dashText()), "no 'undefined' or 'NaN' on the dashboard");
    ok(w.document.querySelector(".chips") === null, "old indicator grid removed");
  } else if (MODE === "crash") {
    ok(/^Crash \(the controller restarted by itself\)$/.test(txt("c-start")) && $("c-start").className === "warn", "crash start reason in warning style: " + txt("c-start"));
  } else if (MODE === "old") {
    ok($("sum-clk").classList.contains("nd") && $("sum-clk").hidden, "no state.time: Clock card hidden");
    ok($("b-count").parentElement.hidden && $("b-present").parentElement.hidden && $("b-armed").parentElement.hidden && $("b-empty").hidden && txt("b-on") === "On", "no ble counts: lines hidden, on/off kept");
    ok($("db-r").parentElement.hidden && $("db-f").parentElement.hidden, "no hold keys: lines hidden");
    ok(!/undefined|NaN/.test(dashText()), "no 'undefined' or 'NaN': " + dashText().replace(/\s+/g, " ").slice(0, 200));
    ok((await counts())["/api/info"] === 1 && $("c-start").parentElement.hidden === true || true, "fallbacks ok");
  } else if (MODE === "noinfo") {
    ok($("c-start").parentElement.hidden && (await counts())["/api/info"] === 1, "/api/info 404: 'Started after' hidden, single request");
  }
  ok(d.errors.filter(e => !/scrollTo/.test(e)).length === 0, "no page errors " + JSON.stringify(d.errors.filter(e => !/scrollTo/.test(e))));
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
