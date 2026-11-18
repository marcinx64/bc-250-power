// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal SSH-2 client (ssh_client.h). Layouts: RFC 4253 (transport), 4252
// (user auth), 4254 (channels), 8731 (curve25519-sha256), 8709 (ssh-ed25519),
// OpenSSH PROTOCOL.chacha20poly1305 and PROTOCOL (strict kex).
#include "ssh_client.h"
#include <string.h>
#include <stdlib.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"

namespace {

const char *CLIENT_VERSION = "SSH-2.0-bc250ctl_1";
const char *KEX_ALGS = "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com";
const char *ED25519 = "ssh-ed25519";
const char *CIPHER = "chacha20-poly1305@openssh.com";
// Not used with an AEAD cipher, but the lists must not be empty.
const char *MACS = "hmac-sha2-256-etm@openssh.com,hmac-sha2-256";

const size_t RX_CAP = 4096;        // largest packet accepted: length, payload, padding, tag
const size_t TX_CAP = 1024;        // largest packet sent
const size_t HDR = 5;              // packet_length + padding_length
const size_t TAG = 16;             // Poly1305
const size_t MAX_PAD = 11;         // padding is 4..11 bytes for a block size of 8
const uint32_t WINDOW = 4096;      // channel window we grant the server (caps data in flight)
const uint32_t MAX_PACKET = 2048;  // largest channel data packet we take
const uint32_t POLL_MS = 1000;
const uint32_t HANDSHAKE_SILENCE_MS = 30000;

enum : uint8_t {
  MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
  MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6, MSG_EXT_INFO = 7,
  MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31,
  MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
  MSG_USERAUTH_BANNER = 53,
  MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS = 81, MSG_REQUEST_FAILURE = 82,
  MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
  MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
  MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
  MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};

void put32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}
uint32_t get32(const uint8_t *b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// Builds a message in place; ok turns false when it does not fit.
struct Out {
  uint8_t *p;
  size_t cap, n = 0;
  bool ok = true;
  Out(uint8_t *b, size_t c) : p(b), cap(c) {}
  void raw(const void *d, size_t len) {
    if (!ok || len > cap - n) { ok = false; return; }
    memcpy(p + n, d, len);
    n += len;
  }
  void u8(uint8_t v) { raw(&v, 1); }
  void u32(uint32_t v) { uint8_t b[4]; put32(b, v); raw(b, 4); }
  void str(const void *d, size_t len) { u32((uint32_t)len); raw(d, len); }
  void str(const char *s) { str(s, strlen(s)); }
  void str(const std::string &s) { str(s.data(), s.size()); }
};

// Reads a received message; ok turns false on a short or malformed one.
struct In {
  const uint8_t *p;
  size_t n, i = 0;
  bool ok = true;
  In(const uint8_t *b, size_t len) : p(b), n(len) {}
  uint8_t u8() {
    if (!ok || n - i < 1) { ok = false; return 0; }
    return p[i++];
  }
  uint32_t u32() {
    if (!ok || n - i < 4) { ok = false; return 0; }
    uint32_t v = get32(p + i);
    i += 4;
    return v;
  }
  void skip(size_t k) {
    if (!ok || n - i < k) { ok = false; return; }
    i += k;
  }
  // A string: points into the message.
  bool str(const uint8_t *&d, uint32_t &len) {
    len = u32();
    if (!ok || len > n - i) { ok = false; len = 0; d = nullptr; return false; }
    d = p + i;
    i += len;
    return true;
  }
  std::string text() {
    const uint8_t *d;
    uint32_t l;
    if (!str(d, l)) return std::string();
    return std::string((const char *)d, l);
  }
};

bool listHas(const uint8_t *list, uint32_t len, const char *name) {
  size_t nl = strlen(name);
  uint32_t s = 0;
  while (s <= len) {
    uint32_t e = s;
    while (e < len && list[e] != ',') e++;
    if (e - s == nl && memcmp(list + s, name, nl) == 0) return true;
    s = e + 1;
  }
  return false;
}

bool listFirstIs(const uint8_t *list, uint32_t len, const char *name) {
  size_t nl = strlen(name);
  return len >= nl && memcmp(list, name, nl) == 0 && (len == nl || list[nl] == ',');
}

struct Hash {
  SshSha256 *h = sshSha256New();
  ~Hash() { if (h) sshSha256Free(h); }
  void raw(const void *d, size_t n) { if (h) sshSha256Update(h, d, n); }
  void str(const void *d, size_t n) { uint8_t b[4]; put32(b, (uint32_t)n); raw(b, 4); raw(d, n); }
  void str(const char *s) { str(s, strlen(s)); }
  bool final(uint8_t out[32]) {
    if (!h) return false;
    sshSha256Final(h, out);
    return true;
  }
};

// The shared secret as an SSH mpint (RFC 8731: the 32 bytes read big-endian).
size_t mpint(const uint8_t k[32], uint8_t out[37]) {
  size_t i = 0;
  while (i < 32 && k[i] == 0) i++;
  size_t len = 32 - i;
  bool lead = len && (k[i] & 0x80);
  put32(out, (uint32_t)(len + (lead ? 1 : 0)));
  size_t o = 4;
  if (lead) out[o++] = 0;
  memcpy(out + o, k + i, len);
  return o + len;
}

// string "ssh-ed25519", string key: the public key blob
size_t keyBlob(const uint8_t pk[32], uint8_t out[51]) {
  Out o(out, 51);
  o.str(ED25519);
  o.str(pk, 32);
  return o.n;
}

std::string base64(const uint8_t *d, size_t n, bool pad) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string s;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16;
    if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    s += A[(v >> 18) & 63];
    s += A[(v >> 12) & 63];
    if (i + 1 < n) s += A[(v >> 6) & 63]; else if (pad) s += '=';
    if (i + 2 < n) s += A[v & 63]; else if (pad) s += '=';
  }
  return s;
}

