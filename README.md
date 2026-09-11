# MeshyQ

A self-provisioning Bluetooth Mesh node for the Adafruit Feather nRF52840
Express + Music Maker FeatherWing (VS1053 codec + microSD). Type a message
into the serial shell on one node and it's published over the mesh; any
node whose SD card root directory has a file matching that message plays
it.

## Hardware

- Adafruit Feather nRF52840 Express
- Adafruit Music Maker FeatherWing w/ Amp, stacked directly on the header
- A microSD card with your audio files (WAV or MP3 — the VS1053 decodes
  both identically) in its **root directory**

## Building

Board target: `adafruit_feather_nrf52840/nrf52840/uf2` — the `/uf2`
qualifier is required; it links the app to load after the Adafruit
bootloader and routes the console over USB CDC-ACM. The plain
`adafruit_feather_nrf52840/nrf52840` target assumes no bootloader and
requires an external SWD probe to flash.

Via nRF Connect for VS Code: create/select a build configuration with that
board and build normally.

Via the command line, with the NCS v3.4.0 toolchain:

```sh
export PATH="/opt/nordic/ncs/toolchains/ccc010f809/bin:/opt/nordic/ncs/toolchains/ccc010f809/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_BASE=/opt/nordic/ncs/v3.4.0/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/ccc010f809/opt/zephyr-sdk

west build -b adafruit_feather_nrf52840/nrf52840/uf2 -d build
```

## Flashing

1. Double-tap the physical reset button. The board mounts as a USB drive
   named `FTHR840BOOT`.
2. Copy the built firmware onto it:
   ```sh
   cp build/MeshyQ/zephyr/zephyr.uf2 /Volumes/FTHR840BOOT/
   ```
   A `fcopyfile`/`fchmod` error at the end is expected and harmless — the
   board reboots into the new firmware the moment it finishes receiving
   the file, which pulls the drive out from under the copy.
3. Open a serial monitor on the new USB CDC-ACM port that enumerates
   after reboot (it's a different device than before the reboot).

## Serial shell commands

| Command | Description |
|---|---|
| `ls` | List the SD card's root directory |
| `play <filename>` | Play a file from the SD card's root directory |
| `volume [0-100]` | Get or set output volume (persists across plays and reboots-in-session; 100 = loudest) |
| `chat status` | Show provisioning state, this node's address, and presence |
| `chat msg <message>` | Publish a text message to the mesh chat group |
| `chat private <addr> <message>` | Send a private message to a specific node address |
| `chat presence set <available\|away\|dnd\|inactive>` | Set this node's presence |
| `chat presence get <addr>` | Query another node's presence |

Sending a message whose text exactly matches a filename in a receiving
node's SD card root (e.g. `chat msg example.wav`) triggers that node to
play the file. A node never reacts to its own messages, so testing this
requires at least two provisioned nodes.

## Notes

- Each node derives its own mesh unicast address automatically from its
  hardware ID at boot (printed as `Derived mesh node address: 0x____`) —
  no manual per-board configuration needed.
- `net_key`/`dev_key`/`app_key` are hardcoded and shared across all nodes
  for self-provisioning convenience. Not suitable for production use.
