// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  await fetch(B + "/api/auth/password", { method: "POST", body: JSON.stringify({ new: "latepass123" }) });
  const r = await fetch(B + "/api/login", { method: "POST", body: JSON.stringify({ password: "latepass123" }) });
  const ck = r.headers.get("set-cookie").split(";")[0];
  await fetch(B + "/api/config", { method: "POST", headers: { Cookie: ck }, body: JSON.stringify({ ble_wake: false }) });
  for (const wakeOff of [true, false]) {
    if (!wakeOff) await fetch(B + "/api/config", { method: "POST", headers: { Cookie: ck }, body: JSON.stringify({ ble_wake: true }) });
    const d = await open(B + "/#wake"), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
    await until(() => !$("glog").hidden, 3000);
    const secs = () => [...w.document.querySelectorAll("main>section[data-view=wake]")];
    let early = 0, stateSeen = false;
    $("gp").value = "latepass123"; $("glog").dispatchEvent(new w.Event("submit", { cancelable: true }));
    const t0 = Date.now();
    while (Date.now() - t0 < 2500) {
      const visible = secs().filter(s => !s.hidden).length;
      if (!stateSeen && $("stname").textContent !== "…") stateSeen = true;
      if (!stateSeen && visible) early++;
      await sleep(2);
    }
    const vis = secs().filter(s => !s.hidden).length;
    console.log((early === 0 && vis === (wakeOff ? 2 : 4) ? "PASS " : "FAIL ") + `late state, wake ${wakeOff ? "off" : "on"}: cards before first state: ${early}, after: ${vis}`);
  }
  process.exit(0);
})().catch(e => { console.log("ERROR", e.stack || e.message); process.exit(2); });
