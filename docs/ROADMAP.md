# Roadmap

Current plans, roughly in order of how soon they are likely. Open questions
about the hardware are in `docs/FINAL_FIRMWARE_SPEC.md`, section 11.

## Planned

- **More languages.** The web UI is translatable (`docs/TRANSLATING.md`);
  English and Polish come first. Contributions of further catalogs are
  welcome.
- **More 2.4G protocols.** Enhanced ShockBurst at 2 Mbit/s, and the Steam
  Controller's own protocol. Both need captures of real devices
  (`docs/SNIFFING.md`); the plug-in structure is described in
  `docs/ADDING_PROTOCOLS.md`.
- **PCB rev A: fabrication and test.** The KiCad design is in `hardware/`;
  boards still have to be made, assembled and tested against the
  perfboard build.
- **Controller as sniffer.** Use the controller's own radio to capture raw
  2.4G and Bluetooth packets of an unknown device and export them, so users
  can contribute captures for new devices without extra hardware.
- **OS updates on Bazzite and Fedora.** The update script's rpm-ostree and
  dnf paths are tested in containers only; they need a run on real
  installs.
