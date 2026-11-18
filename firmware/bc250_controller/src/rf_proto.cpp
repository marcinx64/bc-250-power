// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol registry. To add a protocol: write proto_<name>.cpp that
// defines `const rfp::Proto proto_<name>` and add one X(<name>) to
// RF_PROTOCOLS below (docs/ADDING_PROTOCOLS.md). Order = detection order, and
// protocols with the same bit rate must stay together (one demodulation per
// rate and burst).
#include "rf_proto.h"
#include <string.h>

#ifdef RF_PROTO_ESB2M
#define RF_PROTO_EXPERIMENTAL(X) X(esb2m)
#else
#define RF_PROTO_EXPERIMENTAL(X)
#endif

#define RF_PROTOCOLS(X) \
  X(esb1m)              \
  X(btpage)             \
  RF_PROTO_EXPERIMENTAL(X)

#define X(n) extern const rfp::Proto proto_##n;
RF_PROTOCOLS(X)
#undef X

namespace rfp {

static const Proto *const REG[] = {
#define X(n) &proto_##n,
    RF_PROTOCOLS(X)
#undef X
};
static uint32_t ready;

int count() { return (int)(sizeof(REG) / sizeof(REG[0])); }
const Proto *at(int i) { return REG[i]; }
int find(const char *id) {
  for (int i = 0; i < count(); i++) if (strcmp(REG[i]->id, id) == 0) return i;
  return -1;
}
int defaultFor(bool host) {
  for (int i = 0; i < count(); i++) if (REG[i]->host == host) return i;
  return -1;
}
void initAll() {
  ready = 0;
  for (int i = 0; i < count(); i++) if (!REG[i]->init || REG[i]->init()) ready |= 1u << i;
}
uint32_t readyMask() { return ready; }

}  // namespace rfp
