// SPDX-License-Identifier: GPL-3.0-or-later
// SSH client of the controller (ssh.h): key and settings in NVS, one session
// at a time in its own task, outcome and memory figures for osupd.cpp.
#include "ssh.h"
#include "ssh_client.h"
#include "util.h"
#include "eventlog.h"
#include "wifi_mgr.h"
#include "web.h"
#include "radio.h"
#include "monocypher.h"
#include <atomic>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <psa/crypto.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mdns.h>

// ---- platform hooks of ssh_client.cpp ---------------------------------------

void sshRandom(uint8_t *buf, size_t n) { esp_fill_random(buf, n); }   // hardware RNG, RF is on

struct SshSha256 { psa_hash_operation_t op; };

SshSha256 *sshSha256New() {
  SshSha256 *h = (SshSha256 *)calloc(1, sizeof(SshSha256));
  if (!h) return nullptr;
  h->op = psa_hash_operation_init();
  if (psa_hash_setup(&h->op, PSA_ALG_SHA_256) != PSA_SUCCESS) { free(h); return nullptr; }
  return h;
}
void sshSha256Update(SshSha256 *h, const void *p, size_t n) { psa_hash_update(&h->op, (const uint8_t *)p, n); }
void sshSha256Final(SshSha256 *h, uint8_t out[32]) {
  size_t len = 0;
  psa_hash_finish(&h->op, out, 32, &len);
}
void sshSha256Free(SshSha256 *h) {
  psa_hash_abort(&h->op);
  free(h);
}

// ---- state ------------------------------------------------------------------

static const char *NS = "ssh";
static const uint32_t STACK = 6144;        // measured use about 3.5 KB (high-water mark in the API)
static const size_t TAIL = 1024;           // output kept for the API
static const uint32_t CONNECT_MS = 5000;
static const uint32_t RETRY_MS = 10000;     // between TCP connection attempts

// The types of this file are in an unnamed namespace: settings.h has another
// struct Settings, and two definitions of one name break the program (the
// linker keeps one copy of their inline member functions).
namespace {
struct Settings { std::string host, user; uint32_t port = 22; };
}  // namespace
static Settings set;                       // netLock
static bool keyLoaded = false;             // netLock
static uint8_t pubKey[32];                 // netLock
static bool pinned = false;                // netLock
static uint8_t hostKey[32];                // netLock

namespace {
struct Mem {                               // netLock (task fields published at the end)
  uint32_t freeBefore = 0, largestBefore = 0, minFree = 0, minLargest = 0;
  uint32_t stackFree = 0, freeAfter = 0;
  uint32_t stageFree[5] = {};
  bool valid = false, afterValid = false;
};
}  // namespace
static Mem mem;

static SshOutcome outcome;                 // netLock
static bool linked = false;                // netLock, persisted
static std::atomic<bool> loggedInNow{false};
static std::atomic<bool> running{false};
static std::atomic<bool> cancelReq{false};
static std::atomic<bool> finished{false};  // task done, freeAfter not sampled yet
static uint32_t finishedAt = 0;

// ---- key and settings -------------------------------------------------------

// Loads the key, or creates and saves one (RF is on by now: true random).
static bool ensureKey() {
  if (keyLoaded) return true;
  uint8_t seed[32];
  Nvs n;
  if (!n.open(NS, true)) return false;
  if (n.getBlob("seed", seed, sizeof seed) != sizeof seed) {
    esp_fill_random(seed, sizeof seed);
    if (!n.setBlob("seed", seed, sizeof seed)) { crypto_wipe(seed, sizeof seed); return false; }
    logEvent("ssh: new key created");
  }
  sshPublicKey(seed, pubKey);
  crypto_wipe(seed, sizeof seed);
  keyLoaded = true;
  return true;
}

static bool loadSeed(uint8_t seed[32]) {
  Nvs n;
  return n.open(NS, false) && n.getBlob("seed", seed, 32) == 32;
}