void nonceOf(uint32_t seq, uint8_t nonce[8]) {
  memset(nonce, 0, 4);
  put32(nonce + 4, seq);
}

class Session {
 public:
  Session(const SshIo &io, const SshParams &p, const SshCallbacks &cb, SshResult &r)
      : io_(io), p_(p), cb_(cb), r_(r) {}
  ~Session() {
    crypto_wipe(kOut_, sizeof kOut_);
    crypto_wipe(kIn_, sizeof kIn_);
    if (rx_) { crypto_wipe(rx_, RX_CAP); free(rx_); }
    if (tx_) { crypto_wipe(tx_, TX_CAP); free(tx_); }
  }
  bool run();
  uint32_t elapsed() { return io_.now(io_.ctx) - start_; }

 private:
  const SshIo &io_;
  const SshParams &p_;
  const SshCallbacks &cb_;
  SshResult &r_;
  uint8_t *rx_ = nullptr, *tx_ = nullptr;
  uint32_t start_ = 0, lastRx_ = 0, lastPoll_ = 0;
  uint32_t seqIn_ = 0, seqOut_ = 0;
  bool encIn_ = false, encOut_ = false, strict_ = false, inKex_ = true;
  bool keepalives_ = false, kaSent_ = false;
  uint8_t kOut_[64] = {}, kIn_[64] = {};   // [0,32) payload key, [32,64) length key
  uint8_t sessionId_[32] = {};
  std::string vs_;
  const uint8_t *msg_ = nullptr;
  size_t msgLen_ = 0;
  uint32_t remoteId_ = 0;

  bool fail(const std::string &e) {
    if (r_.error.empty()) r_.error = e;
    return false;
  }
  bool unexpected(const char *where) {
    return fail(std::string("unexpected message ") + std::to_string(msg_[0]) + " " + where);
  }
  void stage(SshStage s) { if (cb_.stage) cb_.stage(cb_.ctx, s); }
  Out begin() { return Out(tx_ + HDR, TX_CAP - HDR - MAX_PAD - TAG); }
  bool send(const Out &o) { return o.ok ? sendPacket(o.n) : fail("message too large"); }
  bool sendPacket(size_t n);
  bool readExact(uint8_t *d, size_t n);
  bool recvPacket();
  bool next();
  bool versionExchange();
  bool keyExchange();
  bool derive(const uint8_t *k, size_t kl, const uint8_t h[32], char x, uint8_t out[64]);
  bool authenticate();
  bool runCommand();
};

