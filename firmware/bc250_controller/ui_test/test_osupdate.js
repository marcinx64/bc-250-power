// SPDX-License-Identifier: GPL-3.0-or-later
// OS updates view: the switch guards everything, settings, first-time setup, scripts, actions, time-card note.
// Mock: MOCK_CONN=1 on port P.
const { open, until, sleep } = require("./harness.js");
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
(async () => {
  const B = "http://127.0.0.1:" + (process.env.P || 8766);
  await fetch(B + "/__reset", { method: "POST" });
  const d = await open(B + "/", w => { w.scrollTo = () => {}; }), w = d.window, $ = id => w.document.getElementById(id);
  const patch = o => fetch(B + "/__osu", { method: "POST", body: JSON.stringify(o) });
  const posts = async () => (await (await fetch(B + "/__last")).json()).filter(x => x._p);
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); $(id).dispatchEvent(new w.Event("change", { bubbles: true })); };
  const toasts = () => [...$("toasts").children].map(t => t.textContent);
  const shown = id => !$(id).hidden;
  const poll = () => { w.lastOs = 0; return sleep(1300); };
  await until(() => !$("gset").hidden, 3000);
  $("gn1").value = $("gn2").value = "osupdpass1"; $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent !== "…", 5000);

  // sidebar entry right below the wake entry
  const nav = [...w.document.querySelectorAll("nav .nv")];
  const iw = nav.findIndex(b => b.dataset.view === "wake");
  ok(nav[iw + 1].dataset.view === "osupdate" && nav[iw + 1].textContent.trim() === "OS updates", "sidebar: OS updates right below the wake entry");

  // time card note
  ok(/Used for the quiet hours and for automatic OS updates/.test($("timec").textContent), "time card mentions automatic OS updates");

  // disabled: only the switch and one line
  w.location.hash = "osupdate"; await until(() => shown("osu") && $("osu-en").disabled === false, 4000, "view");
  const vis = () => ["osu-st", "osu-cf", "osu-su", "osu-sc"].map(shown);
  ok(!$("osu-en").checked && vis().every(x => !x), "off: only the switch card is shown");
  ok($("osu").querySelectorAll("p:not([hidden])").length === 1, "off: one line of explanation");
  ok(w.document.title.startsWith("OS updates"), "title");
  const g = await w.fetch(B + "/bc250-update.sh"); ok(g.status === 404, "script route 404 while disabled");

  // enable
  $("osu-en").checked = true; $("osu-en").dispatchEvent(new w.Event("change"));
  await until(() => shown("osu-st"), 4000, "enabled");
  ok((await posts()).some(x => x._p === "/api/osupdate" && JSON.stringify(x) === '{"enabled":true,"_p":"/api/osupdate"}'), "switch posts {enabled:true}");
  ok(vis().every(x => x), "on: status, settings, setup, scripts shown");
  await until(() => $("osu-host").value === "192.168.1.20", 3000, "form filled");
  ok($("osu-port").value === "22" && $("osu-user").value === "gamer" && $("osu-from").value === "03:00" && $("osu-len").value === "120" && $("osu-after").value === "off", "form shows the stored values");
  ok([...Array(7).keys()].every(i => $("osu-d" + i).checked), "all weekdays on");
  ok($("osu-days").textContent.replace(/\s/g, "") === "MonTueWedThuFriSatSun", "days listed Mon..Sun");
  const tx = e => e.textContent.replace(/\s+/g, " ");
  ok(/2026-11-19 03:00/.test(tx($("osu-next").parentNode)) && /Idle/.test(tx($("osu-job"))), "status: next run, job idle");
  ok(/Failed/.test(tx($("osu-l-res"))) && /update exited with 1/.test(tx($("osu-l-err"))) && /2m 53s/.test(tx($("osu-l-dur"))) && /Off/.test(tx($("osu-l-end"))) && /2026-11-18 03:00/.test(tx($("osu-l-at"))), "last result: time, result, error, duration, end");
  ok(shown("osu-outd") && /conflicting files/.test($("osu-out").textContent) && w.getComputedStyle($("osu-out")).whiteSpace === "pre" && w.getComputedStyle($("osu-out")).overflow === "auto", "output tail in a scrollable pre");
  ok($("osu-outd").open === false, "output collapsed");
  ok(/ssh-ed25519 AAAAC3/.test($("osu-key").textContent) && /^Fingerprint: SHA256:/.test($("osu-fp").textContent), "controller key and fingerprint");
  ok($("osu-clk").hidden, "no clock warning when the clock is set");
  ok(/silent/.test($("osu-cf").textContent) && /off when the window starts/.test($("osu-cf").textContent) && /shut down gracefully/.test($("osu-cf").textContent) && /hands it back/.test($("osu-cf").textContent), "explanation of the behaviour");
  ok(/controller’s clock/.test($("osu-cf").textContent), "window uses the controller's clock");

  // setup block while not linked; copy style like Bluetooth Classic
  ok(shown("osu-su") && $("osu-cmd").textContent === "curl -fsS http://192.168.1.50/bc250-setup.sh | sudo sh" && $("osu-cmd").closest(".cmd").contains($("osu-cp2")) && $("osu-cp2").classList.contains("cp") && $("osu-cp2").querySelector("svg"), "setup command in a copy block");
  ok(!shown("osu-forget"), "no Forget host key without a pinned key");
  let copied = null; w.document.execCommand = c => { const ta = w.document.querySelector("body>textarea"); copied = c === "copy" && ta ? ta.value : null; return true; };
  $("osu-cp2").click();
  await until(() => toasts().some(t => /^Copied: curl -fsS/.test(t)), 3000, "copied toast");
  ok(copied === "curl -fsS http://192.168.1.50/bc250-setup.sh | sudo sh", "copy button copies the setup command");
  $("osu-cp1").click(); await until(() => /AAAAC3/.test(copied || ""), 2000, "key copy"); ok(/^ssh-ed25519 /.test(copied), "the key copies too");

  // test connection (PC off -> 409 toast), then PC on
  $("toasts").replaceChildren();
  $("osu-test2").click();
  await until(() => toasts().some(t => /the BC-250 is not on/.test(t)), 3000, "409 toast");
  ok($("toasts").querySelector(".err"), "409 shown as an error toast");
  await fetch(B + "/api/power", { method: "POST", body: JSON.stringify({ action: "on" }) }).catch(() => {});
  await w.fetch(B + "/api/power", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ action: "on" }) });
  await until(() => w.S && w.S.state === "on", 9000, "PC on");
  $("osu-test").click();
  await until(() => !shown("osu-su"), 4000, "setup hidden once linked");
  ok((await posts()).some(x => x._p === "/api/osupdate/test"), "Test connection posts /test");
  ok(shown("osu-forget") && /SHA256:mockHostKey/.test($("osu-hk").textContent) && /Linked/.test(tx($("osu-link"))), "linked: setup hidden, host key shown, Forget host key shown");

  // settings: validation and save
  const sv = $("osu-save");
  ok(sv.disabled && $("osu-uns").hidden && $("mark-osupdate").hidden, "Save disabled while nothing changed");
  type("osu-len", "10"); ok(!sv.disabled && !$("osu-uns").hidden && !$("mark-osupdate").hidden, "edit: Save enabled, unsaved marks shown");
  sv.click(); ok(/Length: 15 to 120 minutes/.test($("osu-err").textContent), "len 10 refused");
  type("osu-len", "121"); sv.click(); ok(/Length: 15 to 120/.test($("osu-err").textContent), "len 121 refused");
  type("osu-len", "15"); type("osu-host", ""); sv.click(); ok(/address/.test($("osu-err").textContent), "empty address refused");
  type("osu-host", "192.168.1.30"); type("osu-port", "70000"); sv.click(); ok(/Port/.test($("osu-err").textContent), "port 70000 refused");
  type("osu-port", "2222"); type("osu-user", "x".repeat(33)); $("osu-user").maxLength = 99; type("osu-user", "x".repeat(33)); sv.click(); ok(/user name/.test($("osu-err").textContent), "user over 32 refused");
  type("osu-user", "pc"); for (let i = 0; i < 7; i++) { $("osu-d" + i).checked = false; } $("osu-d1").dispatchEvent(new w.Event("change")); sv.click();
  ok(/at least one day/.test($("osu-err").textContent), "no day refused");
  { const n = (await posts()).filter(x => x._p === "/api/osupdate").length; ok(n === 1, "nothing posted for invalid values (only the enable call)"); }
  $("osu-d1").checked = $("osu-d3").checked = $("osu-d0").checked = true;   // Mon, Wed, Sun
  type("osu-from", "04:30"); type("osu-after", "reboot");
  sv.click();
  await until(() => $("osu-save").disabled && toasts().some(t => /Saved/.test(t)), 4000, "saved");
  { const x = (await posts()).filter(x => x._p === "/api/osupdate").pop();
    ok(JSON.stringify(x) === JSON.stringify({ host: "192.168.1.30", port: 2222, user: "pc", from: 270, len: 15, days: 11, after: "reboot", _p: "/api/osupdate" }), "saved only the changed fields: " + JSON.stringify(x)); }
  await poll();
  ok($("osu-host").value === "192.168.1.30" && $("osu-from").value === "04:30" && $("osu-after").value === "reboot" && !$("osu-d2").checked && $("osu-d3").checked, "form shows the saved values after a reload");
  ok(shown("osu-su") && !shown("osu-forget"), "host changed: not linked any more, setup shown again");
  // an edit is not overwritten by the poll
  type("osu-user", "other"); await poll(); ok($("osu-user").value === "other", "poll does not overwrite an unsaved edit");
  type("osu-user", "pc");

  // clock warning
  await patch({ clock: false }); await poll();
  ok(shown("osu-clk"), "warning when the clock is not set"); await patch({ clock: true });

  // running job: Stop appears, Run/Test disabled, edits cannot be saved, 409 mapped
  await patch({ job: { state: "updating", kind: "night", since_s: 95 } }); await poll();
  ok(/Updating, automatic, 1m 35s/.test(tx($("osu-job"))), "job state text: " + tx($("osu-job")));
  ok(shown("osu-stop") && $("osu-run").disabled && $("osu-test").disabled, "running: Stop shown, Run and Test disabled");
  type("osu-len", "30"); ok(sv.disabled, "Save disabled while a job runs"); type("osu-len", "15");
  $("toasts").replaceChildren();
  $("osu-stop").click();
  await until(() => !shown("osu-stop") && !$("osu-run").disabled, 4000, "stopped");
  ok((await posts()).some(x => x._p === "/api/osupdate/stop") && /Stopped/.test(tx($("osu-l-res"))), "Stop posts /stop; last result becomes stopped");
  $("osu-run").click();
  await until(() => shown("osu-stop"), 4000, "run");
  ok((await posts()).some(x => x._p === "/api/osupdate/run") && /Updating, manual/.test(tx($("osu-job"))), "Run now posts /run and the job shows");
  $("toasts").replaceChildren(); await patch({ job: { state: "idle", kind: "", since_s: 0 } }); await poll();
  await patch({ host_key: "SHA256:otherKey", linked: true }); await poll();
  ok(shown("osu-forget"), "Forget host key shown again once a key is pinned");
  $("osu-forget").click();
  await until(() => !shown("osu-forget"), 4000, "forgot");
  ok((await posts()).some(x => x._p === "/api/osupdate/forget_host"), "Forget host key posts /forget_host");

  // last: ok with restart needed; none yet
  await patch({ last: { kind: "night", at: "2026-11-18 03:00", result: "ok", error: "", exit: 0, duration_s: 3700, reboot_needed: true, end: "on", out_bytes: 10, tail: "bc250-update: done\n" } }); await poll();
  ok(/Succeeded/.test(tx($("osu-l-res"))) && /Yes/.test(tx($("osu-l-rb"))) && /On/.test(tx($("osu-l-end"))) && $("osu-l-err").parentNode.hidden && /1h 1m/.test(tx($("osu-l-dur"))), "ok result: restart needed, PC left on, no error row");
  await patch({ last: null }); await poll();
  ok(shown("osu-nolast") && $("osu-last").hidden && $("osu-outd").hidden, "no last result yet");

  // scripts
  ok($("osu-sh1") && $("osu-sh2") && $("osu-sh1").tagName === "DETAILS" && !$("osu-sh1").open && !$("osu-sh2").open, "two collapsed details for the scripts");
  ok(/setup script/i.test($("osu-sh1").querySelector("summary").textContent) && /\/usr\/local\/sbin\/bc250-update/.test($("osu-sh2").querySelector("summary").textContent), "labelled with where they end up");
  $("osu-sh2").open = true; $("osu-sh1").open = true;
  await until(() => /case "\$1" in/.test($("osu-sh2").querySelector("pre").textContent) && /bc250-setup/.test($("osu-sh1").querySelector("pre").textContent), 4000, "scripts loaded");
  const cs = w.getComputedStyle($("osu-sh2").querySelector("pre"));
  ok(/\n  check\)/.test($("osu-sh2").querySelector("pre").textContent) && cs.whiteSpace === "pre" && /monospace/.test(cs.fontFamily) && cs.overflow === "auto", "script shown as preformatted monospace, scrollable");

  // switch off: everything hides again
  $("osu-en").checked = false; $("osu-en").dispatchEvent(new w.Event("change"));
  await until(() => !shown("osu-st"), 4000, "disabled");
  ok(vis().every(x => !x) && (await posts()).some(x => x._p === "/api/osupdate" && x.enabled === false), "switch off posts {enabled:false} and hides the rest");

  ok(d.errors.length === 0, "no page errors " + d.errors.join("|"));
  w.close(); process.exit(fails ? 1 : 0);
})().catch(e => { console.log("FAIL exception " + e.stack); process.exit(1); });