void sshInit() {
  psa_crypto_init();   // done by IDF at start-up already; harmless again
  Nvs n;
  if (n.open(NS, false)) {
    set.host = n.getStr("host");
    set.user = n.getStr("user");
    set.port = n.getU32("port", 22);
    linked = n.getBool("linked", false);
    pinned = n.getBlob("hostpk", hostKey, sizeof hostKey) == sizeof hostKey;
  }
}

static void saveLinked(bool v) {
  if (linked == v) return;
  linked = v;
  Nvs n;
  if (n.open(NS, true)) n.setBool("linked", v);
}

bool sshSetTarget(const std::string &host, uint32_t port, const std::string &user, std::string &err) {
  if (running) { err = "a session is running"; return false; }
  if (host.size() > 63 || user.size() > 32 || port < 1 || port > 65535) { err = "bad value"; return false; }
  Nvs n;
  if (!n.open(NS, true) || !n.setStr("host", host) || !n.setStr("user", user) || !n.setU32("port", port)) {
    err = "could not save";
    return false;
  }
  if (host != set.host || port != set.port || user != set.user) {   // another server: pin its key anew
    n.remove("hostpk");
    pinned = false;
    saveLinked(false);
  }
  set.host = host;
  set.port = port;
  set.user = user;
  return true;
}

void sshForgetHost() {
  Nvs n;
  if (n.open(NS, true)) n.remove("hostpk");
  pinned = false;
  saveLinked(false);
}

const std::string &sshHost() { return set.host; }
uint32_t sshPort() { return set.port; }
const std::string &sshUser() { return set.user; }
bool sshLinked() { return linked; }
std::string sshHostKeyFingerprint() { return pinned ? sshFingerprint(hostKey) : std::string(); }
const SshOutcome &sshOutcome() { return outcome; }
bool sshLoggedIn() { return loggedInNow.load(); }

bool sshKeyInfo(std::string &line, std::string &fingerprint) {
  if (!ensureKey()) return false;
  line = sshPublicKeyLine(pubKey, "bc250-controller");
  fingerprint = sshFingerprint(pubKey);
  return true;
}

// ---- socket I/O ---------------------------------------------------------------

static bool ioSend(void *c, const uint8_t *p, size_t n) {
  int fd = *(int *)c;
  while (n) {
    int k = send(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= (size_t)k;
  }
  return true;
}

static int ioRecv(void *c, uint8_t *p, size_t n, uint32_t waitMs) {
  int fd = *(int *)c;
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(fd, &rs);
  struct timeval tv = {(time_t)(waitMs / 1000), (suseconds_t)((waitMs % 1000) * 1000)};
  int r = select(fd + 1, &rs, nullptr, nullptr, &tv);
  if (r < 0) return -1;
  if (r == 0) return 0;
  int k = recv(fd, p, n, 0);
  return k > 0 ? k : -1;
}

static uint32_t ioNow(void *) { return nowMs(); }

static bool resolve(const std::string &host, struct in_addr &a, std::string &err) {
  if (inet_aton(host.c_str(), &a)) return true;
  if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0) {
    esp_ip4_addr_t ip;
    if (mdns_query_a(host.substr(0, host.size() - 6).c_str(), 3000, &ip) != ESP_OK) {
      err = "no answer for " + host;
      return false;
    }
    a.s_addr = ip.addr;
    return true;
  }
  struct addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    err = "cannot resolve " + host;
    return false;
  }
  a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