bool Session::sendPacket(size_t n) {
  size_t aad = encOut_ ? 4 : 0;   // the length is not padded for an AEAD cipher
  size_t pad = 8 - ((HDR + n - aad) % 8);
  if (pad < 4) pad += 8;
  size_t len = 1 + n + pad;
  size_t total = 4 + len + (encOut_ ? TAG : 0);
  if (total > TX_CAP) return fail("message too large");
  uint8_t *b = tx_;
  put32(b, (uint32_t)len);
  b[4] = (uint8_t)pad;
  sshRandom(b + HDR + n, pad);
  if (encOut_) {
    uint8_t nonce[8], pk[32];
    nonceOf(seqOut_, nonce);
    crypto_chacha20_djb(b, b, 4, kOut_ + 32, nonce, 0);
    crypto_chacha20_djb(b + 4, b + 4, len, kOut_, nonce, 1);
    crypto_chacha20_djb(pk, nullptr, 32, kOut_, nonce, 0);
    crypto_poly1305(b + 4 + len, b, 4 + len, pk);
    crypto_wipe(pk, sizeof pk);
  }
  seqOut_++;
  if (!io_.send(io_.ctx, b, total)) return fail("connection lost while sending");
  return true;
}

bool Session::readExact(uint8_t *d, size_t n) {
  size_t got = 0;
  while (got < n) {
    if (elapsed() >= p_.timeoutMs) return fail("session time limit reached");
    int k = io_.recv(io_.ctx, d + got, n - got, POLL_MS);
    if (k < 0) return fail("connection closed by the server");
    uint32_t now = io_.now(io_.ctx);
    if (k > 0) {
      got += (size_t)k;
      lastRx_ = now;
    }
    // At least once a second, also while data keeps coming.
    if (cb_.poll && (k == 0 || now - lastPoll_ >= POLL_MS)) {
      lastPoll_ = now;
      if (cb_.poll(cb_.ctx)) return fail("cancelled");
    }
    if (k > 0) continue;
    uint32_t quiet = io_.now(io_.ctx) - lastRx_;
    if (!keepalives_) {
      if (quiet >= HANDSHAKE_SILENCE_MS) return fail("the server stopped answering");
      continue;
    }
    if (quiet >= 3 * p_.silenceMs) return fail("the server stopped answering");
    if (quiet >= p_.silenceMs && !kaSent_) {
      Out o = begin();
      o.u8(MSG_GLOBAL_REQUEST);
      o.str("keepalive@openssh.com");
      o.u8(1);
      if (!send(o)) return false;
      kaSent_ = true;
    }
  }
  return true;
}

bool Session::recvPacket() {
  uint8_t *b = rx_;
  if (!readExact(b, 4)) return false;
  uint8_t nonce[8];
  uint32_t len;
  if (encIn_) {
    uint8_t l[4];
    nonceOf(seqIn_, nonce);
    crypto_chacha20_djb(l, b, 4, kIn_ + 32, nonce, 0);
    len = get32(l);
  } else {
    len = get32(b);
  }
  size_t tag = encIn_ ? TAG : 0;
  if (len < 8 || len > RX_CAP - 4 - tag)
    return fail("server packet of " + std::to_string(len) + " bytes is too large or malformed");
  if ((encIn_ ? len : len + 4) % 8) return fail("server packet length is not a multiple of 8");
  if (!readExact(b + 4, len + tag)) return false;
  if (encIn_) {
    uint8_t pk[32], mac[16];
    crypto_chacha20_djb(pk, nullptr, 32, kIn_, nonce, 0);
    crypto_poly1305(mac, b, 4 + len, pk);
    crypto_wipe(pk, sizeof pk);
    if (crypto_verify16(mac, b + 4 + len) != 0) return fail("server packet failed authentication");
    crypto_chacha20_djb(b + 4, b + 4, len, kIn_, nonce, 1);
  }
  uint8_t pad = b[4];
  if (pad < 4 || (size_t)pad + 2 > len) return fail("server packet has bad padding");
  msg_ = b + HDR;
  msgLen_ = len - 1 - pad;
  seqIn_++;
  kaSent_ = false;
  return true;
}

