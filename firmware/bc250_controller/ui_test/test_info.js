// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const counts = async () => (await fetch(B + "/__counts")).json();
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); };
  await until(() => !$("gset").hidden, 3000);
  type("gn1", "infopass12"); type("gn2", "infopass12"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent === "Off", 4000);
  await sleep(2500);
  ok((await counts())["/api/info"] === 1, "exactly one /api/info request for the dashboard");
  ok(!/bc250|1\.0\./.test($("side").textContent) && !$("devname") && !$("devfw"), "sidebar has no hostname or version");
  w.location.hash = "controller";
  const missing = !!process.env.NOINFO;
  if (!missing) {
    await until(() => !$("ab-dl").hidden, 4000, "about");
    const fwCards = [...w.document.querySelectorAll("main>section[data-view=controller]")];
    ok(fwCards[0].id === "hw" && /^Hardware/.test(fwCards[0].querySelector("h2").textContent) && fwCards[1].id === "about" && fwCards[1].querySelector("h2").textContent === "Firmware", "Controller view: Hardware card, then Firmware card");
    const hwt = $("hw-dl").textContent, fwt = $("ab-dl").textContent;
    ok(/Chip/.test(hwt) && /MAC address/.test(hwt) && /Memory/.test(hwt) && !/ESP-IDF|Uptime|Built/.test(hwt) && /ESP-IDF/.test(fwt) && /Uptime/.test(fwt) && !/MAC address|Chip temperature|Memory/.test(fwt), "hardware rows and firmware rows are in separate cards");
    const dl = ($("ab-dl").textContent + " " + $("hw-dl").textContent).replace(/\s+/g, " ");
    ok($("ab-ver").textContent === "1.0.0-mock" && /Nov 15 2026 02:53:59/.test(dl) && /app0/.test(dl) && /1\.38 MB of 1\.88 MB \(74 %\)/.test(dl) && !$("ab-bar"), "version, build, slot, size: " + dl.slice(0, 160));
    ok(/v5\.5\.1/.test(dl) && /ESP32-C3, rev 4, 160 MHz/.test(dl) && /4\.00 MB/.test(dl) && /A0:00:00:00:00:94/.test(dl) && /146 KB free \(lowest since start: 117 KB\)/.test(dl), "idf, chip, flash, mac, memory");
    ok($("ab-cpu").textContent === "23 %" && $("ab-cpu-b").style.width === "23%", "about: CPU bar " + $("ab-cpu").textContent);
    ok(/^46 % used of 273 KB$/.test($("ab-mem").textContent) && $("ab-mem-b").style.width === "46%" && !$("ab-mem-m").hidden && $("ab-mem-m").style.left === "calc(57% - 1px)", "about: memory bar + highest-use mark " + $("ab-mem").textContent + " " + $("ab-mem-m").style.left);
    ok($("ab-temp").textContent === "41.5 \u00b0C" && /Chip temperature/.test(dl) && $("ab-temp").className === "", "chip temperature: " + $("ab-temp").textContent);
    const rs = $("ab-rst").textContent, lab = $("ab-dl").textContent;
    if (!process.env.REASON) ok(rs === "Firmware update" && $("ab-rst").className === "" && /Started after/.test(lab) && !/Last restart/.test(lab), "Started after: capitalised, not highlighted: " + rs);
    ok(!/Installed version/.test($("fw").textContent), "version not repeated in Update card");
    await sleep(800); const c0 = (await counts())["/api/info"]; await sleep(11500); const c1 = (await counts())["/api/info"];
    ok(c1 === c0 + 1, `refresh about every ~10 s (${c0} -> ${c1})`);
    w.location.hash = "dashboard"; await sleep(500); const c2 = (await counts())["/api/info"]; await sleep(11500);
    ok((await counts())["/api/info"] === c2, "no /api/info requests after leaving the view");
    if (process.env.REASON) ok($("ab-rst").className === "warn" && $("ab-rst").textContent === process.env.WANT, "reason " + process.env.REASON + " warned: " + $("ab-rst").textContent);
  } else {
    await until(() => (async () => 1)() && true, 100); await sleep(1500);
    ok($("ab-dl").hidden && $("ab-ver").textContent === "1.0.0-mock" || $("ab-ver").textContent.length > 0, "404: only version shown, details hidden (" + $("ab-ver").textContent + ")");
    ok($("ab-dl").hidden && $("toasts").children.length === 0, "404: no toast");
    ok($("hw-dl").hidden && !$("hwold").hidden, "404: Hardware card says the firmware does not report it");
    await sleep(11500); ok((await counts())["/api/info"] === 1, "404: not retried");
  }
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
