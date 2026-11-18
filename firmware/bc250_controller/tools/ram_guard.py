# SPDX-License-Identifier: GPL-3.0-or-later
"""Post-link RAM guard (PlatformIO extra script).

Static RAM and RAM code must end below the 2.4G capture bank
(0x3fca0000 data bus / 0x403a0000 instruction bus, src/rf.cpp). The check
runs after the ELF is linked; a linker script would upset the order of
IDF's own linker scripts.
"""
import subprocess
import sys

Import("env")  # noqa: F821  (PlatformIO extra script)

BANK_DATA = 0x3FCA0000
BANK_CODE = 0x403A0000


def _guard(target, source, env):
    nm = env.subst("$NM") if env.get("NM") else "riscv32-esp-elf-nm"
    out = subprocess.run([nm, str(target[0])], capture_output=True, text=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}
    if sym.get("_bss_end", 0) > BANK_DATA or sym.get("_iram_end", 0) > BANK_CODE:
        sys.exit("static RAM overlaps the 2.4G capture bank: move a buffer to the heap")
    print("ram_guard: static RAM ends at 0x%08x, %d bytes below the capture bank"
          % (sym["_bss_end"], BANK_DATA - sym["_bss_end"]))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _guard)  # noqa: F821
