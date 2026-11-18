// SPDX-License-Identifier: GPL-3.0-or-later
// WiFi manager on plain ESP-IDF (esp_wifi / esp_netif / event loop / mdns /
// lwIP sockets): station with stored credentials, setup AP, mDNS, and roaming
// between the access points of one network (802.11k/v/r and the IDF roaming
// app, settings in sdkconfig.defaults; see docs/FINAL_FIRMWARE_SPEC.md).
//
// Threading: the WiFi/IP event handlers run in the event-loop task and only
// set atomics. All work (reconnect, AP up/down, scan steps, mDNS, DNS
// responder) is done in wifiUpdate() in the main task with netLock() held.
#include "wifi_mgr.h"
#include <atomic>
#include <vector>
#include <string.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_roaming.h>
#include <mdns.h>
#include <lwip/sockets.h>
#include "settings.h"
#include "eventlog.h"
#include "util.h"
#include "time_mgr.h"
#include "web.h"
#include "ble.h"
#include "rf.h"

static const char *AP_SSID = "BC250-Setup";
static const char *AP_PASS = "bc250setup";
static const uint32_t AP_AFTER_MS = 30000;   // start AP if STA down this long
static const uint32_t AP_KEEP_MS = 60000;    // keep AP this long after STA is up
static const uint32_t RECONNECT_MS = 1000;   // pause between connection attempts
static const uint32_t STUCK_MS = 15000;      // our connect refused this long: the station is stuck
static const uint32_t SCAN_TIMEOUT_MS = 3000;   // one channel never takes this long
static const int MAX_NETS = 30;
static const uint32_t ROAM_WAIT_MS = 10000;     // a roam must be back this fast, else it was a drop
static const uint32_t ROAM_EVAL_MS = 100;       // how often the pause policy is evaluated
static const uint32_t HOLD_LOWRSSI_MS = 4000;   // roaming scan expected (13 channels, home-channel dwell)
static const uint32_t HOLD_ROAM_MS = 1500;      // the stack reconnects to another AP
static const int WEAK_DBM = -70;                // below this the roaming app is asked again ...
static const uint32_t WEAK_RECHECK_MS = 30000;  // ... this often

static std::string ssid, pass;
static std::atomic<bool> apUp{false};
static bool staWasUp = false;
static bool mdnsUp = false;
static uint32_t downSince = 0;     // STA not connected since (boot or drop)
static uint32_t upSince = 0;       // STA connected since
static bool forgetPending = false;
static bool netApplyPending = false;

static esp_netif_t *staNetif = nullptr, *apNetif = nullptr;

// Set by the event handlers (event-loop task), consumed by wifiUpdate().
static std::atomic<bool> evLinkUp{false};     // station has an IP
static std::atomic<bool> evDisc{false};       // STA_DISCONNECTED seen
static std::atomic<bool> evScanDone{false};
static std::atomic<bool> evAssoc{false};      // associated with an AP (the IP may still be coming)
static std::atomic<bool> evConn{false};       // STA_CONNECTED seen
static std::atomic<uint32_t> evRoamAt{0};     // time of a roam-type disconnect (0 = none)
static std::atomic<int> evLowRssi{0};         // weak-signal event (dBm), 0 = none
static std::atomic<uint32_t> holdUntil{0};    // the radio is expected busy (roaming scan / roam) until then

// Roaming bookkeeping (main task)
static std::string lastBssid;                 // AP of the last association
static int lastRssi = 0;                      // signal sampled about once a second while up
static uint32_t rssiAt = 0;
static volatile uint32_t roamCount = 0;       // roams since boot
static bool roamAllowed = true;               // false while paused (upload, Learn, WiFi scan)
static uint32_t roamEvalAt = 0;
static uint32_t assocNoIpSince = 0;
static uint32_t weakCheckAt = 0;              // next weak-signal re-check (0 = signal not weak)
static uint32_t weakAskedAt = 0;              // our last re-check (its event is not logged)
static bool roamHaveBase = false;
static struct roam_config roamCfg;            // the app's config; trigger flags are switched off while paused
static struct { bool low, scan, legacy, btm, rrm; } roamBase;

// Connection attempts: staWanted = we want a link (false after wifiForget).
static bool staWanted = false;
static bool reconnectArmed = false;
static uint32_t reconnectDue = 0;
static uint32_t refusedSince = 0;   // our connect refused ("sta is connecting") since, 0 = not
static bool stuckLogged = false;    // the restart below logged once per outage

// Captive DNS (setup AP): UDP 53, answers every A query with 192.168.4.1.
static int dnsSock = -1;

