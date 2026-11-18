// SPDX-License-Identifier: GPL-3.0-or-later
// Host build of the firmware's SSH client (src/ssh_client.cpp) for tests
// against a real OpenSSH server (test_ssh.sh).
//   ssh_host keyfile --pubkey                       prints the authorized_keys line
//   ssh_host keyfile host port user command [hostkey-hex]
// keyfile holds the 32-byte seed (created when missing). Output goes to
// stdout/stderr as the server sends it; the exit code is the command's, 255
// when the session failed (the reason on stderr as "ssh_host: ...").
#include "ssh_client.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---- hooks: random and SHA-256 (FIPS 180-4) ---------------------------------

void sshRandom(uint8_t *buf, size_t n) {
  while (n) {
    ssize_t k = getrandom(buf, n, 0);
    if (k <= 0) abort();
    buf += k;
    n -= (size_t)k;
  }
}

struct SshSha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
};

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

static void block(SshSha256 *s, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

SshSha256 *sshSha256New() {
  SshSha256 *s = (SshSha256 *)calloc(1, sizeof(SshSha256));
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  return s;
}

void sshSha256Update(SshSha256 *s, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  s->len += n;
  while (n) {
    size_t k = 64 - s->n < n ? 64 - s->n : n;
    memcpy(s->buf + s->n, p, k);
    s->n += k;
    p += k;
    n -= k;
    if (s->n == 64) { block(s, s->buf); s->n = 0; }
  }
}

void sshSha256Final(SshSha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80, zero = 0, l[8];
  sshSha256Update(s, &pad, 1);
  while (s->n != 56) sshSha256Update(s, &zero, 1);
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sshSha256Update(s, l, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
  }
}

void sshSha256Free(SshSha256 *s) { free(s); }

// ---- socket I/O ----------------------------------------------------------------

static bool ioSend(void *c, const uint8_t *p, size_t n) {
  int fd = *(int *)c;
  while (n) {
    ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
    if (k <= 0) return false;
    p += k;
    n -= (size_t)k;
  }
  return true;
}

static int ioRecv(void *c, uint8_t *p, size_t n, uint32_t waitMs) {
  struct pollfd pf = {*(int *)c, POLLIN, 0};
  int r = poll(&pf, 1, (int)waitMs);
  if (r < 0) return -1;
  if (r == 0) return 0;
  ssize_t k = recv(*(int *)c, p, n, 0);
  return k > 0 ? (int)k : -1;
}

static uint32_t ioNow(void *) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static size_t outTotal = 0, cancelAfter = 0;   // SSH_HOST_CANCEL_AFTER=<bytes>: test of stop

static void cbOutput(void *, int stream, const uint8_t *p, size_t n) {
  fwrite(p, 1, n, stream == 2 ? stderr : stdout);
  outTotal += n;
}

static bool cbPoll(void *) { return cancelAfter && outTotal >= cancelAfter; }

static bool loadKey(const char *path, uint8_t seed[32]) {
  FILE *f = fopen(path, "rb");
  if (f) {
    bool ok = fread(seed, 1, 32, f) == 32;
    fclose(f);
    return ok;
  }
  sshRandom(seed, 32);
  f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(seed, 1, 32, f) == 32;
  fclose(f);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s keyfile --pubkey | keyfile host port user command [hostkey-hex]\n", argv[0]);
    return 2;
  }
  SshParams p;
  if (!loadKey(argv[1], p.seed)) { fprintf(stderr, "ssh_host: cannot read or create %s\n", argv[1]); return 2; }
  if (!strcmp(argv[2], "--pubkey")) {
    uint8_t pk[32];
    sshPublicKey(p.seed, pk);
    printf("%s\n", sshPublicKeyLine(pk, "ssh_host").c_str());
    return 0;
  }
  if (argc < 6) return 2;
  p.user = argv[4];
  p.command = argv[5];
  p.timeoutMs = 60000;
  p.silenceMs = 5000;
  if (argc > 6) {
    if (strlen(argv[6]) != 64) return 2;
    for (int i = 0; i < 32; i++) sscanf(argv[6] + 2 * i, "%2hhx", &p.hostKey[i]);
    p.pinned = true;
  }

  struct addrinfo hints = {}, *res;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(argv[2], argv[3], &hints, &res) != 0) { fprintf(stderr, "ssh_host: cannot resolve\n"); return 255; }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { fprintf(stderr, "ssh_host: cannot connect\n"); return 255; }
  freeaddrinfo(res);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  SshIo io = {&fd, ioSend, ioRecv, ioNow};
  SshCallbacks cb;
  cb.output = cbOutput;
  cb.poll = cbPoll;
  if (const char *c = getenv("SSH_HOST_CANCEL_AFTER")) cancelAfter = strtoul(c, nullptr, 10);
  SshResult r;
  bool ok = sshRun(io, p, cb, r);
  close(fd);
  fflush(stdout);
  if (r.hostKeySeen) {
    fprintf(stderr, "ssh_host: host key ");
    for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", r.hostKey[i]);
    fprintf(stderr, " %s\n", sshFingerprint(r.hostKey).c_str());
  }
  fprintf(stderr, "ssh_host: ok=%d exit=%d signal=%s out=%u err=%u kex=%ums auth=%ums total=%ums\n", ok,
          r.exitStatus, r.exitSignal.c_str(), r.outBytes, r.errBytes, r.kexMs, r.authMs, r.totalMs);
  if (!ok) {
    fprintf(stderr, "ssh_host: %s\n", r.error.c_str());
    return 255;
  }
  return r.exitStatus < 0 ? 255 : r.exitStatus;
}