// The next message for the caller. Outside a strict key exchange it answers
// or drops the transport-level ones (ignore, debug, global requests).
bool Session::next() {
  for (;;) {
    if (!recvPacket()) return false;
    uint8_t t = msg_[0];
    if (inKex_ && strict_) return true;
    if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPLEMENTED) continue;
    if (t == MSG_DISCONNECT) {
      In in(msg_, msgLen_);
      in.u8();
      in.u32();
      return fail("the server closed the session: " + in.text());
    }
    if (inKex_) return true;
    if (t == MSG_GLOBAL_REQUEST) {   // e.g. hostkeys-00@openssh.com
      In in(msg_, msgLen_);
      in.u8();
      in.text();
      if (in.u8()) {
        Out o = begin();
        o.u8(MSG_REQUEST_FAILURE);
        if (!send(o)) return false;
      }
      continue;
    }
    if (t == MSG_REQUEST_SUCCESS || t == MSG_REQUEST_FAILURE) continue;   // keepalive answers
    if (t == MSG_KEXINIT) return fail("the server started a key re-exchange (not supported)");
    return true;
  }
}

bool Session::versionExchange() {
  std::string line = std::string(CLIENT_VERSION) + "\r\n";
  if (!io_.send(io_.ctx, (const uint8_t *)line.data(), line.size())) return fail("connection lost while sending");
  for (int lines = 0; lines < 50; lines++) {
    std::string l;
    for (;;) {
      uint8_t c;
      if (!readExact(&c, 1)) return false;
      if (c == '\n') break;
      if (l.size() >= 255) return fail("server version line too long");
      l += (char)c;
    }
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.compare(0, 4, "SSH-") != 0) continue;   // a banner line before the version
    if (l.compare(0, 8, "SSH-2.0-") != 0 && l.compare(0, 9, "SSH-1.99-") != 0)
      return fail("not an SSH-2 server: " + l);
    vs_ = l;
    return true;
  }
  return fail("no SSH version line from the server");
}

bool Session::derive(const uint8_t *k, size_t kl, const uint8_t h[32], char x, uint8_t out[64]) {
  Hash a, b;
  a.raw(k, kl);
  a.raw(h, 32);
  a.raw(&x, 1);
  a.raw(sessionId_, 32);
  if (!a.final(out)) return false;
  b.raw(k, kl);
  b.raw(h, 32);
  b.raw(out, 32);
  return b.final(out + 32);
}

