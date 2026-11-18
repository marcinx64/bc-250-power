// SPDX-License-Identifier: GPL-3.0-or-later
// Web UI screenshots for the README, taken in headless Chromium against the
// test mock (firmware/bc250_controller/ui_test/mock.py), so they only ever
// show made-up data: no real addresses, networks or names.
//
//   cd tools/screenshots && bun install && bunx playwright install chromium && bun shoot.mjs
//
// Writes docs/images/screenshot-{dashboard,wake,settings,pl}.png (phone
// size, 390 x 844 CSS pixels at 2x).
import { chromium } from "playwright";
import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MOCK_DIR = ROOT + "/firmware/bc250_controller/ui_test";
const OUT = ROOT + "/docs/images";
const PORT = 8811, B = `http://127.0.0.1:${PORT}`, PASSWORD = "screenshot-demo";
const sleep = ms => new Promise(r => setTimeout(r, ms));

const mock = spawn("python3", ["mock.py", String(PORT)], {
  cwd: MOCK_DIR, stdio: "ignore",
  env: { ...process.env, MOCK_CONN: "1", MOCK_PW: PASSWORD, MOCK_RADIO: "2g4" },
});
process.on("exit", () => mock.kill());

async function waitUp() {
  for (let i = 0; i < 50; i++) {
    try { if ((await fetch(B + "/api/auth")).ok) return; } catch (e) {}
    await sleep(200);
  }
  throw new Error("mock did not start");
}

async function main() {
  await waitUp();
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, colorScheme: "dark", timezoneId: "UTC", locale: "en-US" });
  const api = async (path, body) => {
    const r = await ctx.request.post(B + path, { data: body });
    if (!r.ok()) throw new Error(`${path}: ${r.status()} ${await r.text()}`);
  };
  await api("/api/login", { password: PASSWORD });
  // A wake list like a real one, with made-up addresses
  await api("/api/rf/wake/add", { addr: "5A6B7C8D9E", mhz: 2436.5, name: "Ultimate 2C" });
  await api("/api/rf/wake/add", { addr: "0F1E2D3C4B", mhz: 2441.0, name: "Rii mini i25" });
  await api("/api/rf/wake/add", { addr: "9A8B7C", name: "BC-250 (DualShock 4)" });
  await api("/api/config/reset", {});   // the firmware's default settings
  await api("/api/power", { action: "on" });
  await sleep(4000);   // starting -> on

  const page = await ctx.newPage();
  // The mock simulates the BC-250's LED but reports the raw pin as dark, which
  // the dashboard shows as a debug note; a real board reports both the same.
  await page.route("**/api/state", async route => {
    const r = await route.fetch(), j = await r.json();
    if (j.sense !== undefined) j.sense_raw = j.sense;
    await route.fulfill({ response: r, json: j });
  });
  const prefs = async (theme, lang) => {
    await page.evaluate(([t, l]) => { localStorage.setItem("bc250_theme", t); localStorage.setItem("bc250_lang", l); }, [theme, lang]);
  };
  const shot = async (hash, file, scrollTo) => {
    await page.goto(B + "/" + hash);
    await page.reload();
    await sleep(2500);   // first polls, translations, fold animations
    // unfold folded cards (the wake lists start folded)
    for (const b of await page.$$('.fold[aria-expanded="false"]')) if (await b.isVisible()) await b.click();
    await sleep(600);
    if (scrollTo) {
      await page.evaluate(t => {   // a selector, or the text of a card heading
        const h = t.startsWith("#") ? document.querySelector(t)
          : [...document.querySelectorAll("h2,h3")].find(e => e.textContent.trim() === t && e.offsetParent);
        const card = h && (h.closest("section") || h);
        if (card) { card.scrollIntoView({ block: "start" }); window.scrollBy(0, -64); }   // below the sticky header
      }, scrollTo);
      await sleep(500);
    }
    await page.screenshot({ path: `${OUT}/${file}` });
    console.log("written", `docs/images/${file}`);
  };

  await page.goto(B + "/");
  await prefs("dark", "en");
  await shot("", "screenshot-dashboard.png");
  await shot("#wake", "screenshot-wake.png", "#wtitle");
  await prefs("neon", "en");
  await shot("#settings", "screenshot-settings.png", "Appearance");
  await prefs("dark", "pl");
  await shot("", "screenshot-pl.png");
  await browser.close();
}

main().then(() => process.exit(0), e => { console.error(e); process.exit(1); });
