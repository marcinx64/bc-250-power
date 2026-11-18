// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Minimal SSH-2 client: logs in with a key, runs one command, returns its
// output and exit status. One fixed algorithm set, the OpenSSH defaults:
//   key exchange  curve25519-sha256, strict kex (kex-strict-c-v00@openssh.com)
//   host key      ssh-ed25519 (pinned by the caller)
//   user key      ssh-ed25519 from a 32-byte seed
//   cipher        chacha20-poly1305@openssh.com, no compression
// No PTY, no re-key, no forwarding. Plain C++ without ESP-IDF: the firmware
// (ssh.cpp) and the host test (host_test/ssh_host.cpp) supply the socket I/O
// and the hooks at the end of this file. Ed25519, X25519, ChaCha20 and
// Poly1305 come from Monocypher (components/monocypher).
#include <stdint.h>
#include <stddef.h>
#include <string>

struct SshIo {
  void *ctx;
  bool (*send)(void *ctx, const uint8_t *p, size_t n);              // all n bytes, false on error
  int (*recv)(void *ctx, uint8_t *p, size_t n, uint32_t waitMs);    // >0 bytes, 0 nothing yet, <0 closed
  uint32_t (*now)(void *ctx);                                       // ms, may wrap
};

enum SshStage { SSH_STAGE_VERSION, SSH_STAGE_KEX, SSH_STAGE_AUTH, SSH_STAGE_EXEC, SSH_STAGE_CLOSED };

struct SshCallbacks {
  void *ctx = nullptr;
  void (*output)(void *ctx, int stream, const uint8_t *p, size_t n) = nullptr;   // 1 stdout, 2 stderr
  void (*stage)(void *ctx, SshStage s) = nullptr;     // reached a stage
  bool (*poll)(void *ctx) = nullptr;                  // about once a second; true = cancel
};

struct SshParams {
  std::string user, command;
  uint8_t seed[32];          // user key
  bool pinned = false;       // hostKey is the expected server key
  uint8_t hostKey[32];
  uint32_t timeoutMs = 3600000;   // whole session
  uint32_t silenceMs = 60000;     // keepalive after this long without a packet; give up after 3x
};

struct SshResult {
  bool ok = false;            // the command ran and its channel closed
  int exitStatus = -1;        // -1: none reported (killed by a signal, or failed earlier)
  std::string exitSignal;
  std::string error;          // why it failed, empty when ok
  bool hostKeySeen = false;   // hostKey holds the server's verified key
  uint8_t hostKey[32];
  uint32_t outBytes = 0, errBytes = 0;
  uint32_t kexMs = 0, authMs = 0, totalMs = 0;   // from the start of sshRun
};

// Runs the whole session on a connected stream. Wipes its secrets on return.
bool sshRun(const SshIo &io, const SshParams &p, const SshCallbacks &cb, SshResult &r);

// Public key of a seed, its authorized_keys line and its ssh-keygen -l
// fingerprint ("SHA256:...").
void sshPublicKey(const uint8_t seed[32], uint8_t pk[32]);
std::string sshPublicKeyLine(const uint8_t pk[32], const std::string &comment);
std::string sshFingerprint(const uint8_t pk[32]);

// ---- hooks the platform provides -------------------------------------------
void sshRandom(uint8_t *buf, size_t n);   // cryptographic random bytes
struct SshSha256;                         // multi-part SHA-256
SshSha256 *sshSha256New();
void sshSha256Update(SshSha256 *h, const void *p, size_t n);
void sshSha256Final(SshSha256 *h, uint8_t out[32]);
void sshSha256Free(SshSha256 *h);
