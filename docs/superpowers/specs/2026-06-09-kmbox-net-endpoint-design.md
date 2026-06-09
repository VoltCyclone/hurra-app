# Design: KMBox Net (UDP) endpoint for hurra-bridge

**Date:** 2026-06-09
**Status:** Approved (pending spec review)
**Repos affected:** `hurra-app` (this repo, host-side), `Hurra-v2` (firmware, dependency)

## Summary

Add a second input endpoint to the single `hurra-bridge` binary: a **KMBox Net**
UDP listener, alongside the existing **VCOM** (Ferrum-over-virtual-serial)
endpoint. At startup the bridge presents an interactive TUI selector; the user
picks one endpoint for that run. Both endpoints funnel through one shared
translation layer into the same `libhurra` serial link to the device.

KMBox Net is a UDP request/reply protocol in which the KMBox *device* is the
server: client tools send fixed-layout command packets and the device echoes an
ACK. `hurra-bridge` therefore **emulates the KMBox Net device side** — it binds a
UDP socket, accepts `cmd_connect` + command packets, translates them into Hurra
serial frames, and echoes ACK headers. This mirrors how it already emulates a
Ferrum device on a virtual COM port.

## Core principle: the host translates, the device does the work

**The bridge is a thin translator, not an emulator.** All KMBox behavioural
features — movement smoothing (auto/bezier), physical-input monitoring, input
masking — live on the **device** in the Hurra-v2 firmware, never in the host.
The host's only jobs are (a) receive KMBox Net packets and forward them as Hurra
frames, and (b) toggle device features on/off.

This is a hard constraint. Where a KMBox Net command needs a capability the
Hurra protocol does not yet have, the answer is a **new Hurra-protocol frame +
firmware implementation**, not a host-side workaround. Keeping the logic on the
device guarantees identical behaviour regardless of which host endpoint is in
use and prevents drift between a host emulation and the real device.

## Goals / Non-goals

**Goals**
- One binary, two selectable endpoints (VCOM, KMBox Net), sharing one device link.
- Full KMBox Net *input* command coverage (mouse, keyboard, connect, reboot,
  auto/bezier move, monitor, mask) — via host translation to Hurra frames.
- A frozen host↔firmware wire contract for the new Hurra frames the advanced
  KMBox commands depend on, so the host can be built first and firmware fills in
  behind it.
- Isolated, independently testable layers (codec / transport / translation).

**Non-goals**
- KMBox LCD/image display, video capture (KVM), and on-device AI/YOLO commands —
  explicitly out of scope.
- Running both endpoints simultaneously (one per run, chosen at startup).
- Host-side emulation of any device behaviour (see Core principle).

## Architecture

Three layers. Today's `bridge.c` collapses all three; this design lifts the
middle layer out so both frontends share it.

```
┌─ frontend (owns transport + protocol) ─────────────┐
│  frontend_vcom    : PTY/com0com  + ferrum_parser   │   ← existing, refactored
│  frontend_kmbox   : UDP socket   + kmbox_codec     │   ← NEW
└──────────────────────┬─────────────────────────────┘
                       │ emits abstract input events  (input_sink_t)
┌──────────────────────▼─────────────────────────────┐
│  translation core  (input_core.c)                  │   ← lifted out of bridge.c
│  move / button / wheel / kb_report / mouse_all /   │
│  reboot / set_feature → hurra_* calls. Thin.       │
└──────────────────────┬─────────────────────────────┘
                       │ hurra_* (serial frames)
                ┌──────▼───────┐
                │  libhurra    │  → device does ALL the work
                └──────────────┘
```

The main loop stays single-threaded and transport-agnostic: each tick calls
`fe_poll()` (one non-blocking drain + decode + dispatch) then `hurra_poll()`,
exactly the existing `recv → dispatch → sleep_us` shape — `recvfrom` for UDP
instead of `read` for the PTY.

### Files

| File | Role |
|---|---|
| `src/input_core.{h,c}` | NEW. Translation layer: `input_sink_t` vtable backed by one `hurra_client_t`. Holds the `cb_*` logic lifted verbatim from today's `bridge.c`. No protocol knowledge. |
| `src/frontend.h` | NEW. Common frontend interface: `fe_open`, `fe_poll`, `fe_close`, `fe_describe` (banner text). |
| `src/frontend_vcom.c` | NEW. Wraps existing `virtual_port_*` + `ferrum_parser` behind `frontend.h`. |
| `src/frontend_kmbox.c` | NEW. Owns UDP socket + `kmbox_codec`; decodes packets → `input_sink_t` calls; echoes ACK; relays monitor telemetry. |
| `src/kmbox_codec.{h,c}` | NEW. Pure packet (de)serialization + ACK builder. No I/O, no hurra. Unit-testable. |
| `src/udp_socket.h` + `src/udp_socket_unix.c` / `src/udp_socket_win32.c` | NEW. Thin non-blocking UDP abstraction, split per-OS like `virtual_port`. |
| `src/selector.{h,c}` | NEW. Startup TUI endpoint menu. |
| `src/bridge.c` | CHANGED. Slims to: parse args → `ui_init` → selector → build chosen frontend + `input_core` → existing main loop (now `fe_poll` + `hurra_poll`). Telemetry/diag/status-line code stays. |
| `include/hurra.h`, `src/hurra.c` | CHANGED. Expose `hurra_mo()` (wraps existing firmware `MOUSE_MO 0x13`). Add wrappers for the new frames (§ Firmware contract). |
| `include/hurra_types.h` | CHANGED. Reserve new TYPE codes (§ Firmware contract), kept byte-identical with firmware. |
| `tests/kmbox_codec_test.c`, `tests/input_core_test.c` | NEW. Unit tests (CMake + CI). |