static int connectTo(const std::string &host, uint16_t port, std::string &err) {
  struct sockaddr_in sa = {};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (!resolve(host, sa.sin_addr, err)) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) { err = "no free socket"; return -1; }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
  if (r != 0 && errno == EINPROGRESS) {
    fd_set ws;
    FD_ZERO(&ws);
    FD_SET(fd, &ws);
    struct timeval tv = {CONNECT_MS / 1000, 0};
    r = select(fd + 1, nullptr, &ws, nullptr, &tv) == 1 ? 0 : -1;
    int soErr = 0;
    socklen_t len = sizeof soErr;
    if (r == 0 && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0 || soErr)) r = -1;
  }
  if (r != 0) {
    close(fd);
    err = strPrintf("cannot connect to %s:%u", host.c_str(), (unsigned)port);
    return -1;
  }
  fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct timeval stv = {15, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
  return fd;
}

// ---- session task -------------------------------------------------------------

namespace {
struct Job {
  std::string host, mode;
  uint16_t port;
  uint32_t connectMs, limitMs;
  SshParams p;
  std::string tail;
  bool loggedIn = false;
  uint32_t minLargest = 0xFFFFFFFF, stageFree[5] = {}, outCalls = 0;
};
}  // namespace

static void sampleLargest(Job *j) {
  uint32_t l = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  if (l < j->minLargest) j->minLargest = l;
}

static void cbStage(void *c, SshStage s) {
  Job *j = (Job *)c;
  j->stageFree[s] = (uint32_t)esp_get_free_heap_size();
  if (s == SSH_STAGE_EXEC) { j->loggedIn = true; loggedInNow = true; }
  sampleLargest(j);
}

static bool cbPoll(void *c) {
  sampleLargest((Job *)c);
  return cancelReq.load();
}

static void cbOutput(void *c, int, const uint8_t *p, size_t n) {
  Job *j = (Job *)c;
  if (n >= TAIL) {
    j->tail.assign((const char *)p + n - TAIL, TAIL);
  } else {
    if (j->tail.size() + n > TAIL) j->tail.erase(0, j->tail.size() + n - TAIL);
    j->tail.append((const char *)p, n);
  }
  if ((++j->outCalls & 7) == 0) sampleLargest(j);
}

// cleanText for the output tail, keeping line breaks and tabs; a lone CR
// (progress bars) becomes a line break too.
static std::string cleanTail(const std::string &s) {
  std::string out, part;
  auto flush = [&]() { out += cleanText(part.c_str(), TAIL); part.clear(); };
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') continue;
      c = '\n';
    }
    if (c == '\n' || c == '\t') { flush(); out += c; }
    else part += c ? c : '?';
  }
  flush();
  return out;
}