// Fixed-address settings (section 20), stored in the "wifi" namespace.
// DHCP is the default. Host byte order, 0 = not set.
static bool netDhcp = true;
static uint32_t netIp, netMask, netGw, netDns1, netDns2;

struct Net { std::string ssid; int rssi; bool secure; };
static std::vector<Net> nets;   // heap (reserved at the first scan): static RAM must stay below the 2.4G capture bank
static std::atomic<bool> scanning{false};
static bool scanWaiting = false;      // first start (or a retry) is due at scanNextTry
static uint32_t scanNextTry = 0;
static uint32_t scanGiveUp = 0;       // stop retrying a busy channel at this time
// Scans go one channel at a time: the WiFi driver keeps every network a scan
// sees in RAM until the scan ends, which in a busy area (29 networks) took
// ~50 KB and left the board with ~4 KB free. Per channel it is a few KB.
static uint8_t scanCh = 0;
static uint32_t scanStartedAt = 0;
static const uint8_t SCAN_LAST_CH = 13;
static const int SCAN_BATCH = 16;   // records fetched per channel (rest dropped)

static uint32_t mk(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}
static std::string quadStr(uint32_t h) {   // host order; "" for 0.0.0.0
  if (!h) return std::string();
  return strPrintf("%u.%u.%u.%u", (unsigned)(h >> 24), (unsigned)((h >> 16) & 255),
                   (unsigned)((h >> 8) & 255), (unsigned)(h & 255));
}
static std::string ipStrNet(uint32_t n) { return quadStr(lwip_ntohl(n)); }   // lwIP network order

static bool parseQuad(const std::string &s, uint32_t &out) {
  int part = 0, val = 0, digits = 0;
  uint8_t o[4];
  for (size_t i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : '.';
    if (c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
      if (++digits > 3 || val > 255) return false;
    } else if (c == '.' && digits > 0 && part < 4) {
      o[part++] = (uint8_t)val; val = 0; digits = 0;
    } else {
      return false;
    }
    if (i == s.length() && part != 4) return false;
  }
  if (part != 4) return false;
  out = mk(o[0], o[1], o[2], o[3]);
  return true;
}

static void loadCreds() {
  Nvs p;
  p.open("wifi", false);   // missing namespace: defaults
  ssid = p.getStr("ssid", "");
  pass = p.getStr("pass", "");
  netDhcp = p.getBool("dhcp", true);
  netIp = netMask = netGw = netDns1 = netDns2 = 0;
  parseQuad(p.getStr("ip", ""), netIp); parseQuad(p.getStr("mask", ""), netMask);
  parseQuad(p.getStr("gw", ""), netGw); parseQuad(p.getStr("dns1", ""), netDns1);
  parseQuad(p.getStr("dns2", ""), netDns2);
  p.close();
  // A stored fixed config that does not make sense falls back to DHCP.
  if (!netDhcp && (!netIp || !netMask || !netGw)) netDhcp = true;
}

// ---- events (event-loop task: flags only) ---------------------------------
static void holdRadio(uint32_t ms) {
  uint32_t until = nowMs() + ms;
  if (!until) until = 1;
  uint32_t cur = holdUntil.load();
  if (!cur || (int32_t)(until - cur) > 0) holdUntil = until;   // wifiUpdate() clears an expired hold
}

static void onEvent(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      int reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
      evAssoc = false;
      if (reason == WIFI_REASON_ROAMING || reason == WIFI_REASON_BSS_TRANSITION_DISASSOC) {
        // A roam: the stack (or the roaming app) is already connecting to
        // another AP. ROAMING keeps the IP (no LOST_IP follows); the app's
        // own reconnect drops it and DHCP runs again. No reconnect of ours.
        uint32_t t = nowMs(); evRoamAt = t ? t : 1;
        holdRadio(HOLD_ROAM_MS);
        if (reason != WIFI_REASON_ROAMING) evLinkUp = false;
      } else {
        evLinkUp = false; evDisc = true;
      }
    } else if (id == WIFI_EVENT_STA_CONNECTED) { evAssoc = true; evConn = true; holdRadio(HOLD_ROAM_MS); }
    else if (id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
      int r = data ? ((wifi_event_bss_rssi_low_t *)data)->rssi : -128;
      evLowRssi = r ? r : -128;
      holdRadio(HOLD_LOWRSSI_MS);   // the roaming app scans now
    }
    else if (id == WIFI_EVENT_SCAN_DONE) evScanDone = true;
  } else if (base == IP_EVENT) {
    if (id == IP_EVENT_STA_GOT_IP) evLinkUp = true;
    else if (id == IP_EVENT_STA_LOST_IP) evLinkUp = false;
  }
}