### Isolation properties
- `kmbox_codec` is pure bytes-in/bytes-out: testable with captured packets, no
  sockets or device.
- `input_core` depends only on `libhurra`; both frontends drive it through the
  same `input_sink_t`, so a mock sink proves both protocols converge on the same
  translation.
- Each frontend owns its transport entirely; `bridge.c` never touches a socket
  or PTY fd directly.

## KMBox Net wire protocol

Reference: <https://github.com/ZCban/kmboxNET> (`kmboxNet.h` / `kmboxNet.cpp`).

16-byte header, four little-endian `uint32`:

```c
typedef struct { uint32_t mac, rand, indexpts, cmd; } km_head_t;   /* 16 B LE */
```

Payloads (KMBox uses `int32` fields):
```c
/* soft_mouse_t  */ int32_t button, x, y, wheel, point[10];
/* soft_keyboard */ int8_t ctrl; int8_t resvel; uint8_t button[10];
```

**ACK:** every recognized packet is acknowledged by echoing the 16-byte header
back with the same `cmd` + `indexpts` (the client matches on those two fields).
`connect` with a wrong MAC is dropped (not ACKed). Malformed/short packets are
dropped and counted.

### Command codes (verbatim from reference SDK)

```
cmd_connect   0xaf3c2828   cmd_mouse_move 0xaede7345   cmd_mouse_left  0x9823AE8D
cmd_mouse_mid 0x97a3AE8D   cmd_mouse_right 0x238d8212   cmd_mouse_wheel 0xffeead38
cmd_automove  0xaede7346   cmd_bazerMove  0xa238455a   cmd_keyboard_all 0x123c2c2f
cmd_reboot    0xaa8855aa   cmd_monitor    0x27388020   cmd_mask_mouse  0x23234343
cmd_unmask_all 0x23344343
```

### Commands that map to existing firmware (host translation only)

| KMBox cmd | → `input_sink_t` | → Hurra frame |
|---|---|---|
| `connect` | verify MAC → ACK; cache client addr | optional `hurra_version` probe |
| `mouse_move(x,y)` | `move(x,y)` | `MOUSE_MOVE 0x10` |
| `mouse_left/right/middle(isdown)` | `button(mask,down)` | `BTN_* 0x20–0x22` |
| `mouse_wheel(n)` | `wheel(n)` | `MOUSE_WHEEL 0x15` |
| `mouse_all(button,x,y,wheel)` | `mouse_all(...)` | `MOUSE_MO 0x13` via existing `hurra_mo()` (already public in `hurra.h`) |
| `keyboard_all(mod, keys[10])` | `kb_report(mod, keys)` | `KB_MULTIDOWN/UP 0x46/0x47` diffed vs previous report; modifier via `KB_DOWN 0xE0–0xE7`. **fw caps at 6 keys → truncate extras, log once.** |
| `reboot` | `reboot()` | `REBOOT 0x04` |

Button bit mapping matches existing bridge convention
(L=0x01,R=0x02,M=0x04,S1=0x08,S2=0x10 → hurra index 0–4).

## Firmware contract (NEW Hurra frames — co-designed)

These four KMBox commands need capability the Hurra protocol lacks today. Per
the Core principle they are implemented **on the device**. The host sends the
new frames; the firmware does the work. The contract below is **frozen now** so
the host can be built and merged first.

Proposed new TYPE codes (fill existing gaps in `hurra_types.h`; reserve the same
values in the firmware enum in the same change):

| KMBox cmd | New Hurra frame | Payload (LE) | Device behaviour to implement in Hurra-v2 |
|---|---|---|---|
| `automove(x,y,time_ms)` | `MOUSE_MOVE_DUR 0x1B` | `int16 dx, int16 dy, uint16 dur_ms` | Step the move across `dur_ms` using the firmware's existing humanize filter. (`MOUSE_MOVE_SMOOTH 0x11` exists but is single-frame; this adds duration.) |
| `bazerMove(...)` | `MOUSE_MOVE_BEZIER 0x1C` | `int16 dx,dy, uint16 dur_ms, int16 x1,y1,x2,y2` | Walk a cubic Bézier over `dur_ms`. |
| `monitor(port)` + per-button/key queries | `CB_PHYS 0x77` (enable toggle) + telemetry `TLM_PHYS_BUTTONS 0x86`, `TLM_PHYS_AXIS 0x87`, `TLM_PHYS_KB 0x88` | Tap physical input **before** the inject merge in `kmbox_merge_report()`; emit physical-only frames. Host relays them into KMBox monitor replies on the cached client UDP address. |
| `mask_* / unmask_all` | `PHYS_MASK 0x68` | `uint8 domain, uint8 code, uint8 enable` | **Apply** `g_lock_mask` / `g_masked_keys` in the merge path (today set but never enforced) to actually suppress physical pass-through. |

