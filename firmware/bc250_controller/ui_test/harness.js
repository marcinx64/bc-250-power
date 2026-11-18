// SPDX-License-Identifier: GPL-3.0-or-later
const { JSDOM, CookieJar, VirtualConsole } = require("jsdom");
exports.sleep = ms => new Promise(r => setTimeout(r, ms));
exports.until = async (fn, ms = 5000, what = "") => { const t = Date.now(); while (Date.now() - t < ms) { try { if (fn()) return true; } catch (e) {} await exports.sleep(50); } throw new Error("timeout: " + what + " " + fn); };
exports.open = async (url, pre) => {
  const errors = [], vc = new VirtualConsole();
  vc.on("jsdomError", e => errors.push("jsdomError: " + (e.detail || e.message)));
  vc.on("error", e => errors.push("console.error: " + e));
  const jar = new CookieJar();
  const html = await (await fetch(url)).text();
  const dom = new JSDOM(html, { url, runScripts: "dangerously", pretendToBeVisual: true, cookieJar: jar, virtualConsole: vc,
    beforeParse(w) {
      if (pre) pre(w);
      let cookie = "";
      const nf = globalThis.fetch;
      w.fetch = (u, o = {}) => {
        const ac = new AbortController();
        if (o.signal) o.signal.addEventListener("abort", () => ac.abort());
        const h = Object.assign({}, o.headers || {}); if (cookie) h.Cookie = cookie;
        return nf(new URL(u, url).href, { method: o.method, headers: h, body: o.body, signal: ac.signal }).then(r => {
          const sc = r.headers.get("set-cookie"); if (sc) cookie = sc.split(";")[0];
          return r;
        });
      };
    } });
  dom.errors = errors; return dom;
};