// ---- captive DNS ------------------------------------------------------------
static void dnsStart() {
  if (dnsSock >= 0) return;
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return;
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(53);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return; }
  dnsSock = s;
}

static void dnsStop() {
  if (dnsSock >= 0) { close(dnsSock); dnsSock = -1; }
}

static void dnsService() {
  if (dnsSock < 0) return;
  for (int k = 0; k < 4; k++) {
    uint8_t buf[300];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(dnsSock, buf, 256, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n < 0) return;
    if (n < 17) continue;
    // standard query (QR=0, opcode 0), exactly one question
    if ((buf[2] & 0xF8) != 0 || buf[4] != 0 || buf[5] != 1) continue;
    int p = 12;
    while (p < n && buf[p] != 0) {
      if (buf[p] & 0xC0) { p = n; break; }   // no compression in a question
      p += buf[p] + 1;
    }
    p += 1;                      // root label
    int qend = p + 4;            // QTYPE + QCLASS
    if (qend > n) continue;
    uint16_t qtype = (buf[p] << 8) | buf[p + 1];
    bool ans = (qtype == 1 || qtype == 255);   // A or ANY
    buf[2] = 0x84 | (buf[2] & 0x01);           // QR, AA, RD copied
    buf[3] = 0;
    buf[6] = 0; buf[7] = ans ? 1 : 0; buf[8] = buf[9] = buf[10] = buf[11] = 0;
    int len = qend;
    if (ans) {
      static const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
      memcpy(buf + len, rr, sizeof(rr));
      len += sizeof(rr);
    }
    sendto(dnsSock, buf, len, 0, (struct sockaddr *)&from, fl);
  }
}

// ---- AP / STA ----------------------------------------------------------------
static void startAp() {
  if (apUp) return;
  wifi_config_t c = {};
  strlcpy((char *)c.ap.ssid, AP_SSID, sizeof(c.ap.ssid));
  strlcpy((char *)c.ap.password, AP_PASS, sizeof(c.ap.password));
  c.ap.ssid_len = strlen(AP_SSID);
  c.ap.channel = 1;
  c.ap.authmode = WIFI_AUTH_WPA2_PSK;
  c.ap.max_connection = 4;
  c.ap.pmf_cfg.required = false;
  esp_wifi_set_config(WIFI_IF_AP, &c);
  dnsStart();
  apUp = true;
  logEvent("setup AP up");
}

// Must run before esp_wifi_connect().
static void applyNetConfig() {
  if (netDhcp) {
    esp_netif_dhcpc_start(staNetif);   // already started: harmless error
    return;
  }
  esp_netif_dhcpc_stop(staNetif);
  esp_netif_ip_info_t ii = {};
  ii.ip.addr = lwip_htonl(netIp);
  ii.netmask.addr = lwip_htonl(netMask);
  ii.gw.addr = lwip_htonl(netGw);
  esp_netif_set_ip_info(staNetif, &ii);
  esp_netif_dns_info_t d = {};
  d.ip.type = ESP_IPADDR_TYPE_V4;
  d.ip.u_addr.ip4.addr = lwip_htonl(netDns1 ? netDns1 : netGw);   // empty dns1 = gateway
  esp_netif_set_dns_info(staNetif, ESP_NETIF_DNS_MAIN, &d);
  if (netDns2) {
    d.ip.u_addr.ip4.addr = lwip_htonl(netDns2);
    esp_netif_set_dns_info(staNetif, ESP_NETIF_DNS_BACKUP, &d);
  }
}

// Sets the configuration and arranges the first connect attempt `delayMs` from now.
static void beginSta(uint32_t delayMs) {
  applyNetConfig();
  esp_netif_set_hostname(staNetif, cfg.hostname.c_str());
  wifi_config_t c = {};
  size_t sl = ssid.size() > 32 ? 32 : ssid.size();
  memcpy(c.sta.ssid, ssid.data(), sl);
  strlcpy((char *)c.sta.password, pass.c_str(), sizeof(c.sta.password));
  c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;   // strongest AP of the SSID
  c.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  c.sta.pmf_cfg.capable = true;
  c.sta.pmf_cfg.required = false;
  // Roaming: neighbour reports (k), BSS transition (v), fast transition (r).
  // No fixed BSSID or channel is set, so the stack may move between APs.
  c.sta.rm_enabled = 1;
  c.sta.btm_enabled = 1;
  c.sta.ft_enabled = 1;
  esp_wifi_set_config(WIFI_IF_STA, &c);
  staWanted = true;
  evLinkUp = false;
  reconnectArmed = true;
  reconnectDue = nowMs() + delayMs;
  downSince = nowMs();
  staWasUp = false;
}