static void sessionTask(void *arg) {
  Job *j = (Job *)arg;
  SshResult r;
  std::string err;
  uint32_t t0 = nowMs();
  int fd = -1;
  for (;;) {   // the PC may still be booting: keep trying until the time is up
    uint32_t at = nowMs();
    fd = connectTo(j->host, j->port, err);
    if (fd >= 0 || cancelReq.load() || nowMs() - t0 + RETRY_MS > j->connectMs) break;
    while (nowMs() - at < RETRY_MS && !cancelReq.load()) vTaskDelay(pdMS_TO_TICKS(200));
    if (cancelReq.load()) break;
  }
  if (cancelReq.load()) {
    if (fd >= 0) { close(fd); fd = -1; }
    err = "cancelled";
  }
  if (fd >= 0) {
    uint32_t spent = nowMs() - t0;
    j->p.timeoutMs = j->limitMs > spent + 5000 ? j->limitMs - spent : 5000;
    SshIo io = {&fd, ioSend, ioRecv, ioNow};
    SshCallbacks cb;
    cb.ctx = j;
    cb.output = cbOutput;
    cb.stage = cbStage;
    cb.poll = cbPoll;
    sshRun(io, j->p, cb, r);
    shutdown(fd, SHUT_RDWR);
    close(fd);
  } else {
    r.error = err;
  }
  crypto_wipe(j->p.seed, sizeof j->p.seed);
  sampleLargest(j);
  uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  uint32_t minFree = (uint32_t)esp_get_minimum_free_heap_size();
  heap_caps_monitor_local_minimum_free_size_stop();

  netLock();
  if (r.hostKeySeen && !j->p.pinned) {
    Nvs n;
    if (n.open(NS, true) && n.setBlob("hostpk", r.hostKey, sizeof r.hostKey)) {
      memcpy(hostKey, r.hostKey, sizeof hostKey);
      pinned = true;
      logEvent("ssh: host key pinned %s", sshFingerprint(r.hostKey).c_str());
    }
  }
  SshOutcome o;
  o.valid = true;
  o.loggedIn = j->loggedIn;
  o.exitStatus = r.exitStatus;
  o.error = r.error;
  o.tail = cleanTail(j->tail);
  o.rebootNeeded = j->tail.find("bc250-update: reboot needed") != std::string::npos;
  o.keyRefused = r.error.find("refused the key") != std::string::npos;
  o.hostKeyBad = r.error.find("does not match the pinned key") != std::string::npos;
  o.outBytes = r.outBytes;
  o.totalMs = r.totalMs;
  o.ok = r.ok;
  // poweroff and reboot end with the connection dropping
  if (!r.ok && o.loggedIn && (j->mode == "poweroff" || j->mode == "reboot") &&
      r.error != "cancelled" && r.error != "session time limit reached") {
    o.ok = true;
    o.error.clear();
  }
  if (o.loggedIn) saveLinked(true);
  else if (o.keyRefused || o.hostKeyBad) saveLinked(false);
  outcome = o;
  mem.minFree = minFree;
  mem.minLargest = j->minLargest;
  mem.stackFree = stackFree;
  memcpy(mem.stageFree, j->stageFree, sizeof mem.stageFree);
  mem.valid = true;
  if (o.ok) logEvent("ssh: %s ended, exit %d, %u ms", j->mode.c_str(), r.exitStatus, (unsigned)r.totalMs);
  else logEvent("ssh: %s failed: %.40s", j->mode.c_str(), r.error.c_str());
  finishedAt = nowMs();
  finished = true;
  netUnlock();
  delete j;
  running = false;
  vTaskDelete(nullptr);
}

bool sshStart(const char *mode, uint32_t connectMs, uint32_t limitMs, std::string &err) {
  if (running) { err = "a session is running"; return false; }
  if (webOtaRunning()) { err = "a firmware update is running"; return false; }
  if (!wifiConnected()) { err = "not connected to WiFi"; return false; }
  if (set.host.empty() || set.user.empty()) { err = "set host and user first"; return false; }
  if (!ensureKey()) { err = "no key (NVS)"; return false; }
  Job *j = new (std::nothrow) Job;
  if (!j) { err = "out of memory"; return false; }
  j->host = set.host;
  j->port = (uint16_t)set.port;
  j->p.user = set.user;
  j->mode = mode;
  j->p.command = mode;
  j->connectMs = connectMs;
  j->limitMs = limitMs;
  j->p.pinned = pinned;
  memcpy(j->p.hostKey, hostKey, sizeof hostKey);
  if (!loadSeed(j->p.seed)) { delete j; err = "no key (NVS)"; return false; }
  j->tail.reserve(TAIL);

  mem = Mem();
  mem.freeBefore = (uint32_t)esp_get_free_heap_size();
  mem.largestBefore = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
    heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_monitor_local_minimum_free_size_start();
  }
  cancelReq = false;
  loggedInNow = false;
  outcome = SshOutcome();
  running = true;
  finished = false;
  if (xTaskCreate(sessionTask, "ssh", STACK, j, 1, nullptr) != pdPASS) {
    heap_caps_monitor_local_minimum_free_size_stop();
    crypto_wipe(j->p.seed, sizeof j->p.seed);
    delete j;
    running = false;
    err = "out of memory";
    return false;
  }
  logEvent("ssh: %s on %s", mode, set.host.c_str());
  return true;
}

void sshStop() { if (running) cancelReq = true; }

bool sshRunning() { return running.load(); }

