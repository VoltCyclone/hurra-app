# hurra-app

Host-side adapter for the [Hurra binary protocol](https://github.com/ramseymcgrath/imxrtnsy).

The primary product is `hurra-bridge` — a small daemon that exposes a
**Ferrum-compatible virtual COM port** while talking to the firmware over
the real serial link. Existing Ferrum-speaking tools (e.g. `ferrum_aim_test.py`)
work unchanged against Hurra firmware.

The underlying `libhurra` library (used internally by the bridge) is also
available for C/C++ projects that want to drive the Hurra protocol directly.

* **Targets**: macOS, Linux, Windows (build matrix in CI).
* **Real-link baud**: 4 Mbps default; arbitrary rates supported via
  platform-specific custom-baud APIs (IOSSIOSPEED on macOS, `termios2`/BOTHER
  on Linux, `DCB.BaudRate` on Windows).

## Build

```bash
cmake -S . -B build
cmake --build build
```

Produces:

* `build/hurra-bridge` — bridge daemon (primary product)
* `build/libhurra.a`   — static library (or `hurra.lib` on Windows)
* `build/hello`        — minimal smoke-test for the library

On Unix you can also just `make`, which is a thin wrapper around the two
CMake commands.

## Quickstart — macOS / Linux

The bridge owns the real serial port and exposes a pseudo-terminal that
Ferrum-speaking tools connect to. A symlink at `$HOME/.hurra-bridge.tty`
points at whatever PTY slave the kernel allocated, so clients can hardcode
a stable path.

```bash
# Terminal 1 — bridge
./build/hurra-bridge --device /dev/cu.usbmodem01 --baud 4000000
# PTY: /dev/ttys004
# Symlink: /Users/you/.hurra-bridge.tty
# hurra: opened /dev/cu.usbmodem01 @ 4000000 baud
# bridge: running. SIGINT to stop.

# Terminal 2 — point any ferrum-speaking tool at the symlink
~/code/imxrtnsy/tools/ferrum_aim_test.py --port ~/.hurra-bridge.tty
```

Flags:

| Flag | Default | Description |
|---|---|---|
| `--device PATH` | _required_ | Real serial device (e.g. `/dev/cu.usbmodem01`, `COM5`). |
| `--baud N` | `4000000` | Real-link baud rate. |
| `--link PATH` | `$HOME/.hurra-bridge.tty` | Symlink to the PTY slave (Unix only). |
| `--virtual-port NAME` | _required on Win32_ | com0com COM name the bridge will open. |
| `--timeout-ms N` | `250` | Per-request timeout for get-style commands. |

## Quickstart — Windows + com0com

Windows has no PTY equivalent, so the bridge requires a pre-configured
[com0com](https://com0com.sourceforge.net/) virtual COM port pair.

1. Install com0com and run `setupc.exe` to create a pair, e.g. `CNCA0 ↔ CNCB0`.
2. Run the bridge against one end:

   ```cmd
   hurra-bridge.exe --device COM5 --baud 4000000 --virtual-port CNCA0
   ```

3. Point your Ferrum-speaking tool at the other end (`CNCB0`).

The bridge auto-prefixes `\\.\` on COM names that need it (any name with a
numeric suffix ≥10 or a non-`COMx` form like `CNCA0`).

## Ferrum surface (handled by the bridge)

The bridge implements every command in the firmware's Ferrum parser
(`imxrtnsy/src/ferrum.c`):

* Mouse: `km.move`, `m(x,y)`, `km.left/right/middle/side1/side2` (get + set),
  `km.click`, `km.wheel`, `km.catch_xy`.
* Locks: `km.lock_ml`, `km.lock_mr`, `km.lock_mm`, `km.lock_ms1`,
  `km.lock_ms2`, `km.lock_mx`, `km.lock_my` (get + set).
* Keyboard: `km.down`, `km.up`, `km.press`, `km.multidown`, `km.multiup`,
  `km.multipress`, `km.isdown`, `km.mask`.
* Init / version / baud: `km.init`, `km.version`, `km.baud`.
* Telemetry callbacks: `km.buttons`, `km.axes`, `km.keys`.

Reply formats match `ferrum.c` byte-for-byte (`0\r\n`, `1\r\n`,
`(x, y)\r\n`, `km.<bitmap_byte>\r\n`, `Axes(dx, dy, scroll)\r\n`,
`Keys(k1, k2, ...)\r\n`, `kmbox: Ferrum\r\n`).

### v1 limitations

* `km.<button>()` (button-state read) currently returns `0` — the firmware
  side supports it but `libhurra` does not yet expose `hurra_button_get`.
  Calls to `km.<button>(0)` / `km.<button>(1)` (set) work fine.
* `km.baud(n)` instructs the firmware to switch baud but does **not** change
  the bridge's host-side serial link. After issuing it you must restart the
  bridge with `--baud N`.

## Library (advanced)

If you want to drive the Hurra protocol directly from C without the bridge,
link against `libhurra.a` and `#include <hurra.h>`. The full API surface
covers mouse / keyboard / locks / telemetry / change-only callbacks; see
`examples/hello.c` for a minimal sketch and `include/hurra.h` for the
complete header.

## Platform notes

* **macOS** — the WCH CH34xVCP driver must be installed for ≥1.5 Mbps
  operation with CH343-based USB-UART bridges. Install via WCH's signed DMG.
* **Linux** — non-standard baud (4 Mbps) uses `struct termios2` +
  `ioctl(TCSETS2)` from `<asm/termbits.h>`. Kernel headers are required at
  build time; `ubuntu-latest` runners have them.
* **Windows** — the CH343 driver must be installed and must support 4 Mbps.
  Generic in-box USB-serial drivers may cap at 1.5 Mbps; check
  `SetCommState` failure in that case.

## License

MIT (see [`LICENSE`](LICENSE)). TinyFrame is vendored at
`vendor/TinyFrame/` with its own MIT license.
