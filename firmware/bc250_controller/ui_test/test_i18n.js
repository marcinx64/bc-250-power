// SPDX-License-Identifier: GPL-3.0-or-later
// Translations: the language list and catalogs are served without login, the page picks the language
// from the browser or from the stored choice, translated texts replace the English ones (static texts,
// toasts, server errors, plurals), a missing translation falls back to English, switching back to
// English restores the original texts, and the setup page does the same.
// Mock: MOCK_CONN=1 on port P (default 8766); AP_P = a second mock started with MOCK_APONLY=1 (setup page).
const { open, until, sleep } = require("./harness.js");
const B = "http://127.0.0.1:" + (process.env.P || 8766);
let fails = 0; const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if (!c) fails++; };
const collapse = s => s.replace(/\s+/g, " ").trim();
function page(langs, mem) {
  return w => {
    Object.defineProperty(w, "localStorage", { value: { getItem: k => mem[k] ?? null, setItem: (k, v) => { mem[k] = String(v); }, removeItem: k => { delete mem[k]; } } });
    Object.defineProperty(w.navigator, "languages", { value: langs });
  };
}
(async () => {
  await fetch(B + "/__reset", { method: "POST" });
  // ---- the endpoints
  const idx = await (await fetch(B + "/i18n/index.json")).json();
  ok(Array.isArray(idx) && idx.some(l => l.code === "pl" && l.name === "Polski" && l.english_name === "Polish"), "language list: " + JSON.stringify(idx));
  const r = await fetch(B + "/i18n/pl.json"), pl = await r.json();
  ok(r.status === 200 && r.headers.get("content-encoding") === "gzip" && pl["Dashboard"] === "Pulpit" && !("@meta" in pl), "pl.json served gzip'd, without @meta");
  ok(Object.values(pl).every(v => typeof v === "object" ? Object.values(v).every(Boolean) : !!v), "untranslated (empty) texts are left out");
  ok((await fetch(B + "/i18n/xx.json")).status === 404, "unknown language: 404");

  // ---- main page, browser language Polish
  const mem = {};
  const d = await open(B + "/", page(["pl-PL", "pl", "en"], mem)), w = d.window, $ = id => w.document.getElementById(id); w.scrollTo = () => {};
  const type = (id, v) => { $(id).value = v; $(id).dispatchEvent(new w.Event("input", { bubbles: true })); $(id).dispatchEvent(new w.Event("change", { bubbles: true })); };
  await until(() => w.document.documentElement.lang === "pl" && $("gh").textContent, 5000, "Polish login screen");
  ok($("gh").textContent === "Ustaw hasło" && w.document.querySelector("label[for=gn1]").textContent === "Nowe hasło (od 8 do 64 znaków)", "gate in Polish: " + $("gh").textContent);
  ok($("glang").value === "pl" && [...$("glang").options].map(o => o.textContent).join() === "English,Polski", "language selector on the login screen");
  ok(mem.bc250_lang === undefined, "an automatic choice is not stored");
  type("gn1", "polishpass1"); type("gn2", "polishpass1"); $("gset").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => $("gate").hidden && $("stname").textContent, 5000, "logged in");
  await until(() => [...$("toasts").children].some(t => /Hasło ustawione/.test(t.textContent)), 3000, "toast"); ok(true, "toast in Polish: Hasło ustawione");
  const nav = v => w.document.querySelector(`.nv[data-view=${v}] span`).textContent;
  await until(() => $("stname").textContent === "Wyłączone", 5000, "state name");
  ok(nav("dashboard") === "Pulpit" && nav("settings") === "Ustawienia" && nav("network") === "Sieć" && nav("log") === "Dziennik", "sidebar in Polish");
  ok(w.document.querySelector("#power h2").textContent === "Zasilanie" && w.document.querySelector("[data-act=on]").textContent === "Włącz", "dashboard cards and buttons in Polish");
  ok(/^Wyłączony: co najmniej \d+s, od włączenia zasilania sterownika$/.test($("stsub").textContent) && w.T("The BC-250 is powered off.") === "BC-250 jest wyłączony." && $("vtitle").textContent === "Pulpit" && w.document.title === "Pulpit - BC-250 Power", "state text and titles: " + $("stsub").textContent + " | " + w.document.title);
  await until(() => /urządzenie/.test($("b-count").textContent), 5000, "wake count"); ok($("b-count").textContent === "1 urządzenie", "plural one: " + $("b-count").textContent);
  ok(w.TN(2, "{n} device", "{n} devices") === "2 urządzenia" && w.TN(5, "{n} device", "{n} devices") === "5 urządzeń" && w.TN(22, "{n} device", "{n} devices") === "22 urządzenia" && w.TN(1.5, "{n} device", "{n} devices") === "1.5 urządzenia", "plural few / many / few / other");
  ok(w.T("Added {name}", { name: "Pad" }) === "Dodano: Pad", "placeholders");
  // a banner / toast from an action
  w.document.querySelector("[data-act=on]").click();
  await until(() => [...$("toasts").children].some(t => /Zażądano włączenia/.test(t.textContent)), 4000, "power toast"); ok(true, "toast from an action in Polish");
  // a server error text, found in the catalog
  ok(w.TE("already on") === "już włączony" && w.TE("hold_force_s (3) must be at least 1 more than hold_reset_s (5)") === "hold_force_s (3) musi być co najmniej o 1 większe niż hold_reset_s (5)", "server errors: whole text and pattern");
  w.location.hash = "security"; await until(() => !$("secform").closest("section").hidden, 3000);
  type("so", "wrongold123"); type("s1", "newpass1234"); type("s2", "newpass1234"); $("secform").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await until(() => [...$("toasts").children].some(t => /błędne obecne hasło/.test(t.textContent)), 4000, "server error toast"); ok(true, "an error from the server shown in Polish");
  // every static text has a translation (the page and tools/i18n.py agree on the keys)
  const miss = [];
  w.document.querySelectorAll("[data-i18n]").forEach(e => { const k = collapse(e._o); if (!(k in pl)) miss.push(k); });
  w.document.querySelectorAll("[data-i18n-attr]").forEach(e => e.dataset.i18nAttr.split(" ").forEach(a => { if (!(e._a[a] in pl)) miss.push(e._a[a]); }));
  ok(miss.length === 0, "all marked texts are in the catalog" + (miss.length ? ": " + miss.slice(0, 5).join(" | ") : ""));
  ok(w.document.querySelector("#sum-clk-h").textContent === "Zegar" && w.document.querySelector("#side").getAttribute("aria-label") === "Panel boczny", "headings and attributes translated");
  // dynamic texts built by the script
  w.location.hash = "settings"; await until(() => !$("timec").hidden, 3000);
  ok(w.document.querySelector("#c_led_on").closest(".f").querySelector("label").textContent === "Jasność, gdy włączony", "settings labels in Polish");
  // the language selector in the Appearance card; a text with no translation stays English
  ok($("langsel").value === "pl", "selector in the Appearance card");
  w.CAT["Log"] = ""; w.CAT = Object.assign({}, w.CAT); w.langApply();
  ok(nav("log") === "Log" && nav("settings") === "Ustawienia", "empty translation: English shown, the rest stays Polish");
  ok(w.T("Not in any catalog") === "Not in any catalog" && w.TN(3, "{n} gizmo", "{n} gizmos") === "3 gizmos" && w.TN(1, "{n} gizmo", "{n} gizmos") === "1 gizmo", "missing key: English text, English plural");
  // back to English: the original texts return
  type("langsel", "en");
  await until(() => w.document.documentElement.lang === "en" && nav("dashboard") === "Dashboard", 4000, "English again");
  ok(mem.bc250_lang === "en", "the choice is stored: " + mem.bc250_lang);
  const changed = [...w.document.querySelectorAll("[data-i18n]")].filter(e => e.innerHTML !== e._o);
  ok(changed.length === 0 && nav("log") === "Log" && ["Off", "Starting", "On", "Shutting down"].includes($("stname").textContent) && !/[ąćęłńóśźż]/.test($("stsub").textContent), "English texts restored (" + changed.length + " differ)");
  await until(() => !/urządzenie/.test($("b-count").textContent) && /device/.test($("b-count").textContent), 4000, "plural in English"); ok($("b-count").textContent === "1 device", "plural in English: " + $("b-count").textContent);
  ok(w.document.querySelector("#sum-clk-h").textContent === "Clock" && w.document.querySelector("#side").getAttribute("aria-label") === "Sidebar" && w.document.title === "Settings - BC-250 Power", "headings, attributes and title in English");
  type("langsel", "pl");
  await until(() => nav("dashboard") === "Pulpit", 4000, "Polish again"); ok(mem.bc250_lang === "pl" && w.document.title === "Ustawienia - BC-250 Power", "switching to Polish needs no reload");
  w.close();

  // ---- the stored choice wins over the browser language
  const o2 = await open(B + "/", page(["pl"], { bc250_lang: "en" })), w2 = o2.window; w2.scrollTo = () => {};
  await sleep(1500);
  ok(w2.document.documentElement.lang === "en" && w2.document.querySelector(".nv[data-view=dashboard] span").textContent === "Dashboard", "stored English wins over a Polish browser");
  w2.close();
  const o3 = await open(B + "/", page(["de-DE", "en-US"], {})), w3 = o3.window; w3.scrollTo = () => {};
  await sleep(1500);
  ok(w3.document.documentElement.lang === "en" && w3.document.querySelector(".nv[data-view=dashboard] span").textContent === "Dashboard", "a browser language without catalog: English");
  w3.close();
  const o4 = await open(B + "/", page(["pl"], { bc250_lang: "xx" })), w4 = o4.window; w4.scrollTo = () => {};
  await sleep(1500);
  ok(w4.document.documentElement.lang === "en", "a stored language that does not exist: English");
  w4.close();

  // ---- setup page
  const AP = process.env.AP_P ? "http://127.0.0.1:" + process.env.AP_P : "";
  if (AP) {
    await fetch(AP + "/__reset", { method: "POST" });
    const m5 = {};
    const o5 = await open(AP + "/", page(["pl"], m5)), w5 = o5.window, $5 = id => w5.document.getElementById(id); w5.scrollTo = () => {};
    await until(() => w5.document.documentElement.lang === "pl", 5000, "setup in Polish");
    ok(w5.document.querySelector("h1").textContent === "Sterownik zasilania BC-250" && $5("start").textContent === "Skonfiguruj WiFi" && w5.document.title === "Konfiguracja WiFi BC-250", "setup page texts in Polish");
    ok($5("lang").value === "pl", "language selector on the setup page");
    $5("start").click(); await until(() => !$5("s0").hidden || !$5("s2").hidden, 4000);
    ok($5("ah").textContent === "Ustaw hasło" || !$5("s2").hidden, "setup step in Polish: " + $5("ah").textContent);
    $5("lang").value = "en"; $5("lang").dispatchEvent(new w5.Event("change"));
    await until(() => w5.document.documentElement.lang === "en", 4000);
    ok(w5.document.querySelector("h1").textContent === "BC-250 power controller" && $5("ah").textContent !== "Ustaw hasło" && m5.bc250_lang === "en", "setup page back in English");
    const miss5 = [];
    w5.document.querySelectorAll("[data-i18n]").forEach(e => { if (!(collapse(e._o) in pl)) miss5.push(collapse(e._o)); });
    ok(miss5.length === 0, "all marked texts of the setup page are in the catalog" + (miss5.length ? ": " + miss5.join(" | ") : ""));
    w5.close();
  } else console.log("skipped: setup page (set AP_P to a mock started with MOCK_APONLY=1)");
  console.log(fails ? fails + " FAILED" : "all passed");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log("ERROR", e.message); process.exit(2); });