void wifiInit() {
  loadCreds();
  staNetif = esp_netif_create_default_wifi_sta();
  apNetif = esp_netif_create_default_wifi_ap();
  // Setup AP: 192.168.4.1/24 (the default of the AP netif); the DHCP server
  // offers us as DNS so the captive responder sees the clients' lookups.
  {
    esp_netif_ip_info_t ii = {};
    ii.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ii.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ii.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    esp_netif_dhcps_stop(apNetif);
    esp_netif_set_ip_info(apNetif, &ii);
    esp_netif_dns_info_t d = {};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &d);
    uint8_t offer = 2;   // OFFER_DNS
    esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(apNetif);
  }
  esp_netif_set_hostname(staNetif, cfg.hostname.c_str());

  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&ic);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);   // we keep credentials ourselves
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, onEvent, nullptr);
  // Country: not set ("01" world safe, channels 1..11,
  // auto policy: adopts the country of the AP we connect to).

  if (!ssid.empty()) {
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // modem sleep stays on for BT coexistence
    beginSta(0);
  } else {
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    startAp();
  }
  // Connect scans (the first connect, our reconnects): 30..70 ms per active
  // channel, as the roaming app's scans, and 150 ms per passive one (one
  // beacon interval), instead of the driver's 0..120 and 360 ms: an AP
  // answers a probe within a few ms. Saves about 1.4 s at every start.
  // Only after esp_wifi_start() (refused before). Our network list scan
  // sets its own times.
  {
    wifi_scan_default_params_t sp = {};
    if (esp_wifi_get_scan_parameters(&sp) == ESP_OK) {
      sp.scan_time.active.min = 30;
      sp.scan_time.active.max = 70;
      sp.scan_time.passive = 150;
      esp_wifi_set_scan_parameters(&sp);
    }
  }
  downSince = nowMs();
}

bool wifiHaveCreds() { return !ssid.empty(); }
bool wifiConnected() { return evLinkUp.load(); }
bool wifiApUp() { return apUp.load(); }

const char *wifiModeName() {
  if (apUp && !ssid.empty()) return "ap+sta";
  if (apUp) return "ap";
  return "sta";
}

bool wifiSaveAndConnect(const std::string &s, const std::string &p, std::string &err) {
  if (s.length() < 1 || s.length() > 32) { err = "ssid must be 1..32 bytes"; return false; }
  if (p.length() > 63 || (p.length() > 0 && p.length() < 8)) {
    err = "password must be empty or 8..63 characters"; return false;
  }
  Nvs pr;
  if (pr.open("wifi", true)) {
    pr.setStr("ssid", s);
    pr.setStr("pass", p);
    pr.close();
  }
  ssid = s; pass = p;
  // Keep the setup AP up while the new link comes up, so the phone stays connected.
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  startAp();   // no-op if already up
  esp_wifi_disconnect();
  beginSta(300);   // the disconnect's own event arrives first
  logEvent("wifi: connecting to saved network");
  return true;
}

void wifiForget() { forgetPending = true; }

// ---- fixed address (section 20) -------------------------------------------
bool wifiNetSave(bool dhcp, const std::string &ip, const std::string &mask, const std::string &gw,
                 const std::string &dns1, const std::string &dns2, std::string &err) {
  uint32_t ii = 0, mi = 0, gi = 0, d1 = 0, d2 = 0;
  if (!dhcp) {
    if (!parseQuad(ip, ii)) { err = "ip: not a dotted IPv4 address"; return false; }
    if (!parseQuad(mask, mi)) { err = "mask: not a dotted IPv4 mask"; return false; }
    if (!parseQuad(gw, gi)) { err = "gw: not a dotted IPv4 address"; return false; }
    uint32_t inv = ~mi;
    // contiguous ones, at least /8, room for hosts (<= /30)
    if ((mi >> 24) != 255 || (inv & (inv + 1)) != 0 || inv < 3) { err = "mask: must be a contiguous netmask between 255.0.0.0 and 255.255.255.252"; return false; }
    if ((ii & inv) == 0 || (ii & inv) == inv) { err = "ip: is the network or broadcast address"; return false; }
    if ((gi & mi) != (ii & mi)) { err = "gw: not in the same subnet as ip"; return false; }
    if ((gi & inv) == 0 || (gi & inv) == inv || gi == ii) { err = "gw: not a usable address in the subnet"; return false; }
    if (dns1.length() && !parseQuad(dns1, d1)) { err = "dns1: not a dotted IPv4 address"; return false; }
    if (dns2.length() && !parseQuad(dns2, d2)) { err = "dns2: not a dotted IPv4 address"; return false; }
  }
  Nvs p;
  if (p.open("wifi", true)) {
    p.setBool("dhcp", dhcp);
    p.setStr("ip", dhcp ? "" : quadStr(ii));
    p.setStr("mask", dhcp ? "" : quadStr(mi));
    p.setStr("gw", dhcp ? "" : quadStr(gi));
    p.setStr("dns1", quadStr(d1));
    p.setStr("dns2", quadStr(d2));
    p.close();
  }
  netDhcp = dhcp;
  if (dhcp) { netIp = netMask = netGw = netDns1 = netDns2 = 0; }
  else { netIp = ii; netMask = mi; netGw = gi; netDns1 = d1; netDns2 = d2; }
  netApplyPending = true;   // reconnect from wifiUpdate(), after the answer is out
  logEvent(dhcp ? "network: DHCP" : "network: fixed address");
  return true;
}