bool Session::keyExchange() {
  Hash h;   // exchange hash H, fed as the parts become known
  h.str(CLIENT_VERSION);
  h.str(vs_.data(), vs_.size());

  Out o = begin();
  uint8_t cookie[16];
  sshRandom(cookie, sizeof cookie);
  o.u8(MSG_KEXINIT);
  o.raw(cookie, sizeof cookie);
  o.str(KEX_ALGS);
  o.str(ED25519);
  o.str(CIPHER);
  o.str(CIPHER);
  o.str(MACS);
  o.str(MACS);
  o.str("none");
  o.str("none");
  o.str("");
  o.str("");
  o.u8(0);    // first_kex_packet_follows
  o.u32(0);
  if (!o.ok) return fail("message too large");
  h.str(o.p, o.n);
  if (!send(o)) return false;

  if (!recvPacket()) return false;
  if (msg_[0] != MSG_KEXINIT) return unexpected("instead of the server's KEXINIT");
  h.str(msg_, msgLen_);
  In in(msg_, msgLen_);
  in.u8();
  in.skip(16);
  const uint8_t *l[10];
  uint32_t ll[10];
  for (int i = 0; i < 10; i++) in.str(l[i], ll[i]);
  bool follows = in.u8() != 0;
  if (!in.ok) return fail("malformed KEXINIT from the server");
  const char *kex = listHas(l[0], ll[0], "curve25519-sha256") ? "curve25519-sha256"
                    : listHas(l[0], ll[0], "curve25519-sha256@libssh.org") ? "curve25519-sha256@libssh.org"
                    : nullptr;
  if (!kex) return fail("the server does not offer curve25519-sha256");
  if (!listHas(l[1], ll[1], ED25519)) return fail("the server has no ssh-ed25519 host key");
  if (!listHas(l[2], ll[2], CIPHER) || !listHas(l[3], ll[3], CIPHER))
    return fail("the server does not offer chacha20-poly1305@openssh.com");
  if (!listHas(l[6], ll[6], "none") || !listHas(l[7], ll[7], "none"))
    return fail("the server insists on compression");
  strict_ = listHas(l[0], ll[0], "kex-strict-s-v00@openssh.com");
  // A guessed first packet from the server is dropped when the guess was wrong.
  bool skipGuess = follows && !(listFirstIs(l[0], ll[0], kex) && listFirstIs(l[1], ll[1], ED25519));

  uint8_t sk[32], qc[32];
  sshRandom(sk, sizeof sk);
  crypto_x25519_public_key(qc, sk);
  o = begin();
  o.u8(MSG_KEX_ECDH_INIT);
  o.str(qc, 32);
  if (!send(o)) { crypto_wipe(sk, sizeof sk); return false; }
  if (skipGuess && !next()) { crypto_wipe(sk, sizeof sk); return false; }
  if (!next()) { crypto_wipe(sk, sizeof sk); return false; }
  if (msg_[0] != MSG_KEX_ECDH_REPLY) { crypto_wipe(sk, sizeof sk); return unexpected("instead of KEX_ECDH_REPLY"); }

  In rep(msg_, msgLen_);
  const uint8_t *ks, *qs, *sig;
  uint32_t ksl, qsl, sigl;
  rep.u8();
  rep.str(ks, ksl);
  rep.str(qs, qsl);
  rep.str(sig, sigl);
  In kb(ks, ksl);
  const uint8_t *alg, *pk;
  uint32_t algl, pkl;
  kb.str(alg, algl);
  kb.str(pk, pkl);
  if (!rep.ok || !kb.ok || qsl != 32 || pkl != 32 || algl != strlen(ED25519) || memcmp(alg, ED25519, algl) != 0) {
    crypto_wipe(sk, sizeof sk);
    return fail("malformed KEX_ECDH_REPLY from the server");
  }
  uint8_t k[32], kenc[37];
  crypto_x25519(k, sk, qs);
  crypto_wipe(sk, sizeof sk);
  uint8_t any = 0;
  for (int i = 0; i < 32; i++) any |= k[i];
  size_t kl = mpint(k, kenc);
  crypto_wipe(k, sizeof k);
  if (!any) { crypto_wipe(kenc, sizeof kenc); return fail("invalid key exchange value from the server"); }
  h.str(ks, ksl);
  h.str(qc, 32);
  h.str(qs, 32);
  h.raw(kenc, kl);
  uint8_t H[32];
  if (!h.final(H)) { crypto_wipe(kenc, sizeof kenc); return fail("out of memory"); }

  In sb(sig, sigl);
  const uint8_t *salg, *sg;
  uint32_t salgl, sgl;
  sb.str(salg, salgl);
  sb.str(sg, sgl);
  if (!sb.ok || sgl != 64 || salgl != strlen(ED25519) || memcmp(salg, ED25519, salgl) != 0 ||
      crypto_ed25519_check(sg, pk, H, 32) != 0) {
    crypto_wipe(kenc, sizeof kenc);
    return fail("the server's host key signature is invalid");
  }
  memcpy(r_.hostKey, pk, 32);
  r_.hostKeySeen = true;
  if (p_.pinned && crypto_verify32(pk, p_.hostKey) != 0) {
    crypto_wipe(kenc, sizeof kenc);
    return fail("the server's host key does not match the pinned key");
  }
  memcpy(sessionId_, H, 32);
  bool keys = derive(kenc, kl, H, 'C', kOut_) && derive(kenc, kl, H, 'D', kIn_);
  crypto_wipe(kenc, sizeof kenc);
  if (!keys) return fail("out of memory");

  o = begin();
  o.u8(MSG_NEWKEYS);
  if (!send(o)) return false;
  encOut_ = true;
  if (strict_) seqOut_ = 0;
  if (!next()) return false;
  if (msg_[0] != MSG_NEWKEYS) return unexpected("instead of NEWKEYS");
  encIn_ = true;
  if (strict_) seqIn_ = 0;
  inKex_ = false;
  return true;
}

