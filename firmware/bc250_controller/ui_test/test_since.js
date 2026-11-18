// SPDX-License-Identifier: GPL-3.0-or-later
// The line under the state name: how long the BC-250 has been on or off (spec section 25).
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "sincepass123"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent !== "\u2026", 5000, "logged in");
  const sub = () => $("stsub").textContent;
  const post = (p, b) => w.fetch(p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
  // the mock's clock is UTC: the expected "since" for a period that began s seconds ago (either side of a minute edge)
  const at = (s, date) => [0, 60].map(e => { const i = new Date(Date.now() - (s + e) * 1000).toISOString(); return (date ? i.slice(0, 10) + " " : "") + i.slice(11, 16); });
  const since = t => (/, since (.+)$/.exec(t) || [])[1];

  await until(() => /^Powered off for at least \d+s, since the controller was powered up$/.test(sub()), 3000, "off since start");
  ok(true, "off, not run since the controller was powered up: " + sub());

  await fetch(B + "/__since=5400"); await until(() => /^Powered off for 1h 30m, since /.test(sub()), 3000, "off 1h 30m");
  ok(at(5400).includes(since(sub())), "off for 1h 30m, since today's time only: " + sub());
  const s1 = since(sub()); await sleep(2300);
  ok(since(sub()) === s1, "the since time stays put between polls: " + s1 + " / " + since(sub()));

  await fetch(B + "/__since=200000"); await until(() => /^Powered off for 2d 7h, since /.test(sub()), 3000, "off 2d 7h");
  ok(at(200000, true).includes(since(sub())), "off for 2d 7h, since an earlier day with its date: " + sub());

  await post("/api/power", { action: "on" });
  await until(() => $("stname").textContent === "Starting", 3000, "starting");
  ok(sub() === "Powering up the supply and waiting for the BC-250.", "starting: the state's sentence");
  await until(() => $("stname").textContent === "On", 6000, "on");
  await until(() => /^Powered on for \d+s, since \d\d:\d\d$/.test(sub()), 3000, "on, seconds");
  ok(true, "on: counts from the start of running: " + sub());
  await fetch(B + "/__since=7980"); await until(() => /^Powered on for 2h 13m, since /.test(sub()), 3000, "on 2h 13m");
  ok(at(7980).includes(since(sub())), "on for 2h 13m: " + sub());

  // no clock set: no "since" part; older firmware: the plain sentences
  const keep = w.clk; w.clk = null;
  ok(w.stSub({ on_s: 7980 }, "on") === "Powered on for 2h 13m" && w.stSub({ off_s: 65, off_since_start: false }, "off") === "Powered off for 1m 5s", "without a clock: duration only");
  w.clk = keep;
  ok(w.stSub({}, "on") === "The BC-250 is running." && w.stSub({}, "off") === "The BC-250 is powered off." && w.stSub({ on_s: null, off_s: 9 }, "shutting_down") === "Waiting for the operating system to shut down.", "fields missing or a passing state: the state's sentence");

  await post("/api/power", { action: "shutdown" });
  await until(() => $("stname").textContent === "Shutting down", 3000, "shutting down");
  ok(sub() === "Waiting for the operating system to shut down.", "shutting down: the state's sentence");
  await until(() => $("stname").textContent === "Off", 9000, "off");
  await until(() => /^Powered off for \d+s, since \d\d:\d\d$/.test(sub()), 3000, "off after a shutdown");
  ok(!/at least/.test(sub()), "after a shutdown: counts from the stop, no 'at least': " + sub());

  ok(!/undefined|NaN/.test(sub()), "no 'undefined' or 'NaN'");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
