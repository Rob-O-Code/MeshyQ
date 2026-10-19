# MeshyQ

A self-provisioning Bluetooth Mesh wireless audio cue system for Adafruit
Feather nRF52840 boards, paired with a browser-based control app.

This is a monorepo with two parts:

- [`firmware/`](firmware/README.md) — the Zephyr/NCS application: a single,
  unified firmware image that runs on every node, whether or not it has a
  Music Maker FeatherWing + SD card attached. See its README for hardware,
  build, flash, and serial shell command docs.
- [`web/`](web/README.md) — a browser-based GUI that talks to a MeshyQ node
  over USB using the Web Serial API. See its README for setup and build
  instructions.