bool Session::authenticate() {
  Out o = begin();
  o.u8(MSG_SERVICE_REQUEST);
  o.str("ssh-userauth");
  if (!send(o)) return false;
  for (;;) {
    if (!next()) return false;
    if (msg_[0] == MSG_EXT_INFO) continue;
    if (msg_[0] == MSG_SERVICE_ACCEPT) break;
    return unexpected("instead of SERVICE_ACCEPT");
  }

  uint8_t seed[32], sk[64], pk[32], blob[51], sig[64], sigBlob[83];
  memcpy(seed, p_.seed, 32);
  crypto_ed25519_key_pair(sk, pk, seed);   // wipes seed
  keyBlob(pk, blob);
  uint8_t data[512];
  Out d(data, sizeof data);
  d.str(sessionId_, 32);
  d.u8(MSG_USERAUTH_REQUEST);
  d.str(p_.user);
  d.str("ssh-connection");
  d.str("publickey");
  d.u8(1);
  d.str(ED25519);
  d.str(blob, sizeof blob);
  if (!d.ok) { crypto_wipe(sk, sizeof sk); return fail("user name too long"); }
  crypto_ed25519_sign(sig, sk, data, d.n);
  crypto_wipe(sk, sizeof sk);
  Out sb(sigBlob, sizeof sigBlob);
  sb.str(ED25519);
  sb.str(sig, 64);

  // The request is the signed data without the session id.
  o = begin();
  o.raw(data + 36, d.n - 36);
  o.str(sigBlob, sb.n);
  if (!send(o)) return false;
  for (;;) {
    if (!next()) return false;
    if (msg_[0] == MSG_USERAUTH_BANNER) continue;
    if (msg_[0] == MSG_USERAUTH_SUCCESS) return true;
    if (msg_[0] == MSG_USERAUTH_FAILURE)
      return fail("the server refused the key (not in authorized_keys of " + p_.user + "?)");
    return unexpected("during authentication");
  }
}