void wifiNetToJson(JsonObject out) {
  out["dhcp"] = netDhcp;
  out["ip"] = quadStr(netIp); out["mask"] = quadStr(netMask); out["gw"] = quadStr(netGw);
  out["dns1"] = quadStr(netDns1); out["dns2"] = quadStr(netDns2);
  JsonObject c = out["cur"].to<JsonObject>();
  bool up = wifiConnected();
  esp_netif_ip_info_t ii = {};
  esp_netif_dns_info_t d1 = {}, d2 = {};
  if (up) {
    esp_netif_get_ip_info(staNetif, &ii);
    esp_netif_get_dns_info(staNetif, ESP_NETIF_DNS_MAIN, &d1);
    esp_netif_get_dns_info(staNetif, ESP_NETIF_DNS_BACKUP, &d2);
  }
  c["ip"] = up ? ipStrNet(ii.ip.addr) : std::string();
  c["mask"] = up ? ipStrNet(ii.netmask.addr) : std::string();
  c["gw"] = up ? ipStrNet(ii.gw.addr) : std::string();
  c["dns1"] = up && d1.ip.type == ESP_IPADDR_TYPE_V4 ? ipStrNet(d1.ip.u_addr.ip4.addr) : std::string();
  c["dns2"] = up && d2.ip.type == ESP_IPADDR_TYPE_V4 ? ipStrNet(d2.ip.u_addr.ip4.addr) : std::string();
}

// ---- roaming policy ------------------------------------------------------------
// Pause: the roaming app's triggers (weak-signal event, periodic scan, BTM
// query, directed reconnect) are switched off through its config, and its own
// reconnect after a disconnect is disabled. Resume restores them. Not
// pausable (inside the IDF): a scan already running, and a BSS transition or
// 802.11r move that the AP itself starts.
static bool roamLoadBase() {
  if (roamHaveBase) return true;
  if (roam_get_config_params(&roamCfg) != ESP_OK || roamCfg.backoff_time == 0) return false;   // app not running
  roamBase.low = roamCfg.low_rssi_roam_trigger;
  roamBase.scan = roamCfg.scan_monitor;
  roamBase.legacy = roamCfg.legacy_roam_enabled;
  roamBase.btm = roamCfg.btm_roaming_enabled;
  roamBase.rrm = roamCfg.rrm_monitor;
  roamHaveBase = true;
  return true;
}

void wifiRoamAllow(bool on) {
  if (on == roamAllowed) return;
  if (!roamLoadBase()) { roamEvalAt = nowMs() + 5000; return; }   // app not running (yet): try again later
  roamCfg.low_rssi_roam_trigger = on && roamBase.low;
  roamCfg.scan_monitor = on && roamBase.scan;
  roamCfg.legacy_roam_enabled = on && roamBase.legacy;
  roamCfg.btm_roaming_enabled = on && roamBase.btm;
  roamCfg.rrm_monitor = on && roamBase.rrm;
  if (roam_set_config_params(&roamCfg) != ESP_OK) return;
  if (on) roam_enable_reconnect(); else roam_disable_reconnect();
  roamAllowed = on;
}

// True while a scan may be running or the stack is moving to another AP:
// our own scan, a hold after a weak-signal event, a roam (until it ends: a
// roam-type disconnect keeps the link counted as up). Any task.
bool wifiRadioBusy() {
  if (scanning.load() || evRoamAt.load()) return true;
  uint32_t h = holdUntil.load();
  return h && (int32_t)(h - nowMs()) > 0;
}

