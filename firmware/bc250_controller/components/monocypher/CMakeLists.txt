# SPDX-License-Identifier: GPL-3.0-or-later
# Monocypher 4.0.3 (https://monocypher.org), unmodified: Ed25519, X25519,
# ChaCha20 and Poly1305 for the SSH client (src/ssh_client.cpp). mbedTLS of
# ESP-IDF has no Ed25519. Licence: BSD-2-Clause OR CC0-1.0 (LICENCE.md).
idf_component_register(SRCS monocypher.c monocypher-ed25519.c
                       INCLUDE_DIRS .)
