#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mock of the BC-250 controller API (spec section 9). Usage: python3 mock.py [port]
Serves ui/index.html at / and ui/setup.html at /setup. Env MOCK_APONLY=1 serves setup at / ."""
import glob, gzip, json, os, re, sys, time, random, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
UI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ui")
T0 = time.time()
COUNTS = {}
LAST = []
ms = lambda: int((time.time() - T0) * 1000)
L = threading.Lock()
S = dict(state="off", err_until=0, press_at=None, sense_at=None, wifi_conn_at=None, scan_until=0, wscan_until=0, held=0,
         run_at=None, stop_at=T0, stop0=True)   # section 25: started running / stopped (time.time()); stop0 = not run since start
CFG = dict(hostname="bc250", buzzer=True, led_on=255, led_off_max=120, led_off_period_ms=6000, off_detect_ms=3000,
           start_pwr_pulse=True, start_pwr_delay_ms=2000, start_timeout_ms=15000, shutdown_timeout_ms=90000,
           ble_wake=True, ble_rearm_s=10, scan_s=120, pson_hold=not os.environ.get("MOCK_HOLDOFF"))
if os.environ.get("MOCK_NOHOLD"): del CFG["pson_hold"]
if not os.environ.get("MOCK_NOHOLDT"): CFG.update(hold_reset_s=5, hold_force_s=10)
if not os.environ.get("MOCK_NOTIME"): CFG.update(tz="UTC0", tz_name=os.environ.get("MOCK_TZNAME", ""), ntp_server="pool.ntp.org", ntp_check_h=24, quiet_on=False, quiet_from=1320, quiet_to=420)
CLOCK = dict(synced=not os.environ.get("MOCK_UNSYNCED"), source=("none" if os.environ.get("MOCK_UNSYNCED") else os.environ.get("MOCK_SOURCE", "ntp")), offset=0)
NETCFG = dict(dhcp=True, ip="", mask="", gw="", dns1="", dns2="")
CUR = dict(ip="192.168.1.208", mask="255.255.255.0", gw="192.168.1.1", dns1="192.168.1.1", dns2="")
def ipn(t):
    r = re.fullmatch(r"(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})", t or "")
    if not r or any(int(x) > 255 for x in r.groups()): return None
    n = 0
    for x in r.groups(): n = n * 256 + int(x)
    return n
def net_check(j):
    if j.get("dhcp") is True: return None
    if j.get("dhcp") is not False: return "dhcp must be boolean"
    ip, mk, gw = ipn(j.get("ip")), ipn(j.get("mask")), ipn(j.get("gw"))
    if ip is None or mk is None or gw is None: return "ip, mask and gw must be IPv4 addresses"
    b = format(mk, "032b")
    if not re.fullmatch(r"1{8,30}0+", b): return "invalid netmask"
    nw = ip & mk; bc = nw | (~mk & 0xFFFFFFFF)
    if ip in (nw, bc): return "ip is the network or broadcast address"
    if gw & mk != nw or gw in (nw, bc) or gw == ip: return "gateway must be a different host address in the subnet"
    for k in ("dns1", "dns2"):
        if j.get(k) and ipn(j[k]) is None: return k + " invalid"
    return None
RANGE = dict(led_on=(0,255), led_off_max=(0,255), led_off_period_ms=(1000,20000), off_detect_ms=(500,30000),
  start_pwr_delay_ms=(500,10000), start_timeout_ms=(3000,120000), shutdown_timeout_ms=(10000,600000),
  ble_rearm_s=(3,600), scan_s=(30,600), quiet_from=(0,1439), quiet_to=(0,1439), ntp_check_h=(0,720), hold_reset_s=(2,30), hold_force_s=(3,60))
BOOLS = ("buzzer","start_pwr_pulse","ble_wake","pson_hold","quiet_on")
LOG = [(ms(), "boot")]
G2 = os.environ.get("MOCK_RADIO") == "2g4"
OTHER = {}   # combined build: the wake list of the method not in use
if os.environ.get("MOCK_OTHER"): OTHER["ble" if G2 else "rf"] = [dict(addr="e4:00:00:00:00:11", type="public", name="Pad 2", armed=False)] if G2 else [dict(addr="11223344AA", mhz=2440.0, name="Dongle pad", armed=False), dict(addr="11223344BB", mhz=2450.0, name="Keyboard", armed=False), dict(addr="9A8B7C", kind="bt", name="BC-250", armed=False)]
WAKE = [dict(addr="A1B2C3D4E5", mhz=2468.2, name="Ultimate 2", armed=False)] if G2 else [dict(addr="e4:00:00:00:00:10", type="public", name="8BitDo", armed=False)]
LEARN = dict(start=0, on=False, kind="dongle")
def learn_get():
    t = time.time() - LEARN["start"]
    if LEARN["on"] and t >= 120: LEARN["on"] = False
    found = []
    if LEARN["on"] and t >= 2 and LEARN["kind"] == "dongle":
        two = t >= 5
        found.append(dict(addr="5A6B7C8D9E", packets=14 if two else 6, sessions=2 if two else 1, in_list=any(w["addr"] == "5A6B7C8D9E" for w in WAKE),
            freqs=[dict(mhz=2436.5, packets=9 if two else 4, sessions=2 if two else 1)] + ([dict(mhz=2424.1, packets=5, sessions=1)] if not two else []), fixed_mhz=2436.5 if two else None))
    if LEARN["on"] and t >= 2 and LEARN["kind"] == "bt":
        two = t >= 5
        # a classic Bluetooth controller paging its host (LAP only), ready by packet count
        found.append(dict(addr="9A8B7C", kind="bt", packets=7 if two else 2, ready=two, sessions=1, freqs=[], fixed_mhz=None,
            in_list=any(w["addr"] == "9A8B7C" for w in WAKE)))
    return dict(active=LEARN["on"], kind=LEARN["kind"], left_s=max(0, int(120 - t)) if LEARN["on"] else 0, found=found)
DEV = [dict(addr="e4:00:00:00:00:10", type="public", name="8BitDo", rssi=-60, kind="gamepad", appearance=964, hid=True),
       dict(addr="5c:aa:11:22:33:44", type="random", name="", rssi=-72, kind="hid", appearance=0, hid=True),
       dict(addr="aa:bb:cc:dd:ee:01", type="public", name="Headset <b>x</b>", rssi=-48),
       dict(addr="aa:bb:cc:dd:ee:02", type="public", name="Keyboard K1", rssi=-50, kind="keyboard", appearance=961, hid=True),
       dict(addr="aa:bb:cc:dd:ee:03", type="public", name="Stick", rssi=-80, kind="joystick", appearance=963, hid=True),
       dict(addr="aa:bb:cc:dd:ee:04", type="public", name="TV", rssi=-85, kind="other", appearance=0, hid=False)]
if os.environ.get("MOCK_NOCTL"): DEV[:] = [d for d in DEV if d.get("kind") in (None, "keyboard", "other")]
AUTH = dict(pw=os.environ.get("MOCK_PW"), sessions=[], fails=0, lock_until=0, debug="real")
WIFI = dict(creds=None, connected=False, ssid="", ip="", ap=True, mode="ap", connect_at=None, pw_ok="secret123")
NETS = [dict(ssid="Home", rssi=-50, secure=True), dict(ssid="Guest <i>", rssi=-70, secure=False), dict(ssid="Neighbour", rssi=-85, secure=True)]

def log(m): LOG.insert(0, (ms(), m)); del LOG[40:]
def settle():
    now = time.time()
    if S["state"] == "starting" and S["sense_at"] and now >= S["sense_at"]: S.update(state="on", run_at=now); log("BC-250 running")
    if S["state"] == "shutting_down" and S["sense_at"] and now >= S["sense_at"]:
        if AUTH.get("failsh"): S["state"] = "on"; S["sense_at"] = None; log("shutdown timeout, back to on")
        else: S.update(state="off", stop_at=now, stop0=False); log("power off: OS shutdown")
    if AUTH.get("resume") == "pending" and now >= AUTH["resume_at"]:   # section 17: the power LED decides
        if AUTH.get("failresume"): S.update(state="off", sense_at=None, stop_at=now, stop0=False); AUTH["resume"] = "lost"; log("resume failed: power LED dark, BC-250 not kept on")
        else: AUTH["resume"] = "kept"; log("resumed: BC-250 kept on")
    if WIFI["connect_at"] and now >= WIFI["connect_at"]:
        WIFI.update(connected=True, ip="192.168.1.50", ssid=WIFI["creds"][0], mode="sta", connect_at=None); log("wifi connected")
def sense_eff(st):
    m = AUTH["debug"]
    return {"lit": True, "dark": False}.get(m, st in ("on", "shutting_down"))
def wifi_up():
    return bool(WIFI["connected"] or (os.environ.get("MOCK_CONN") and not os.environ.get("MOCK_DISCONNECTED")))
LASTSYNC = dict(t=None)
if os.environ.get("MOCK_LASTSYNC"): LASTSYNC["t"] = time.time() - int(os.environ["MOCK_LASTSYNC"])
def reset_extras():
    BLEOV["v"] = None; BLEOV["rssi"] = int(os.environ.get("MOCK_RSSI", "-55"))
    WAKE[:] = WAKE[:1]; LEARN.update(on=False, start=0)
    if "hold_reset_s" in CFG: CFG.update(hold_reset_s=5, hold_force_s=10)
    if "ntp_check_h" in CFG: CFG["ntp_check_h"] = 24
    LASTSYNC["t"] = time.time() - int(os.environ["MOCK_LASTSYNC"]) if os.environ.get("MOCK_LASTSYNC") else None
    NETCFG.update(dhcp=True, ip="", mask="", gw="", dns1="", dns2="")
    unsynced = bool(os.environ.get("MOCK_UNSYNCED"))
    OSU.clear(); OSU.update(json.loads(json.dumps(OSU0)))
    CLOCK.update(synced=not unsynced, source="none" if unsynced else os.environ.get("MOCK_SOURCE", "ntp"), offset=0)
    if "tz" in CFG: CFG.update(tz="UTC0", tz_name=os.environ.get("MOCK_TZNAME", ""), ntp_server="pool.ntp.org", quiet_on=False, quiet_from=1320, quiet_to=420)
# automatic OS updates (contract: spec section 26). /__osu merges a patch into this state (tests only).
SETUP_SH = "#!/bin/sh\n# SPDX-License-Identifier: GPL-3.0-or-later\n# bc250-setup: run once on the PC\nCTL=192.168.1.50\nKEY='ssh-ed25519 AAAAC3Nza bc250-controller'\n"
UPDATE_SH = "#!/bin/sh\n# SPDX-License-Identifier: GPL-3.0-or-later\ncase \"$1\" in\n  check) echo pending ;;\n  update) echo done ;;\nesac\n"
OSU0 = dict(enabled=False, host="192.168.1.20", port=22, user="gamer", **{"from": 180}, len=120, days=127, after="off",
            pubkey="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMockKeyMockKeyMockKeyMockKeyMockKeyMockKey bc250-controller",
            fingerprint="SHA256:mockFingerprintMockFingerprintMockFingerp", host_key="", linked=False,
            setup_cmd="curl -fsS http://192.168.1.50/bc250-setup.sh | sudo sh", clock=True, next="2026-11-19 03:00",
            job=dict(state="idle", kind="", since_s=0),
            last=dict(kind="night", at="2026-11-18 03:00", result="failed", error="update exited with 1", exit=1, duration_s=173,
                      reboot_needed=False, end="off", out_bytes=32688, tail="resolving dependencies...\nerror: failed to commit transaction (conflicting files)\nbc250-update: failed\n"))
OSU = json.loads(json.dumps(OSU0))
def osu_get():
    d = json.loads(json.dumps(OSU))
    if not d["enabled"]: d.update(pubkey="", fingerprint="", next="")
    if not CLOCK["synced"]: d.update(clock=False, next="")
    return d
def osu_busy(): return OSU["job"]["state"] != "idle"
BLEOV = dict(v=None, rssi=int(os.environ.get("MOCK_RSSI", "-55")))
def ble_extra():
    if os.environ.get("MOCK_NOBLEC"): return {}
    if BLEOV["v"]: return dict(zip(("count", "present", "armed"), BLEOV["v"]))
    if os.environ.get("MOCK_BLE"):
        c, p, a = [int(x) for x in os.environ["MOCK_BLE"].split(",")]
        return dict(count=c, present=p, armed=a)
    x = dict(hosts=sum(1 for w in WAKE if w.get("kind") == "bt")) if G2 else {}
    return dict(count=len(WAKE), present=sum(1 for w in WAKE if w["addr"].endswith("10")), armed=sum(1 for w in WAKE if w.get("armed")), **x)
def clock_state():
    ep = int(time.time()) + CLOCK["offset"]
    loc = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(ep)) if CLOCK["synced"] else ""
    mins = (ep // 60) % 1440
    f, t = CFG["quiet_from"], CFG["quiet_to"]
    inq = CFG["quiet_on"] and CLOCK["synced"] and f != t and ((f <= mins < t) if f < t else (mins >= f or mins < t))
    return dict(synced=CLOCK["synced"], source=CLOCK["source"], epoch=ep, local=loc, tz_name=CFG.get("tz_name", ""), quiet=bool(inq), last_sync_s=(-1 if LASTSYNC["t"] is None else int(time.time() - LASTSYNC["t"])))
def temp_field():
    # chip_temp_c as the firmware sends it (2.2.0 on): MOCK_TEMP="absent" leaves it out
    # (older firmware), "null" = not read yet, else the value (default 41.5)
    # with it (2.2.0 on): cpu (%) and heap_total; "absent" leaves all three out
    v = os.environ.get("MOCK_TEMP", "41.5")
    if v == "absent": return {}
    return {"chip_temp_c": None if v == "null" else float(v), "cpu": int(os.environ.get("MOCK_CPU", "23")), "heap_total": 280000}

def since_fields(st, now):
    if os.environ.get("MOCK_NOSINCE"): return {}   # older firmware
    run = st in ("on", "shutting_down")
    return dict(on_s=int(now - S["run_at"]) if run else None, off_s=None if run else int(now - S["stop_at"]), off_since_start=not run and S["stop0"])
def state():
    settle(); now = time.time()
    held = 0
    if S["press_at"]: held = int((now - S["press_at"]) * 1000)
    st = S["state"]
    w = WIFI
    return dict(fw="1.0.0-mock", state=st, sense=sense_eff(st), sense_raw=False, **({} if os.environ.get("MOCK_NOHOLD") else dict(hold=CFG["pson_hold"], resumed=bool(AUTH.get("resumed")), resume=AUTH.get("resume", "none"))), debug=dict(sense=AUTH["debug"]), pson=st != "off", pwr=False, rst=False,
        button=bool(S["press_at"]) or S.get("force_held") is not None, button_held_ms=S["force_held"] if S.get("force_held") is not None else held, error=now < S["err_until"], **since_fields(st, now), uptime_s=int(now - T0), heap=150000, **temp_field(),
        wifi=dict(**({} if os.environ.get("MOCK_NOTIME") else {"dhcp": NETCFG["dhcp"]}), mode=w["mode"], connected=wifi_up(), ssid=w["ssid"] or ("Home" if wifi_up() else ""), ip=w["ip"] or ("192.168.1.50" if wifi_up() else ""), rssi=BLEOV["rssi"] if wifi_up() else 0, ap=w["ap"], hostname=CFG["hostname"], bssid="aa:bb:cc:dd:ee:ff" if wifi_up() else "", channel=6 if wifi_up() else 0, roams=1 if wifi_up() else 0),
        ble=dict(radio="2g4" if G2 else "ble", max=8, host_max=2, wake=CFG["ble_wake"], scanning=(LEARN["on"] and time.time() - LEARN["start"] < 120) if G2 else time.time() < S["scan_until"], adverts=ms() // 10, **ble_extra()),
        **({} if os.environ.get("MOCK_NOTIME") else dict(time=clock_state())))

def gate_ok():
    return S["state"] == "off" or (S["state"] == "on" and CFG.get("pson_hold") is True)
def do_boot():
    global T0
    T0 = time.time()   # uptime starts again
    AUTH["sessions"] = []
    if S["state"] == "on" and CFG.get("pson_hold"):   # PS_ON held; settle() decides kept / lost after 1 s
        AUTH.update(resumed=True, resume="pending", resume_at=time.time() + 1.0)
    else: AUTH.update(resumed=False, resume="none")
def authed(h):
    if AUTH["pw"] is None: return True
    m = re.search(r"sid=([0-9a-f]{32})", h.headers.get("Cookie") or "")
    return bool(m and m.group(1) in AUTH["sessions"])
def locked(): return max(0, int(AUTH["lock_until"] - time.time() + 0.99))

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, obj=None, ctype="application/json", raw=None):
        b = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
    def sendc(self, obj, cookie):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.send_header("Set-Cookie", cookie); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
    def err(self, code, m): self.send(code, {"ok": False, "error": m})
    def i18n(self, name):
        """Translation catalogs, like the firmware: the language list and one gzip'd file per language (no login)."""
        langs = {}
        for f in sorted(glob.glob(UI + "/i18n/*.json")):
            c = json.load(open(f, encoding="utf-8")); m = c.get("@meta", {})
            langs[os.path.basename(f)[:-5]] = (c, dict(code=m.get("code"), name=m.get("name"), english_name=m.get("english_name")))
        if os.environ.get("MOCK_NOI18N"): return self.err(404, "not found")
        if name == "index.json": obj = [v[1] for v in langs.values()]
        elif name.endswith(".json") and name[:-5] in langs:
            # untranslated (empty) texts are left out, as in the firmware image
            obj = {k: v for k, v in langs[name[:-5]][0].items() if k != "@meta" and v and (not isinstance(v, dict) or any(v.values()))}
        else: return self.err(404, "not found")
        b = gzip.compress(json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode())
        self.send_response(200); self.send_header("Content-Type", "application/json; charset=utf-8"); self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(b))); self.send_header("Cache-Control", "no-cache"); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length") or 0); return self.rfile.read(n) if n else b""
    def do_GET(self):
        p = self.path.split("?")[0]
        if p.startswith("/__ble="): BLEOV["v"] = [int(x) for x in p.split("=")[1].split(",")]; return self.send(200, {"ok": True})
        if p.startswith("/__since="):   # /__since=<s>[,1]: the current on/off period began <s> ago (,1: off since the controller started)
            a = p.split("=")[1].split(","); t = time.time() - int(a[0]); S.update(run_at=t, stop_at=t, stop0=a[1:] == ["1"]); return self.send(200, {"ok": True})
        if p.startswith("/__rssi="): BLEOV["rssi"] = int(p.split("=")[1]); return self.send(200, {"ok": True})
        if p.startswith("/__button"): S["force_held"] = None if p.endswith("none") else int(p.split("=")[1]); return self.send(200, {"ok": True})
        if p.startswith("/__failresume"): AUTH["failresume"] = p.endswith("1"); return self.send(200, {"ok": True})
        if p.startswith("/__failcfg"): AUTH["failcfg"] = p.endswith("1"); return self.send(200, {"ok": True})
        if p.startswith("/__fail"): AUTH["failsh"] = p.endswith("1"); return self.send(200, {"ok": True})
        if p == "/__scanend": S["scan_until"] = 0; LEARN["on"] = False; return self.send(200, {"ok": True})   # time up
        if p == "/__counts": return self.send(200, COUNTS)
        if p == "/__last": return self.send(200, LAST)
        if p.startswith("/i18n/"): return self.i18n(p[6:])
        COUNTS[p] = COUNTS.get(p, 0) + 1
        with L:
            if p == "/": return self.send(200, ctype="text/html", raw=open(UI + ("/setup.html" if os.environ.get("MOCK_APONLY") else "/index.html"), "rb").read())
            if p == "/setup": return self.send(200, ctype="text/html", raw=open(UI + "/setup.html", "rb").read())
            if p == "/api/auth": return self.send(200, dict(password_set=AUTH["pw"] is not None, authed=authed(self), locked_s=locked()))
            if p.startswith("/api/") and not authed(self): return self.err(401, "login required")
            if p == "/api/net":
                if os.environ.get("MOCK_NONET"): return self.err(404, "not found")
                return self.send(200, dict(NETCFG, cur=CUR))
            if p == "/api/info":
                if os.environ.get("MOCK_NOINFO"): return self.err(404, "not found")
                return self.send(200, dict(fw=os.environ.get("MOCK_FW", "1.0.8"), build="Nov 15 2026 02:53:59", idf="v5.5.1", chip="ESP32-C3", chip_rev=4, cpu_mhz=160, mac="A0:00:00:00:00:94",
                    flash_size=4194304, app_size=1447840, app_slot=1966080, partition="app0", heap=150000, heap_min=120000, **temp_field(), uptime_s=int(time.time() - T0), reset_reason=os.environ.get("MOCK_REASON", "firmware update"), marker="@@" + "BC250FW|" + os.environ.get("MOCK_FW", "1.0.8") + "|2026-11-15 02:53:59|@@", resumed=bool(AUTH.get("resumed")), **({} if os.environ.get("MOCK_NOHOLD") else dict(resume=AUTH.get("resume", "none"))), radio="2g4" if G2 else "ble"))
            if p == "/api/osupdate":
                if os.environ.get("MOCK_NOOSU"): return self.err(404, "not found")
                return self.send(200, osu_get())
            if p in ("/bc250-setup.sh", "/bc250-update.sh"):   # no login; 404 while disabled
                if not OSU["enabled"]: return self.err(404, "not found")
                return self.send(200, ctype="text/plain", raw=(SETUP_SH if "setup" in p else UPDATE_SH).encode())
            if p == "/api/state": return self.send(200, state())
            if p == "/api/config": return self.send(200, CFG)
            if p == "/api/log": return self.send(200, {"now": ms(), "events": [{"t": t, "msg": m} for t, m in LOG]})
            if p == "/api/wake/lists":
                if os.environ.get("MOCK_NOLISTS"): return self.err(404, "not found")
                rf = WAKE if G2 else OTHER.get("rf", []); nb = sum(1 for w in rf if w.get("kind") == "bt")
                return self.send(200, {"ble": len(OTHER.get("ble", [])) if G2 else len(WAKE), "dongle": len(rf) - nb, "bt": nb, "radio": "2g4" if G2 else "ble"})
            if p == "/api/rf/wake" and G2:
                return self.send(200, {"list": [dict(w, present=bool(w.get("on")), last_s=-1, enabled=w.get("enabled", True)) for w in WAKE]})
            if p == "/api/rf/learn" and G2: return self.send(200, learn_get())
            if p == "/api/ble/wake":
                return self.send(200, {"list": [dict(w, present=(w["addr"].endswith("10") and S["state"] != "x"), last_s=(4 if w["addr"].endswith("10") else -1), rssi=-60 + random.randint(-3, 3), enabled=w.get("enabled", True)) for w in WAKE]})
            if p == "/api/ble/devices":
                sc = time.time() < S["scan_until"]
                d = sorted([dict(x, rssi=x["rssi"] + random.randint(-2, 2), in_list=any(w["addr"] == x["addr"] for w in WAKE)) for x in DEV], key=lambda x: -x["rssi"]) if (S["scan_until"] or True) else []
                return self.send(200, {"scanning": sc, "left_s": max(0, int(S["scan_until"] - time.time() + 0.999)) if sc else 0, "devices": d if sc else []})
            if p == "/api/wifi/networks":
                return self.send(200, {"scanning": time.time() < S["wscan_until"], "networks": sorted(NETS, key=lambda n: -n["rssi"])})
        self.err(404, "not found")
    def do_POST(self):
        global G2
        p = self.path.split("?")[0]; raw = self.body()
        COUNTS[p] = COUNTS.get(p, 0) + 1
        if p == "/__kill": AUTH["sessions"] = []; return self.send(200, {"ok": True})
        if p == "/__reset": reset_extras(); COUNTS.clear(); LAST.clear(); AUTH.update(pw=None, sessions=[], fails=0, lock_until=0, debug="real"); WIFI.update(creds=None, connected=False, ssid="", ip="", ap=True, mode="ap", connect_at=None); S.update(state="off", sense_at=None, run_at=None, stop_at=time.time(), stop0=True); AUTH["failsh"] = False; AUTH["failcfg"] = False; AUTH["resumed"] = False; AUTH["resume"] = "none"; AUTH["failresume"] = False; CFG["pson_hold"] = (not os.environ.get("MOCK_HOLDOFF")) if "pson_hold" in CFG else None; (CFG.pop("pson_hold") if CFG.get("pson_hold", 0) is None else 0); return self.send(200, {"ok": True})
        time.sleep(float(os.environ.get("MOCK_DELAY", "0.05")))
        with L:
            settle()
            if p == "/api/login":
                try: j = json.loads(raw)
                except Exception: return self.err(400, "bad JSON")
                if locked(): return self.send(429, {"ok": False, "error": "locked", "locked_s": locked()})
                if AUTH["pw"] is not None and j.get("password") == AUTH["pw"]:
                    AUTH["fails"] = 0; sid = "%032x" % random.getrandbits(128); AUTH["sessions"] = (AUTH["sessions"] + [sid])[-4:]
                    return self.sendc({"ok": True}, "sid=%s; Path=/; HttpOnly; SameSite=Strict" % sid)
                AUTH["fails"] += 1
                if AUTH["fails"] >= 5: AUTH["fails"] = 0; AUTH["lock_until"] = time.time() + 30; return self.send(429, {"ok": False, "error": "too many attempts", "locked_s": 30})
                return self.err(401, "wrong password")
            if p.startswith("/api/") and p != "/api/auth/password" and not authed(self): return self.err(401, "login required")
            if p == "/api/auth/password":
                try: j = json.loads(raw)
                except Exception: return self.err(400, "bad JSON")
                if AUTH["pw"] is not None:
                    if not authed(self): return self.err(401, "login required")
                    if j.get("old") != AUTH["pw"]: return self.err(401, "wrong current password")
                n = j.get("new", "")
                if not 8 <= len(n) <= 64: return self.err(400, "password must be 8..64 characters")
                AUTH["pw"] = n
                m = re.search(r"sid=([0-9a-f]{32})", self.headers.get("Cookie") or "")
                AUTH["sessions"] = [m.group(1)] if m and AUTH["pw"] and m.group(1) in AUTH["sessions"] else []
                return self.send(200, {"ok": True})
            if p == "/api/logout":
                m = re.search(r"sid=([0-9a-f]{32})", self.headers.get("Cookie") or "")
                AUTH["sessions"] = [x for x in AUTH["sessions"] if not m or x != m.group(1)]; return self.send(200, {"ok": True})
            if p.startswith("/api/osupdate"):
                if os.environ.get("MOCK_NOOSU"): return self.err(404, "not found")
                try: j = json.loads(raw or b"{}")
                except Exception: return self.err(400, "bad JSON")
                LAST.append(dict(j, _p=p))
                if p == "/api/osupdate":
                    if osu_busy() and j != {"enabled": False}: return self.err(409, "an update is running")
                    chk = dict(host=lambda v: isinstance(v, str) and 1 <= len(v) <= 63, user=lambda v: isinstance(v, str) and 1 <= len(v) <= 32,
                               port=lambda v: isinstance(v, int) and 1 <= v <= 65535, len=lambda v: isinstance(v, int) and 15 <= v <= 120,
                               days=lambda v: isinstance(v, int) and 1 <= v <= 127, after=lambda v: v in ("off", "reboot"),
                               enabled=lambda v: isinstance(v, bool), **{"from": lambda v: isinstance(v, int) and 0 <= v <= 1439})
                    for k, v in j.items():
                        if k not in chk or isinstance(v, bool) != (k == "enabled") or not chk[k](v): return self.err(400, "bad " + k)
                    if j.get("enabled") is False: OSU["job"] = dict(state="idle", kind="", since_s=0)
                    if ("host" in j and j["host"] != OSU["host"]) or ("port" in j and j["port"] != OSU["port"]): OSU.update(linked=False, host_key="")
                    OSU.update(j); return self.send(200, {"ok": True})
                if not OSU["enabled"]: return self.err(409, "OS updates are off")
                if p == "/api/osupdate/stop":
                    if osu_busy(): OSU["job"] = dict(state="idle", kind="", since_s=0); OSU["last"].update(result="stopped", error="")
                    return self.send(200, {"ok": True})
                if osu_busy(): return self.err(409, "an update is running")
                if p == "/api/osupdate/run": OSU["job"] = dict(state="updating", kind="manual", since_s=42); return self.send(200, {"ok": True})
                if p == "/api/osupdate/test":
                    if S["state"] != "on": return self.err(409, "the BC-250 is not on")
                    OSU.update(linked=True, host_key="SHA256:mockHostKey"); return self.send(200, {"ok": True})
                if p == "/api/osupdate/forget_host": OSU.update(host_key="", linked=False); return self.send(200, {"ok": True})
                return self.err(404, "not found")
            if p == "/__osu":
                OSU.update(json.loads(raw or b"{}")); return self.send(200, {"ok": True})
            if p == "/api/debug":
                try: j = json.loads(raw)
                except Exception: return self.err(400, "bad JSON")
                if j.get("sense") not in ("real", "lit", "dark", "sim"): return self.err(400, "bad mode")
                AUTH["debug"] = j["sense"]; log("debug sense: " + j["sense"]); return self.send(200, {"ok": True})
            if p == "/api/restart" and os.environ.get("MOCK_RESTARTLOSE"): AUTH["sessions"] = []
            if p == "/api/update":
                if not gate_ok(): return self.err(409, "Power the BC-250 off before updating")
                assert b'name="firmware"' in raw, "no firmware field"
                do_boot()
                log("firmware update, rebooting"); self.send(200, {"ok": True}); return
            if p == "/api/factory_reset":
                if json.loads(raw or b"{}").get("confirm") is not True: return self.err(400, "confirm must be true")
                if not gate_ok(): return self.err(409, "Power the BC-250 off before restarting")
                log("factory reset"); return self.send(200, {"ok": True})
            if p == "/api/restart":
                if not gate_ok(): return self.err(409, "Power the BC-250 off before restarting")
                do_boot(); log("restart"); return self.send(200, {"ok": True})
            try: j = json.loads(raw or b"{}")
            except Exception: return self.err(400, "bad JSON")
            if p == "/api/power":
                a, st = j.get("action"), S["state"]
                if a == "on" and st == "off": S.update(state="starting", sense_at=time.time() + 3); log("power on: web")
                elif a == "shutdown" and st == "on": S.update(state="shutting_down", sense_at=time.time() + 6); log("shutdown: web")
                elif a == "reset" and st == "on": log("reset: web")
                elif a == "force_off" and st != "off":
                    S.update(state="off", sense_at=None); log("forced off: web")
                    if st != "starting": S.update(stop_at=time.time(), stop0=False)
                else: return self.err(409, "Action '%s' not allowed in state %s" % (a, st))
                return self.send(200, {"ok": True})
            if p == "/api/config/reset":   # firmware defaults, hostname and wake switch kept
                d = dict(buzzer=True, led_on=255, led_off_max=120, led_off_period_ms=6000, off_detect_ms=3000, start_pwr_pulse=True, start_pwr_delay_ms=2000,
                         start_timeout_ms=15000, shutdown_timeout_ms=90000, ble_rearm_s=30, scan_s=60, pson_hold=True, hold_reset_s=3, hold_force_s=5,
                         tz="UTC0", tz_name="", ntp_server="pool.ntp.org", ntp_check_h=24, quiet_on=False, quiet_from=1320, quiet_to=420)
                for k, v in d.items():
                    if k in CFG: CFG[k] = v
                log("settings reset to defaults"); return self.send(200, {"ok": True})
            if p == "/api/config":
                if AUTH.get("failcfg"): return self.err(400, "boom")
                for k, v in j.items():
                    if k == "hostname":
                        if not isinstance(v, str) or not re.fullmatch(r"[a-z0-9-]{1,31}", v): return self.err(400, "invalid hostname")
                    elif k == "tz":
                        if not isinstance(v, str) or not re.fullmatch(r"[A-Za-z<][A-Za-z0-9+,./:<>-]{0,46}", v) or not re.search(r"\d", v): return self.err(400, "invalid tz")
                    elif k == "tz_name":
                        if not isinstance(v, str) or not re.fullmatch(r"[\x20-\x7e]{0,39}", v): return self.err(400, "invalid tz_name")
                    elif k == "ntp_server":
                        if not isinstance(v, str) or not re.fullmatch(r"[A-Za-z0-9.-]{1,63}", v): return self.err(400, "invalid ntp_server")
                    elif k in BOOLS:
                        if not isinstance(v, bool): return self.err(400, k + " must be boolean")
                    elif k in RANGE:
                        if not isinstance(v, int) or isinstance(v, bool) or not RANGE[k][0] <= v <= RANGE[k][1]: return self.err(400, "%s out of range %s..%s" % ((k,) + RANGE[k]))
                    else: return self.err(400, "unknown setting " + k)
                if "hold_reset_s" in CFG and ("hold_reset_s" in j or "hold_force_s" in j):
                    r = j.get("hold_reset_s", CFG["hold_reset_s"]); f = j.get("hold_force_s", CFG["hold_force_s"])
                    if f < r + 1: return self.err(400, "hold_force_s must be at least hold_reset_s + 1")
                LAST.append(dict(j)); CFG.update(j); log("config saved: " + ",".join(j)); return self.send(200, {"ok": True})
            if G2 and p == "/api/rf/learn":
                if j.get("kind", "dongle") not in ("dongle", "bt"): return self.err(400, "kind must be dongle or bt")
                LEARN.update(on=bool(j.get("on")), start=time.time(), kind=j.get("kind", "dongle")); return self.send(200, {"ok": True})
            if G2 and p == "/api/rf/wake/add":
                a, m, n = j.get("addr"), j.get("mhz"), j.get("name")
                h = re.sub(r"[:\- ]", "", a).upper() if isinstance(a, str) else ""
                bt = bool(re.fullmatch(r"[0-9A-F]{6}|[0-9A-F]{12}", h))   # a Bluetooth host (its LAP)
                if not bt and not re.fullmatch(r"[0-9A-F]{10}", h): return self.err(400, "address must be 10 hex digits (2.4G device) or a Bluetooth address")
                a = h[-6:] if bt else h
                if not isinstance(n, str) or not n or len(n) > 31: return self.err(400, "name required")
                if not bt and (not isinstance(m, (int, float)) or isinstance(m, bool) or not 2400 <= m <= 2483.5): return self.err(400, "bad frequency")
                if sum(1 for w in WAKE if (w.get("kind") == "bt") == bt) >= (2 if bt else 8): return self.err(400, "list full")   # 8 devices, 2 hosts (2.2.16)
                if any(w["addr"].upper() == a.upper() for w in WAKE): return self.err(400, "already in list")
                if any(w["name"].lower() == n.lower() for w in WAKE): return self.err(400, "that name is already used")
                WAKE.append(dict(addr=a, kind="bt", name=n, armed=False) if bt else dict(addr=a, kind="dongle", mhz=m, name=n, armed=False)); log("rf wake add " + a); return self.send(200, {"ok": True})
            if G2 and p == "/api/rf/wake/remove":
                WAKE[:] = [w for w in WAKE if w["addr"] != j.get("addr")]; return self.send(200, {"ok": True})
            if p == "/api/ble/wake/add":
                if len(WAKE) >= 8: return self.err(400, "wake list full")
                if any(w["addr"] == j.get("addr") for w in WAKE): return self.err(400, "already in list")
                if j.get("name") and any(w["name"].lower() == j["name"].lower() for w in WAKE): return self.err(400, "that name is already used")
                WAKE.append(dict(addr=j["addr"], type=j["type"], name=j.get("name", ""), armed=False)); log("wake add " + j["addr"]); return self.send(200, {"ok": True})
            if p in ("/api/ble/wake/enable", "/api/rf/wake/enable"):
                if not isinstance(j.get("enabled"), bool): return self.err(400, "addr must be a string and enabled true or false")
                hit = [w for w in WAKE if w["addr"].upper() == str(j.get("addr", "")).upper()]
                if not hit: return self.err(400, "not in the list")
                hit[0]["enabled"] = j["enabled"]; return self.send(200, {"ok": True})
            if p in ("/api/ble/wake/rename", "/api/rf/wake/rename"):
                n = j.get("name")
                if not isinstance(n, str) or len(n) > 31 or (G2 and not n): return self.err(400, "name must be 1-31 characters")
                hit = [w for w in WAKE if w["addr"].upper() == str(j.get("addr", "")).upper()]
                if not hit: return self.err(400, "not in the list")
                if n and any(w is not hit[0] and w["name"].lower() == n.lower() for w in WAKE): return self.err(400, "that name is already used")
                hit[0]["name"] = n; return self.send(200, {"ok": True})
            if p == "/api/radio":
                if j.get("radio") not in ("ble", "2g4"): return self.err(400, "radio must be ble or 2g4")
                if (j["radio"] == "2g4") != G2:
                    # both lists are kept, one per method (cleared with /api/wake/clear)
                    OTHER.setdefault("ble" if G2 else "rf", [])
                    OTHER["rf" if G2 else "ble"] = list(WAKE)
                    G2 = j["radio"] == "2g4"; WAKE[:] = OTHER.get("rf" if G2 else "ble", []); log("wake method set to " + j["radio"])
                return self.send(200, {"ok": True})
            if p == "/api/wake/clear":
                if j.get("list") in ("dongle", "bt"):   # the 2.4G/BT lists
                    keep = lambda l: [w for w in l if (w.get("kind") == "bt") != (j["list"] == "bt")]
                    if G2: WAKE[:] = keep(WAKE)
                    else: OTHER["rf"] = keep(OTHER.get("rf", []))
                    log(j["list"] + " wake list cleared"); return self.send(200, {"ok": True})
                if j.get("list") != "ble": return self.err(400, "list must be ble, dongle or bt")
                if not G2: WAKE[:] = []
                else: OTHER["ble"] = []
                log("BLE wake list cleared"); return self.send(200, {"ok": True})
            if p == "/api/ble/wake/remove":
                WAKE[:] = [w for w in WAKE if w["addr"] != j.get("addr")]; return self.send(200, {"ok": True})
            if p == "/api/time/sync":
                if not wifi_up(): return self.err(409, "not connected to WiFi")
                LASTSYNC["t"] = time.time(); return self.send(200, {"ok": True})
            if p == "/api/time":
                if os.environ.get("MOCK_IGNORETIME") or (CLOCK["synced"] and CLOCK["source"] != "none"): return self.send(200, {"ok": True})
                CLOCK.update(synced=True, source="browser", offset=int(j.get("epoch", 0)) - int(time.time())); return self.send(200, {"ok": True})
            if p == "/api/net":
                if os.environ.get("MOCK_NONET"): return self.err(404, "not found")
                e = net_check(j)
                if e: return self.err(400, e)
                if j["dhcp"]: NETCFG.update(dhcp=True, ip="", mask="", gw="", dns1="", dns2="")
                else: NETCFG.update(dhcp=False, ip=j["ip"], mask=j["mask"], gw=j["gw"], dns1=j.get("dns1", ""), dns2=j.get("dns2", ""))
                LAST.append(dict(j)); return self.send(200, {"ok": True})
            if p == "/api/ble/scan": S["scan_until"] = time.time() + 120; return self.send(200, {"ok": True})
            if p == "/api/ble/scan/stop": S["scan_until"] = 0; return self.send(200, {"ok": True})
            if p == "/api/wifi/scan": S["wscan_until"] = time.time() + 2.5; return self.send(200, {"ok": True})
            if p == "/api/wifi":
                if not j.get("ssid"): return self.err(400, "ssid required")
                WIFI["creds"] = (j["ssid"], j.get("pass", ""))
                WIFI["connect_at"] = time.time() + 4 if j.get("pass", "") == WIFI["pw_ok"] or j["ssid"] == "Guest <i>" else None
                return self.send(200, {"ok": True})
            if p == "/api/wifi/forget":
                WIFI.update(creds=None, connected=False, ssid="", ip="", ap=True, mode="ap"); return self.send(200, {"ok": True})
        self.err(404, "not found")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("mock on http://127.0.0.1:%d" % port, flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