bool Session::runCommand() {
  Out o = begin();
  o.u8(MSG_CHANNEL_OPEN);
  o.str("session");
  o.u32(0);
  o.u32(WINDOW);
  o.u32(MAX_PACKET);
  if (!send(o)) return false;
  if (!next()) return false;
  if (msg_[0] == MSG_CHANNEL_OPEN_FAILURE) {
    In in(msg_, msgLen_);
    in.u8();
    in.u32();
    in.u32();
    return fail("the server refused the session: " + in.text());
  }
  if (msg_[0] != MSG_CHANNEL_OPEN_CONFIRMATION) return unexpected("instead of the channel confirmation");
  {
    In in(msg_, msgLen_);
    in.u8();
    in.u32();
    remoteId_ = in.u32();
    if (!in.ok) return fail("malformed channel confirmation");
  }
  o = begin();
  o.u8(MSG_CHANNEL_REQUEST);
  o.u32(remoteId_);
  o.str("exec");
  o.u8(1);
  o.str(p_.command);
  if (!send(o)) return false;

  uint32_t window = WINDOW, consumed = 0;
  bool started = false, closeSent = false;
  for (;;) {
    if (!next()) return false;
    In in(msg_, msgLen_);
    uint8_t t = in.u8();
    if (t == MSG_CHANNEL_DATA || t == MSG_CHANNEL_EXTENDED_DATA) {
      in.u32();
      int stream = 1;
      if (t == MSG_CHANNEL_EXTENDED_DATA) stream = in.u32() == 1 ? 2 : 1;
      const uint8_t *data;
      uint32_t n;
      if (!in.str(data, n)) return fail("malformed channel data");
      if (n > window) return fail("the server sent more than the channel window");
      window -= n;
      consumed += n;
      if (stream == 1) r_.outBytes += n; else r_.errBytes += n;
      if (cb_.output && n) cb_.output(cb_.ctx, stream, data, n);
      if (consumed >= WINDOW / 2 && !closeSent) {
        o = begin();
        o.u8(MSG_CHANNEL_WINDOW_ADJUST);
        o.u32(remoteId_);
        o.u32(consumed);
        if (!send(o)) return false;
        window += consumed;
        consumed = 0;
      }
    } else if (t == MSG_CHANNEL_SUCCESS) {
      started = true;
    } else if (t == MSG_CHANNEL_FAILURE) {
      if (!started) return fail("the server refused to run the command");
    } else if (t == MSG_CHANNEL_REQUEST) {
      in.u32();
      std::string type = in.text();
      bool wantReply = in.u8() != 0;
      if (type == "exit-status") {
        r_.exitStatus = (int)in.u32();
      } else if (type == "exit-signal") {
        r_.exitSignal = in.text();
      } else if (wantReply) {
        o = begin();
        o.u8(MSG_CHANNEL_FAILURE);
        o.u32(remoteId_);
        if (!send(o)) return false;
      }
    } else if (t == MSG_CHANNEL_CLOSE) {
      if (!closeSent) {
        o = begin();
        o.u8(MSG_CHANNEL_CLOSE);
        o.u32(remoteId_);
        if (!send(o)) return false;
      }
      if (!started) return fail("the server closed the channel without running the command");
      return true;
    } else if (t == MSG_CHANNEL_WINDOW_ADJUST || t == MSG_CHANNEL_EOF) {
      // we send no data; end of output is the close
    } else {
      return unexpected("on the channel");
    }
  }
}

bool Session::run() {
  start_ = io_.now(io_.ctx);
  lastRx_ = start_;
  rx_ = (uint8_t *)malloc(RX_CAP);
  tx_ = (uint8_t *)malloc(TX_CAP);
  if (!rx_ || !tx_) return fail("out of memory");
  stage(SSH_STAGE_VERSION);
  if (!versionExchange()) return false;
  stage(SSH_STAGE_KEX);
  if (!keyExchange()) return false;
  r_.kexMs = elapsed();
  stage(SSH_STAGE_AUTH);
  if (!authenticate()) return false;
  r_.authMs = elapsed();
  keepalives_ = true;
  stage(SSH_STAGE_EXEC);
  bool ok = runCommand();
  // Polite goodbye; a failure to send it does not matter.
  std::string err = r_.error;
  Out o = begin();
  o.u8(MSG_DISCONNECT);
  o.u32(11);   // SSH_DISCONNECT_BY_APPLICATION
  o.str("");
  o.str("");
  send(o);
  r_.error = err;
  stage(SSH_STAGE_CLOSED);
  return ok;
}

}  // namespace

bool sshRun(const SshIo &io, const SshParams &p, const SshCallbacks &cb, SshResult &r) {
  r = SshResult();
  Session s(io, p, cb, r);
  bool ok = s.run();
  r.totalMs = s.elapsed();
  r.ok = ok;
  if (!ok && r.error.empty()) r.error = "failed";
  return ok;
}

void sshPublicKey(const uint8_t seed[32], uint8_t pk[32]) {
  uint8_t s[32], sk[64];
  memcpy(s, seed, 32);
  crypto_ed25519_key_pair(sk, pk, s);
  crypto_wipe(sk, sizeof sk);
}

std::string sshPublicKeyLine(const uint8_t pk[32], const std::string &comment) {
  uint8_t blob[51];
  keyBlob(pk, blob);
  std::string s = std::string(ED25519) + " " + base64(blob, sizeof blob, true);
  if (!comment.empty()) s += " " + comment;
  return s;
}

std::string sshFingerprint(const uint8_t pk[32]) {
  uint8_t blob[51], d[32];
  keyBlob(pk, blob);
  Hash h;
  h.raw(blob, sizeof blob);
  if (!h.final(d)) return std::string();
  return "SHA256:" + base64(d, sizeof d, false);
}