// ---- scan ----------------------------------------------------------------------
// true: a channel scan is running or waiting to be started again.
static bool scanStartCh() {
  // async, no hidden networks, active, 120 ms per channel
  while (scanCh <= SCAN_LAST_CH) {
    wifi_scan_config_t sc = {};
    sc.channel = scanCh;
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 120;
    evScanDone = false;
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e == ESP_OK) { scanStartedAt = nowMs(); scanGiveUp = 0; return true; }
    // The driver is busy (a roaming scan or a connect that started a moment
    // ago): try again for a while before skipping the channel.
    uint32_t now = nowMs();
    if (e == ESP_ERR_WIFI_STATE) {
      if (!scanGiveUp) scanGiveUp = now + 3000;
      if ((int32_t)(scanGiveUp - now) > 0) { scanWaiting = true; scanNextTry = now + 100; return true; }
    }
    scanGiveUp = 0;
    scanCh++;   // channel not allowed here (e.g. 12/13 outside the EU): skip it
  }
  return false;
}

void wifiStartScan() {
  if (scanning) return;
  wifi_mode_t m;
  if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);   // scanning needs the station
  esp_wifi_clear_ap_list();
  nets.clear();
  nets.reserve(MAX_NETS);
  scanCh = 1;
  scanGiveUp = 0;
  // Async: results are polled per channel in wifiUpdate(). The first channel
  // starts after a short wait, so that the roaming pause has reached the
  // driver (wifiUpdate evaluates it at once).
  scanning = true;
  scanWaiting = true;
  scanNextTry = nowMs() + 300;
  roamEvalAt = nowMs();   // evaluate the pause now
}

bool wifiScanning() { return scanning.load(); }

// Adds one channel's results to nets (cleared when a scan starts).
static void collectScan(const wifi_ap_record_t *r, int n) {
  for (int i = 0; i < n; i++) {
    std::string s((const char *)r[i].ssid, strnlen((const char *)r[i].ssid, sizeof(r[i].ssid)));
    if (s.empty()) continue;  // hidden
    int rssi = r[i].rssi;
    bool secure = r[i].authmode != WIFI_AUTH_OPEN;
    int dup = -1;
    for (int k = 0; k < (int)nets.size(); k++) if (nets[k].ssid == s) { dup = k; break; }
    if (dup >= 0) {
      if (rssi > nets[dup].rssi) { nets[dup].rssi = rssi; nets[dup].secure = secure; }
    } else if ((int)nets.size() < MAX_NETS) {
      nets.push_back({s, rssi, secure});
    }
  }
  for (int i = 1; i < (int)nets.size(); i++) {
    Net t = nets[i];
    int j = i - 1;
    while (j >= 0 && nets[j].rssi < t.rssi) { nets[j + 1] = nets[j]; j--; }
    nets[j + 1] = t;
  }
}

static void scanStep() {
  if (scanWaiting) {
    if ((int32_t)(nowMs() - scanNextTry) < 0) return;
    scanWaiting = false;
    scanning = scanStartCh();
    return;
  }
  bool done = evScanDone.exchange(false);
  if (!done && nowMs() - scanStartedAt >= SCAN_TIMEOUT_MS) { esp_wifi_scan_stop(); done = true; }
  if (!done) return;
  uint16_t n = 0;
  if (esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0) {
    uint16_t want = n < SCAN_BATCH ? n : SCAN_BATCH;
    wifi_ap_record_t *rec = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * want);
    if (rec) {
      uint16_t got = want;
      if (esp_wifi_scan_get_ap_records(&got, rec) == ESP_OK) collectScan(rec, got);   // also frees the driver's list
      free(rec);
    }
  }
  esp_wifi_clear_ap_list();   // frees this channel's list before the next
  scanCh++;
  scanning = scanStartCh();
}

void wifiNetworksToJson(JsonArray out) {
  for (size_t i = 0; i < nets.size(); i++) {
    JsonObject o = out.add<JsonObject>();
    o["ssid"] = cleanText(nets[i].ssid.c_str(), 32);
    o["rssi"] = nets[i].rssi;
    o["secure"] = nets[i].secure;
  }
}

std::string wifiStaIp() {
  if (!wifiConnected()) return std::string();
  esp_netif_ip_info_t ii = {};
  esp_netif_get_ip_info(staNetif, &ii);
  return ipStrNet(ii.ip.addr);
}

