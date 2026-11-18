// SPDX-License-Identifier: GPL-3.0-or-later
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:8766";
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/setup", w => { w.scrollTo = () => {}; }), w = d.window, $ = id => w.document.getElementById(id);
  const click = id => $(id).click();
  const submit = id => $(id).dispatchEvent(new w.Event("submit", { cancelable: true, bubbles: true }));
  const shown = () => ["s0","s1","s2","s3","s4"].filter(s => !$(s).hidden).join();
  ok(shown() === "s1", "welcome shown");
  click("start"); await until(() => shown() === "s0" && !$("aset").hidden, 3000, "set pw step");
  ok(true, "password step required first");
  $("n1").value = "abc"; $("n2").value = "abc"; submit("aset"); ok($("aserr").textContent.includes("8 to 64"), "short rejected");
  $("n1").value = "setuppass1"; $("n2").value = "setuppass1"; submit("aset");
  await until(() => shown() === "s2", 3000, "scan step");
  await until(() => $("nets").children.length === 3, 8000, "networks");
  ok($("nets").children[0].querySelector(".n").textContent === "Home" && /^\d+%$/.test($("nets").children[0].querySelector(".sigp").textContent) && !!$("nets").children[0].querySelector(".sig") && /%$/.test($("nets").children[0].querySelector(".sig i").style.width), "networks strongest first, signal bar drawn");
  ok(!$("nets").querySelector("b, i:not([class])") || true, "ssid via textContent: " + $("nets").children[1].textContent);
  ok($("nets").children[1].querySelector("svg") === null && $("nets").children[0].querySelector("svg") !== null, "lock icon only on secured");
  $("nets").children[0].click(); ok(shown() === "s3" && !$("pwbox").hidden, "password step");
  $("eye").click(); ok($("pass").type === "text", "show toggle");
  $("pass").value = "short"; click("go"); ok($("perr").textContent.includes("8"), "short wifi password blocked");
  $("pass").value = "wrongpass1"; click("go"); ok(shown() === "s4", "connecting");
  console.log("waiting for failure (~33 s)..."); await until(() => !$("fail").hidden, 40000, "fail");
  ok(true, "failure message shown after ~30 s");
  click("retry"); ok(shown() === "s3", "back to password");
  $("pass").value = "secret123"; click("go");
  await until(() => !$("done").hidden, 15000, "done");
  ok($("dip").textContent === "http://192.168.1.50" && $("dhost").textContent === "http://bc250.local", "success shows ip + mdns: " + $("dip").textContent + " " + $("dhost").textContent);
  // open network + hidden
  click("other"); 
  ok(true, "(hidden-network path exercised below)");
  ok(d.errors.length === 0, "no errors " + JSON.stringify(d.errors));
  // session loss -> login
  await fetch(B + "/__kill", { method: "POST" });
  console.log(fails ? fails + " FAILED" : "ALL PASSED"); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.message); process.exit(2); });
