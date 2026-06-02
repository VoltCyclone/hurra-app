# hurra-bridge UX polish — design

**Date:** 2026-06-02
**Status:** Approved (design); pending implementation plan
**Scope:** Terminal UX of the `hurra-bridge` daemon only. No protocol, library, or
firmware changes.

## Goal

Make `hurra-bridge` feel polished and end-user-ready for **non-expert operators** —
people who run the bridge to use Ferrum tools and are not expected to read C, decode
errno values, or interpret a scrolling log. Wins, in priority order:

1. Plain-English errors that say what went wrong **and what to do about it**.
2. Calm runtime output — a single live status line instead of a perpetual log.
3. Clear startup feedback and device discovery so the common case "just works".

## Constraints / non-goals

- **Single file.** All UI logic lives inside `src/bridge.c` (approach A). No new
  translation units, no `CMakeLists.txt` changes, no new link dependencies.
- **No app version string.** The banner header is just `hurra-bridge`. (The
  firmware's own probed version string may still be shown as link-health feedback.)
- **No driver-installed detection.** Confirmed "USB device present but no driver"
  detection (IOKit/SetupAPI) is explicitly out of scope. The firmware-not-responding
  error carries a driver hint instead.
- **No behavior change to the protocol surface.** Counters, the `__diag__`
  side-channel, telemetry, and all Ferrum command handling are unchanged — this work
  only changes how state is *presented*.
- **Cross-platform.** macOS, Linux, Windows must all build and behave sensibly.
  Windows keeps requiring `--virtual-port`; discovery is Unix-only.

## Architecture: the `ui` layer (Section 1)

A small block of static state + helpers near the top of `bridge.c`, initialized once
in `main` (after arg parse, before any user-facing output):

```c
static struct {
    bool color;       // color escapes allowed?
    bool status_tty;  // is stdout a live terminal (in-place status line ok)?
} g_ui;
```

`ui_init(const args_t *args)` resolves both flags:

- **color** = `stderr` is a TTY **AND** `getenv("NO_COLOR")` is unset/empty **AND**
  `--no-color` was not passed.
  - On Windows, when color would be enabled, also enable
    `ENABLE_VIRTUAL_TERMINAL_PROCESSING` via `GetConsoleMode`/`SetConsoleMode` on the
    stderr handle. If that call fails, color silently falls back to off.
- **status_tty** = `stdout` is a TTY. Gates the live in-place status line vs. plain
  periodic lines.

TTY detection: `isatty(fileno(stderr/stdout))` on Unix; `_isatty(_fileno(...))` on
Windows.

Color helpers emit an escape only when `g_ui.color` is set, so call sites stay
readable. Suggested minimal set (exact names at implementation time):

- `c_red`, `c_green`, `c_yellow`, `c_dim`, `c_reset` — return `""` when color off.
- Status glyphs degrade when color/UTF-8 is off: `✓`→`*`, `✗`→`x`/`!`, `~`→`~`.

A new CLI flag `--no-color` is added to `args_t`, `parse_args`, and `usage`.

## Startup banner (Section 2)

Replaces the scattered startup lines (`hurra: opened …`, `hurra: tx_batch=… `,
`PTY: …`, `Symlink: …`, `bridge: running.`) with one aligned block printed once.

**TTY success example:**

```
hurra-bridge

  ✓ Serial device   /dev/cu.usbmodem01 @ 4 Mbaud
  ✓ Virtual port    /dev/ttys004
    └ linked at     ~/.hurra-bridge.tty
  ✓ Firmware        responding (fw "<probed-version>")

  Ready. Point your Ferrum tool at ~/.hurra-bridge.tty
  Press Ctrl-C to stop.
```

Details:

- `✓` is green when color on; aligned plain markers when off.
- **Baud humanized:** `4000000` → `4 Mbaud`; otherwise `N baud` (e.g. `115200 baud`).
  Exact-million rates render as `N Mbaud`; others fall back to raw `N baud`.
- **Firmware line** comes from an initial `hurra_version` probe at startup:
  - Responds → `✓ Firmware  responding (fw "…")` and counters `probe_ok++`.
  - No response → `✗ Firmware  not responding` plus the remediation block from
    Section 4. **This is non-fatal** — startup continues and the link may recover.
- **Windows:** "Virtual port" line shows the com0com name; no `└ linked at` sub-line.
- **Auto-detected device** appends `(auto-detected)` to the Serial device line
  (Section 4b).
- **Non-TTY** (piped/redirected): same facts as plain prefixed lines, e.g.
  `hurra-bridge: serial device /dev/… @ 4 Mbaud`, `hurra-bridge: ready (…)`, so logs
  stay greppable. No box drawing.

## Live status line (Section 3)

Once `Ready`, the every-5s `heartbeat …` log and the per-move `move(x,y)` logs are
**replaced** (for the TTY case) by a single line refreshed in place:

```
⠋ running · 1m24s · 12,480 moves · link ✓
```

- **Spinner:** Braille frames `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`, advancing one frame per refresh
  ("little running animation"). ASCII fallback `|/-\` when color/UTF-8 is off.
- **Refresh:** ~125 ms cadence, driven off the existing main-loop clock
  (`mono_ms()`); redraw uses `\r` + clear-to-end-of-line (`\x1b[K` when color/VT on,
  else pad-with-spaces). Never scrolls.
- **Fields:** humanized uptime; cumulative `ferrum_moves` with thousands separators;
  link health `✓` ok / `✗` dead / `~` flapping (colored), derived from the existing
  `probe_ok`/`probe_fail` logic.
- **Event lines still print** above the status line for meaningful transitions so
  scrollback keeps a trail: e.g. `✗ firmware stopped responding`,
  `✓ firmware back`. Routine ticks do not print.
  - Implementation note: printing an event line must first clear the current status
    line (`\r\x1b[K`), emit the event + newline, then let the next tick redraw the
    status line.
- **Non-TTY fallback:** no spinner, no in-place redraw; keep the current periodic
  plain line at the existing 5 s cadence so log files still get datapoints.
- **`__diag__` side-channel and all counters are unchanged** — only display changes.

## Diagnostic errors with fixes (Section 4)

A central helper — `fail(category, detail…)` — formats fatal startup errors as
**what happened → likely cause → what to do**, colored (red `✗`, dimmed hint) when
color is on, and returns the process exit code. Categories map to the real failure
sites in the codebase:

### Device won't open (`hurra_open` → `serial_open` failure)

Branch on `errno` read immediately after the failed open. **Dependency:** confirm
during planning that `hurra_open` preserves `errno` from the underlying
`serial_open`; if it does not, the implementation must surface the errno (e.g. read
`errno` right after the failed call, since the serial layer sets it, or thread a
status out of `hurra_open`). The spec assumes errno is available at the call site.

- `ENOENT` — no such device:
  ```
  ✗ Can't open serial device: /dev/cu.usbmodem01
    No such device. Is it plugged in?
    → Available serial ports:
        <discovered list, or "(none found)">
  ```
  (Appends the discovery list from Section 4b.)
- `EACCES` — permission denied:
  ```
  ✗ Permission denied: /dev/ttyUSB0
    Your user isn't allowed to use this serial port.
    → Add yourself to the 'dialout' group, then log out and back in:
        sudo usermod -aG dialout $USER
  ```
- `EBUSY` — in use:
  ```
  ✗ Device is in use: /dev/ttyUSB0
    Another program (another bridge instance?) already holds this port.
  ```
- other — friendly frame around `strerror(errno)`.

Exit code: `1`.

### Firmware not responding (startup probe failed)

```
✗ Firmware isn't responding on /dev/cu.usbmodem01
  The serial port opened, but the device isn't answering.
  Likely causes:
    • Wrong baud rate — firmware default is 4 Mbaud (try without --baud)
    • USB-serial driver too slow for 4 Mbaud
      (macOS: install the WCH CH34x VCP driver)
    • Wrong device — this port may be something else
```

**Non-fatal:** printed as a warning; the bridge keeps running and the status line
reflects `link ✗` until/unless the firmware answers.

### Missing `--virtual-port` (Windows) / `vp_open` failure

```
✗ No --virtual-port given (required on Windows)
  hurra-bridge needs a com0com virtual COM pair.
  → Install com0com, create a pair with setupc.exe (e.g. CNCA0 ↔ CNCB0),
    then run:  hurra-bridge.exe --device COM5 --virtual-port CNCA0
```

`vp_open` failure (both platforms) gets a similarly framed message; on Windows it
includes the `GetLastError` value as a trailing dim detail for support purposes.

Exit code: `1`.

### Usage / bad flag

- Unknown option → `✗ unknown option: --foo` followed by the (reworded, friendlier)
  `usage()` text. Exit code `2`.
- `-h`/`--help` → `usage()` to stdout, exit `0`.

## Device discovery & auto-select (Section 4b)

`--device` becomes **optional on Unix** when discovery can resolve it; it stays
explicit on Windows. `discover_devices()` enumerates likely serial ports and labels
WCH/USB-serial candidates first.

- **macOS:** glob `/dev/cu.usbmodem*`, `/dev/cu.wchusbserial*`, `/dev/cu.usbserial*`.
- **Linux:** glob `/dev/ttyACM*`, `/dev/ttyUSB*`. Optionally read-only consult
  `/sys/bus/usb/devices/*/idVendor == 1a86` purely to *label* WCH ports — no new
  dependency, and skipped silently if `/sys` is unavailable.
- **Windows:** no discovery (would need registry/SetupAPI, declined). `--device` and
  `--virtual-port` remain required as today.

Behavior when `--device` is omitted (Unix):

- **Exactly one candidate** → auto-select; banner shows
  `✓ Serial device  <path> @ 4 Mbaud  (auto-detected)`.
- **Multiple candidates** → do not guess:
  ```
  ✗ No --device given, and found several serial ports:
      /dev/cu.wchusbserial01   (WCH USB-serial)
      /dev/cu.usbmodem2101
    → Re-run with one, e.g.:
        hurra-bridge --device /dev/cu.wchusbserial01
  ```
  Exit code `2`.
- **None found** →
  ```
  ✗ No serial devices found. Is the device plugged in?
    (Looked for /dev/cu.usbmodem*, /dev/cu.wchusbserial* …)
    → If it's plugged in but not listed, you may need the WCH CH34x driver.
  ```
  Exit code `2`.

An explicit `--device` always wins; discovery only runs when it is omitted. The
`ENOENT` open error (Section 4) reuses `discover_devices()` to list what *is*
available.

## Shutdown summary (Section 5)

On `SIGINT`/`SIGTERM` (existing `g_stop` flag):

- Finalize the live status line: emit a clearing `\r\x1b[K` (TTY) then a newline so
  the cursor leaves the spinner cleanly.
- Print a calm one-time summary reusing existing counters:
  ```
  Stopping hurra-bridge.
    Ran for 4m12s · 12,480 moves · firmware link ok
  ```
  Link health shows final state: `ok` / `was unreachable` / `flapped`.
- Existing orderly cleanup (`ferrum_parser_destroy`, `vp_close`, `hurra_close`) runs
  unchanged afterward.
- Non-TTY: same summary as a plain line; no cursor manipulation.

## Affected code (all in `src/bridge.c`)

- **New:** `ui` static state + `ui_init`, color/glyph helpers, `fail(...)`,
  `discover_devices(...)`, baud/uptime/number humanizers, status-line render +
  event-line print, shutdown summary.
- **Changed:** `usage()` (friendlier + `--no-color`), `args_t`/`parse_args`
  (`--no-color`, `--device` optional on Unix), `main` startup block, the
  open/`vp_open`/probe failure paths, the main-loop heartbeat → status-line, the
  shutdown block.
- **Unchanged:** all `cb_*` handlers, telemetry handlers, `__diag__`, counters,
  protocol/library/serial/virtual-port modules.

## Testing strategy

Manual + light automation (this is an interactive daemon; full automated coverage of
terminal rendering is out of scope):

1. **Build matrix** — must compile cleanly on macOS, Linux, Windows (existing CI).
2. **TTY happy path** — run in a real terminal: banner aligns, spinner animates,
   status line refreshes in place without scrolling, Ctrl-C prints summary cleanly.
3. **Non-TTY** — `hurra-bridge … | cat` and `… > log 2>&1`: no escape codes, plain
   periodic lines, greppable banner/summary.
4. **`NO_COLOR=1`** and **`--no-color`** — no escape sequences; glyphs degrade to
   ASCII; alignment preserved.
5. **Error paths** (no hardware needed): unplugged device (`ENOENT` + discovery
   list), unreadable device (`EACCES` message), missing `--device` with zero / one /
   many candidates, missing `--virtual-port` on Windows, unknown flag.
6. **Firmware-silent** — point at a port with nothing listening: non-fatal warning,
   `link ✗` in status line, recovery flips to `link ✓` and prints `✓ firmware back`.
7. **Exit codes** — `0` help, `1` fatal runtime/open failure, `2` usage/discovery.

## Open items to resolve during planning

- Confirm `hurra_open` makes the underlying `errno` observable at the bridge call
  site (drives the Section 4 errno branching). If not, decide the surfacing mechanism.
- Confirm a portable clear-to-EOL approach for the non-VT path (pad-with-spaces width
  tracking) and UTF-8 capability assumption for the Braille spinner (tie the Braille
  vs. ASCII choice to `g_ui.color`/VT availability to stay safe).
