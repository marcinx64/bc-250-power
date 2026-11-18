# Monocypher

[Monocypher](https://monocypher.org) 4.0.3, the files `src/monocypher.[ch]`
and `src/optional/monocypher-ed25519.[ch]` of the release, unmodified. The
SSH client (`src/ssh_client.cpp`) takes Ed25519, X25519, ChaCha20 and
Poly1305 from it; ESP-IDF's mbedTLS has no Ed25519.

Licence: BSD-2-Clause or CC0-1.0, at your choice (`LICENCE.md`).