void sshMemToJson(JsonObject o) {
  if (mem.valid) {
    JsonObject m = o;
    m["free_before"] = mem.freeBefore;
    m["largest_before"] = mem.largestBefore;
    m["min_free"] = mem.minFree;
    m["min_largest"] = mem.minLargest;
    m["peak_use"] = mem.freeBefore > mem.minFree ? mem.freeBefore - mem.minFree : 0;
    m["stack_size"] = STACK;
    m["stack_free"] = mem.stackFree;
    if (mem.afterValid) m["free_after"] = mem.freeAfter;
    JsonArray st = m["stage_free"].to<JsonArray>();
    for (uint32_t v : mem.stageFree) st.add(v);
  }
}

// ---- main loop ------------------------------------------------------------------

#ifdef SSH_SELFTEST
// Bench build: runs the mode SSH_TEST_CMD again and again and prints the
// figures on the USB console. Server from the build flags:
//   -DSSH_SELFTEST -DSSH_TEST_HOST=\"192.168.1.20\" -DSSH_TEST_PORT=2222
//   -DSSH_TEST_USER=\"user\" -DSSH_TEST_CMD=\"check\"
#ifndef SSH_TEST_EVERY_MS
#define SSH_TEST_EVERY_MS 20000
#endif
static void selftest() {
  static uint32_t nextAt = 30000, runs = 0;
  static bool printedResult = true;
  if (finished || running) return;
  if (!printedResult && mem.afterValid) {
    printedResult = true;
    conPrintf("SSHTEST run=%u ok=%d exit=%d out=%u total=%u\n", (unsigned)runs,
              outcome.ok, outcome.exitStatus, (unsigned)outcome.outBytes, (unsigned)outcome.totalMs);
    if (!outcome.ok) conPrintf("SSHTEST error=%s\n", outcome.error.c_str());
    conPrintf("SSHMEM before=%u largest_before=%u min=%u peak=%u min_largest=%u after=%u stack_free=%u/%u\n",
              (unsigned)mem.freeBefore, (unsigned)mem.largestBefore, (unsigned)mem.minFree,
              (unsigned)(mem.freeBefore - mem.minFree), (unsigned)mem.minLargest, (unsigned)mem.freeAfter,
              (unsigned)mem.stackFree, (unsigned)STACK);
    conPrintf("SSHSTAGE %u %u %u %u %u\n", (unsigned)mem.stageFree[0], (unsigned)mem.stageFree[1],
              (unsigned)mem.stageFree[2], (unsigned)mem.stageFree[3], (unsigned)mem.stageFree[4]);
    nextAt = nowMs() + SSH_TEST_EVERY_MS;
  }
  if ((int32_t)(nowMs() - nextAt) < 0 || !wifiConnected()) return;
  set.host = SSH_TEST_HOST;
  set.port = SSH_TEST_PORT;
  set.user = SSH_TEST_USER;
  if (ensureKey()) conPrintf("SSHKEY %s\n", sshPublicKeyLine(pubKey, "bc250-selftest").c_str());
  conPrintf("SSHIDLE free=%u min=%u largest=%u radio=%s\n", (unsigned)esp_get_free_heap_size(),
            (unsigned)esp_get_minimum_free_heap_size(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), radio2g4() ? "2g4" : "ble");
  std::string err;
  runs++;
  if (sshStart(SSH_TEST_CMD, 0, 120000, err)) printedResult = false;
  else conPrintf("SSHTEST start failed: %s\n", err.c_str());
  nextAt = nowMs() + SSH_TEST_EVERY_MS;
}
#endif

void sshUpdate() {
  // The task's stack is freed by the idle task after it ends: sample later.
  if (finished && nowMs() - finishedAt > 2000) {
    finished = false;
    mem.freeAfter = (uint32_t)esp_get_free_heap_size();
    mem.afterValid = true;
  }
#ifdef SSH_SELFTEST
  selftest();
#endif
}
