# hurra-app

Native C adapter library for the [Hurra binary protocol](https://github.com/ramseymcgrath/imxrtnsy),
the host-side counterpart to the iMXRT (Teensy MicroMod) firmware in
[`imxrtnsy`](https://github.com/ramseymcgrath/imxrtnsy).

The library opens a serial port to a kmbox-class device running Hurra
firmware, encodes/decodes [TinyFrame](https://github.com/MightyPork/TinyFrame)
frames per the [protocol spec](https://github.com/ramseymcgrath/imxrtnsy/blob/main/docs/specs/2026-05-23-hurra-binary-protocol-design.md),
and exposes a small C API for mouse/keyboard control + telemetry callbacks.

* **Targets**: macOS, Linux, Windows (build matrix in CI).
* **Baud**: 4 Mbps default; arbitrary rates supported via platform-specific
  custom-baud APIs (IOSSIOSPEED on macOS, `termios2`/BOTHER on Linux,
  `DCB.BaudRate` on Windows).
* **Threading**: the library does not spawn its own threads — call
  `hurra_poll()` frequently from your loop to drain RX and dispatch
  telemetry/reply callbacks.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Produces `build/libhurra.a` (or `hurra.lib` on Windows) and the example
binary `build/hello`.

On Unix you can also just `make`, which is a thin wrapper around the two
CMake commands.

## Use

```c
#include <stdio.h>
#include "hurra.h"

int main(void) {
    hurra_client_t *c = hurra_open("/dev/cu.usbmodem01", 4000000);
    if (!c) return 1;

    char version[64];
    if (hurra_version(c, version, sizeof(version), 1000) == 0)
        printf("version: %s\n", version);

    uint64_t rtt_us;
    if (hurra_ping(c, &rtt_us, 1000) == 0)
        printf("ping: %llu us\n", (unsigned long long)rtt_us);

    hurra_move(c, 50, 0);

    hurra_close(c);
    return 0;
}
```

The full API lives in [`include/hurra.h`](include/hurra.h); wire-protocol
TYPE byte constants are in [`include/hurra_types.h`](include/hurra_types.h).

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