**Contract rules**
- All multi-byte fields little-endian.
- New TYPE codes reserved in **both** `hurra_types.h` and the firmware enum in
  the same change; documented field-by-field so both repos agree.
- Host repo lands first against this frozen contract.

**Behaviour until firmware ships the new frames:** the four advanced commands
ACK the client (so tools don't error) and log `pending firmware: <cmd>` once
each. No host-side emulation is added as a stopgap.

### Hurra-v2 firmware dependency (tracked, separate plan)
1. `MOUSE_MOVE_DUR 0x1B` — duration-stepped move via humanize filter.
2. `MOUSE_MOVE_BEZIER 0x1C` — cubic Bézier over duration.
3. `CB_PHYS 0x77` + `TLM_PHYS_* 0x86/0x87/0x88` — physical-only telemetry tap
   before inject merge.
4. `PHYS_MASK 0x68` — enforce existing `g_lock_mask`/`g_masked_keys` in
   `kmbox_merge_report()`.

(Findings from firmware audit: `humanize.h` only jitters the injected delta
within one frame; telemetry reports merged physical+injected state with no
physical-only tap; lock/mask state is set but never applied in the merge path.)

## Startup selector (TUI)

After arg parse + `ui_init`, before opening any transport, `selector.c` renders
a numbered menu to stderr and reads a keystroke from stdin:

```
hurra-bridge

  Select an endpoint:
    1) Virtual COM port (Ferrum-compatible)   [default]
    2) KMBox Net (UDP)

  > _
```

`1`/Enter → VCOM; `2` → KMBox Net. The chosen frontend's banner then prints and
the loop starts.

**TTY required.** If stdin is not a TTY the bridge exits via `bridge_fail` with a
clear message that an interactive terminal is now needed to choose an endpoint.

> **Breaking change:** today the bridge auto-starts the VCOM endpoint and works
> when piped/redirected/in CI. With the selector, non-interactive runs now error.
> Documented in the README and called out in release notes.

## Configuration (KMBox Net mode)

Reuses existing `--device` / `--baud` / `--timeout-ms` / `--no-color`. Adds:

| Flag | Default | Meaning |
|---|---|---|
| `--km-port N` | a fixed default chosen by this implementation, documented in README | UDP port the bridge binds/listens on |
| `--km-bind ADDR` | `0.0.0.0` | listen address |
| `--km-mac HEX` | a fixed default chosen by this implementation, documented in README | 4-byte MAC the client must present in `connect` |

> Note: a real KMBox Net device assigns its IP/port/MAC and shows them on its
> OLED; there is no protocol-fixed "stock" port. Because the bridge emulates the
> device, it picks its own defaults (overridable via the flags above) and the
> README tells users what to point their client tools at.

VCOM-only flags (`--link`, `--virtual-port`) are ignored in KMBox mode and
vice-versa; a mismatch logs a dim note, not an error.

## Error handling

- **UDP bind failure** (port in use / permission) → `bridge_fail` with the same
  friendly headline+body pattern used for serial-open failures.
- **`connect` wrong MAC** → drop + log (no ACK).
- **Malformed / short packet** → drop + increment a `kmbox_rx_bad` diag counter
  (parallels existing diag fields).
- `__diag__` side-channel and the live status line work unchanged; the status
  line gains a mode tag (e.g. `running … · kmbox · link ✓`).

## Telemetry / monitor relay

The existing `hurra_on_telemetry` callbacks remain wired for VCOM. In KMBox
mode, once `CB_PHYS`/`TLM_PHYS_*` exist in firmware, `frontend_kmbox` subscribes
to the physical-only telemetry and emits KMBox monitor replies to the cached
client address. No synthesis of physical state on the host.

## Testing

Mirrors the existing `tests/ui_util_test.c` C-test style (added to CMake + CI):

- **`kmbox_codec` unit tests:** decode captured packet bytes → assert fields;
  build ACK → assert 16 bytes match header; short-packet / truncation / bad-MAC
  cases. Pure, no I/O.
- **`input_core` tests:** mock `input_sink_t` capturing calls; feed synthetic
  events from a fake ferrum line and a fake kmbox packet; assert identical
  `hurra_*` intent — proves both frontends converge.
- **Loopback integration (Unix):** bind UDP frontend on an ephemeral port,
  `sendto` `connect` + `mouse_move`, assert ACK returns and the mock device saw a
  move.

## Build / CI

- CMake: add new sources to `hurra-bridge`; pick `udp_socket_unix.c` vs
  `_win32.c` like the existing per-OS split; link Winsock (`ws2_32`) on Windows.
- Add the two new test targets to CTest and the CI matrix.

## Out of scope (explicitly)

KMBox LCD color/picture, KVM video capture, YOLO/AI inference. Simultaneous
dual-endpoint operation.