void wifiStateToJson(JsonObject out) {
  bool c = wifiConnected();
  wifi_ap_record_t ap = {};
  bool have = c && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  std::string cur;
  if (have) cur = std::string((const char *)ap.ssid, strnlen((const char *)ap.ssid, sizeof(ap.ssid)));
  else cur = ssid;
  esp_netif_ip_info_t ii = {};
  if (c) esp_netif_get_ip_info(staNetif, &ii);
  out["mode"] = wifiModeName();
  out["dhcp"] = netDhcp;
  out["connected"] = c;
  out["ssid"] = cleanText(cur.c_str(), 32);
  out["ip"] = c ? ipStrNet(ii.ip.addr) : std::string();
  out["rssi"] = have ? (int)ap.rssi : 0;
  out["ap"] = apUp.load();
  out["hostname"] = cfg.hostname;
  out["bssid"] = have ? strPrintf("%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                                  ap.bssid[3], ap.bssid[4], ap.bssid[5]) : std::string();
  out["channel"] = have ? (int)ap.primary : 0;
  out["roams"] = (uint32_t)roamCount;   // roams since boot
  out["roam"] = roamAllowed;            // false: paused (upload, Learn, WiFi scan)
}

void wifiApInfo(std::string &bssid, int &channel) {
  bssid.clear();
  channel = 0;
  wifi_ap_record_t ap = {};
  if (!wifiConnected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
  bssid = strPrintf("%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                    ap.bssid[3], ap.bssid[4], ap.bssid[5]);
  channel = ap.primary;
}

// Roaming bookkeeping: pause policy, log lines, drop of a roam that never ended.
static void roamUpdate(uint32_t now) {
  uint32_t h = holdUntil.load();
  if (h && (int32_t)(now - h) >= 0) holdUntil.compare_exchange_strong(h, 0);

  // Only with a network configured: in setup mode (no station) the roaming
  // app has nothing to do and its config is not touched.
  if (!ssid.empty() && (int32_t)(now - roamEvalAt) >= 0) {
    roamEvalAt = now + ROAM_EVAL_MS;
    // bleDiscoveryActive(): 2.4G Learn in 2.4G mode, the Bluetooth device
    // scan in Bluetooth mode (wake.cpp dispatches).
    wifiRoamAllow(!webOtaRunning() && !bleDiscoveryActive() && !scanning.load());
  }

  int low = evLowRssi.exchange(0);
  if (low && !(weakAskedAt && now - weakAskedAt < 3000)) logEvent("wifi: signal %d dBm, looking for a better AP", low);

  // The roaming app looks for a better AP when the signal crosses its
  // threshold, and lowers the threshold by 5 dB after each look: a board
  // whose signal stays weak (carried next to another AP while the old one
  // still reaches it) is not looked after again. While the signal stays
  // below WEAK_DBM, set the threshold above it every 30 s: the driver then
  // posts the weak-signal event and the app looks again (the same path as
  // the first time: the 2.4G capture stands aside, a roam needs 10 dB more).
  if (roamAllowed && evLinkUp && staWasUp && !evRoamAt.load() && lastRssi && lastRssi < WEAK_DBM) {
    if (!weakCheckAt) weakCheckAt = now + WEAK_RECHECK_MS;
    else if ((int32_t)(now - weakCheckAt) >= 0 && !wifiRadioBusy()) {
      weakCheckAt = now + WEAK_RECHECK_MS;
      weakAskedAt = now ? now : 1;
      esp_wifi_set_rssi_threshold(WEAK_DBM);
    }
  } else {
    weakCheckAt = 0;
  }

  if (evConn.exchange(false)) {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      std::string b = strPrintf("%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                                ap.bssid[3], ap.bssid[4], ap.bssid[5]);
      // Another AP right after a roam-type disconnect, or while the link was
      // still counted as up: a roam. After a real drop the normal path logs it.
      if (!lastBssid.empty() && b != lastBssid && (evRoamAt.load() || staWasUp)) {
        roamCount = roamCount + 1;
        logEvent("wifi roamed to %s ch %d (%d -> %d dBm)", b.c_str(), (int)ap.primary, lastRssi, (int)ap.rssi);
      }
      lastBssid = b;
      lastRssi = ap.rssi;
      rssiAt = now;
    }
    evRoamAt = 0;
  }

  uint32_t ra = evRoamAt.load();
  // Signed: the event handler may set evRoamAt after `now` was read.
  if (ra && (int32_t)(now - ra) >= (int32_t)ROAM_WAIT_MS) {
    // No connection came after the roam-type disconnect: it was a drop.
    evRoamAt = 0;
    if (!evAssoc) { evLinkUp = false; evDisc = true; }
  }

  // Associated but no IP for 20 s (a roam whose DHCP failed): start over.
  if (evAssoc && !evLinkUp && !evRoamAt.load()) {
    if (!assocNoIpSince) assocNoIpSince = now ? now : 1;
    else if (now - assocNoIpSince >= 20000) { assocNoIpSince = 0; esp_wifi_disconnect(); }
  } else {
    assocNoIpSince = 0;
  }

  if (evLinkUp && staWasUp && now - rssiAt >= 1000) {   // the signal before a roam, for the log line
    rssiAt = now;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) lastRssi = ap.rssi;
  }
}

void wifiUpdate() {
  uint32_t now = nowMs();

  if (forgetPending) {
    forgetPending = false;
    Nvs pr;
    if (pr.open("wifi", true)) { pr.clear(); pr.close(); }
    ssid.clear(); pass.clear();
    netDhcp = true; netIp = netMask = netGw = netDns1 = netDns2 = 0;   // clear() wiped the stored copy
    netApplyPending = false;
    staWanted = false;
    reconnectArmed = false;
    evRoamAt = 0; evAssoc = false; lastBssid.clear();
    esp_wifi_disconnect();
    evLinkUp = false;
    evDisc = false;
    if (mdnsUp) { mdns_free(); mdnsUp = false; }
    staWasUp = false;
    esp_wifi_set_mode(WIFI_MODE_AP);
    apUp = false;   // re-apply the AP config below
    dnsStop();
    startAp();
    logEvent("wifi forgotten");
  }

  dnsService();

  if (scanning) scanStep();
  roamUpdate(now);

  if (evDisc.exchange(false) && staWanted && !reconnectArmed) {
    reconnectArmed = true;
    reconnectDue = now + RECONNECT_MS;
  }

  if (netApplyPending) {
    netApplyPending = false;
    if (!ssid.empty()) {
      // Reconnect the station only; the setup AP (if up) is left alone.
      // mDNS and SNTP restart on the next link-up (staWasUp is reset by beginSta).
      esp_wifi_disconnect();
      beginSta(300);
    }
  }

  if (ssid.empty()) return;  // setup mode: AP stays up

  if (staWanted && reconnectArmed && (int32_t)(now - reconnectDue) >= 0) {
    reconnectArmed = false;
    // Not while associated (the IP is still coming) or while the stack is
    // moving to another AP: that connect is not ours to restart.
    if (!evLinkUp) {
      bool busy = evAssoc || evRoamAt.load();
      bool ok = false;
      if (!busy) {
        // A roam pins the station to its target AP (BSSID and channel in the
        // config). Our own reconnect after a real drop searches every channel
        // for the strongest AP, so the pin is cleared here; the stack's own
        // clearing is refused while a connect runs ("sta is connecting,
        // cannot set config"). The IDF roaming app starts a new connect after
        // every failed one, so with the pinned AP out of reach a connect is
        // always running and the pin is never cleared: ours is refused over
        // and over. Then stop the running connect (this also stops the app's
        // reconnects until ours), clear the pin and connect again.
        if (refusedSince && now - refusedSince >= STUCK_MS) {
          refusedSince = 0;
          esp_wifi_disconnect();
          if (!stuckLogged) { stuckLogged = true; logEvent("wifi: connecting stuck, starting over"); }
        }
        wifi_config_t c;
        if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && (c.sta.bssid_set || c.sta.channel)) {
          c.sta.bssid_set = 0;
          c.sta.channel = 0;
          esp_wifi_set_config(WIFI_IF_STA, &c);
        }
        ok = esp_wifi_connect() == ESP_OK;
        if (ok) refusedSince = 0;
        else if (!refusedSince) refusedSince = now ? now : 1;
      } else {
        refusedSince = 0;   // a roam or an association under way is not stuck
      }
      if (!ok) { reconnectArmed = true; reconnectDue = now + RECONNECT_MS; }
    }
  }

  bool up = wifiConnected();
  if (up) {
    refusedSince = 0;
    if (!staWasUp) {
      staWasUp = true;
      stuckLogged = false;
      upSince = now;
      esp_netif_ip_info_t ii = {};
      esp_netif_get_ip_info(staNetif, &ii);
      logEvent("wifi connected %s", ipStrNet(ii.ip.addr).c_str());
      timeLinkUp();   // time_mgr decides whether SNTP is needed
      if (mdnsUp) { mdns_free(); mdnsUp = false; }
      if (mdns_init() == ESP_OK) {
        mdnsUp = true;
        mdns_hostname_set(cfg.hostname.c_str());
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
      }
    }
    if (apUp && (int32_t)(now - upSince) >= (int32_t)AP_KEEP_MS) {
      esp_wifi_set_mode(WIFI_MODE_STA);
      apUp = false;
      dnsStop();
      logEvent("setup AP down");
    }
  } else {
    bool roaming = evRoamAt.load() != 0;   // moving to another AP: not a drop
    if (staWasUp && !roaming) {
      staWasUp = false;
      downSince = now;
      logEvent("wifi dropped");
    }
    // Signed: downSince may be set a moment after `now` was read (beginSta).
    if (!roaming && !apUp && (int32_t)(now - downSince) >= (int32_t)AP_AFTER_MS) {
      esp_wifi_set_mode(WIFI_MODE_APSTA);
      startAp();
    }
  }
}
