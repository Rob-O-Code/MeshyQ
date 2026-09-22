# MeshyQ

A self-provisioning Bluetooth Mesh node for the Adafruit Feather nRF52840
Express + Music Maker FeatherWing (VS1053 codec + microSD). Type a command
into the serial shell on one node and it's published over the mesh; nodes
that receive it act on the command, e.g. playing a file from their own SD
card root directory.

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

1. Double-tap the physical reset button, or, if the board is already
   running firmware with the serial shell up, type `bootloader` at the
   console instead. Either way the board mounts as a USB drive named
   `FTHR840BOOT`.
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
| `cue play <filename>` | Broadcast a play command to the mesh — shorthand for `chat msg play <filename>` |
| `cue find <filename>` | Broadcast a find command to the mesh — shorthand for `chat msg find <filename>` |
| `cue list` | Ask every mesh node to announce its playable files — shorthand for `chat msg list` |
| `chat status` | Show provisioning state, this node's address, and presence |
| `chat msg <cmd> <args>` | Publish a raw `cmd args` command to the mesh chat group |
| `chat private <addr> <cmd> <args>` | Send a raw `cmd args` command as a private message to a specific node address |
| `chat presence set <available\|away\|dnd\|inactive>` | Set this node's presence |
| `chat presence get <addr>` | Query another node's presence |
| `bootloader` | Reboot into the Adafruit UF2 mass-storage bootloader (`FTHR840BOOT`), same as double-tapping reset — see [Flashing](#flashing) |

`cue` and `chat msg`/`chat private` send the exact same `cmd args` mesh
messages. `cue` is the recommended everyday interface — run `cue` alone (or
`cue play -h` / `cue find -h` / `cue list -h`) for built-in help on each
subcommand. Reach for `chat msg`/`chat private` directly when you want to
send free-form chat text, or target one specific node instead of the whole
group.

| Command | Effect |
|---|---|
| `play <filename>` | Play the file from the receiving node's SD card root, if it has one by that name |
| `find <filename>` | Ask which nodes have a file, without playing it. Only node(s) that have the file reply with `<addr>: found <filename>`, so it doubles as a mesh reachability check across the whole fleet |
| `list` | Ask every node to announce its playable (`.wav`/`.mp3`) files as one comma-separated line, e.g. `<addr>: a.wav,ding.mp3`, or `<addr>: (none)` if it has none. Unlike `find`, *every* node replies, so it's both a file inventory and a full roll call of the fleet |

For example, `cue play example.wav` (equivalent to `chat msg play
example.wav`) plays `example.wav` on every node that has it; `cue find
example.wav` reports back which nodes do, without playing anything; `cue
list` has every reachable node announce what it can play. Unrecognized text
sent via `chat msg`/`chat private` (or a message with no recognized verb)
is treated as plain chat and produces no action. A node never reacts to its
own messages, so testing this requires at least two provisioned nodes.

`list` filters by file extension only (a simple heuristic, matching what
the README says the VS1053 decodes) — it doesn't open or validate file
contents. If a node has more playable files than fit in one message
(`CONFIG_BT_MESH_CHAT_CLI_MESSAGE_LENGTH`, `Kconfig`), the reply reports as
many as fit and silently omits the rest rather than sending a truncated
filename.

## Testing the mesh commands

You need at least two flashed, provisioned nodes with a serial monitor open
on each (e.g. two terminal windows running `screen`/`minicom`, or two nRF
Connect for VS Code Serial Terminal panes) — see the note above about nodes
not reacting to their own messages.

**`find` (device-finding / reachability check):**

1. Put a test file (any small file works — `find` only checks that it
   exists, it doesn't play or validate it) on one node's SD card root, and
   make sure it's *not* present on another node's card.
2. On each node, run `chat status` to confirm it's provisioned and note its
   address (`0x____`).
3. From the node *without* the file, run `cue find <filename>` (equivalent
   to `chat msg find <filename>`).
4. Expected result: the console of the node that ran `find` prints
   `<0x____>: found <filename>` for the address of the node that *has* the
   file, and nothing from nodes that don't. No reply at all means the
   message either didn't get out, got lost in transit, or nothing has that
   file — see the debugging tips below to tell those apart.
5. To test a single node directly instead of the whole group, use
   `chat private <addr> find <filename>` and confirm you get exactly one
   `found <filename>` reply when that node has the file, and none when it
   doesn't. (A private `find` also gets a second, generic `<addr>:
   received` line regardless of content — that one just confirms the
   private message itself was delivered, independent of whether the file
   was found.)

**`play`:** same setup, `cue play <filename>` — confirm playback starts on
every node that has the file, and that nodes without it do nothing (no
reply is expected for `play`, unlike `find`).

**`list` (file inventory / full roll call):** with the same two nodes, put a
couple of `.wav`/`.mp3` files (and, to confirm filtering, a non-audio file
like `notes.txt`) on one node's SD card root, then run `cue list` from the
other. Expect a `<addr>: a.wav,b.mp3`-style reply that excludes
`notes.txt`, and a `<addr>: (none)` reply from any node with no playable
files — unlike `find`, every reachable node should reply here, so this
doubles as the simplest full-fleet reachability check once more nodes are
assembled: run `cue list` and confirm you hear back from every node you
expect to be online.

**Scaling up to the full fleet:** once more nodes are assembled, put a file
on every node's SD card and broadcast `cue find <that file>` from one node.
Count the replies over a few seconds and compare against the number
of nodes you expect to hear from — this is the actual test the mesh
reliability tuning in [Notes](#notes) is aimed at validating, not just
trusting the config values. Repeat a few times, and from different
originating nodes / physical positions, to get a feel for real-world
reliability rather than a single best-case run.

**If replies don't show up as expected:**

- `CONFIG_BT_MESH_LOG_LEVEL_DBG=y` is already enabled (`prj.conf`), so each
  node's own console logs its mesh stack activity (publish attempts, relay
  forwarding) — check the sender's and any suspected relay's logs first.
- For a packet-level view of what's actually going over the air (e.g. to
  tell "never transmitted" apart from "transmitted but not received"), use
  the [nRF Sniffer for Bluetooth LE](https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE)
  with an extra nRF52-based dongle/DK and Wireshark.
- The [Bluetooth Mesh: Chat sample docs](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/samples/bluetooth/mesh/chat/README.html)
  describe the underlying Chat Client model this project builds on (this
  project self-provisions instead of using the nRF Mesh phone app, but the
  model/shell behavior it describes still applies).
- Nordic's [Large scale Bluetooth mesh testing](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/large-scale-bluetooth-mesh-testing)
  blog post covers methodology for characterizing mesh reliability across a
  fleet, if you want to go beyond ad hoc `find` sweeps.

## Notes

- Each node derives its own mesh unicast address automatically from its
  hardware ID at boot (printed as `Derived mesh node address: 0x____`) —
  no manual per-board configuration needed.
- `net_key`/`dev_key`/`app_key` are hardcoded and shared across all nodes
  for self-provisioning convenience. Not suitable for production use.
- Tuned for a sparse-traffic fleet of ~13 nodes: relays retransmit each
  forwarded packet (`CONFIG_BT_MESH_RELAY_RETRANSMIT_COUNT=2`) instead of
  sending it once, the originating node repeats its own send
  (`CONFIG_BT_MESH_NETWORK_TRANSMIT_COUNT=3`), and the replay-protection
  list (`CONFIG_BT_MESH_CRPL=16`) and presence cache
  (`BT_MESH_CHAT_SAMPLE_PRESENCE_CACHE_SIZE=16`) are both sized above the
  node count. See `prj.conf` for details. There's no automatic retry if a
  command doesn't reach every node — re-run it, or use `find` to check who
  got it.
