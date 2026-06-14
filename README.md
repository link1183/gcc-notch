# gcc-notch

A Linux calibration and notch-remapping tool for GameCube controllers connected
through a Mayflash-style USB adapter. It corrects worn sticks so the analog
output reaches a clean, symmetric octagonal gate again, remaps the corrected
signal onto a virtual controller that games actually read, and doubles as a
stream overlay and speedrun input-stats tracker.

It targets the device that identifies itself as
`mayflash limited GameCube Controller Adapter`.

---

## Table of contents

- [What it does](#what-it-does)
- [How it works](#how-it-works)
- [Building](#building)
- [Installing](#installing)
- [Permissions](#permissions)
- [Running](#running)
  - [Command-line flags](#command-line-flags)
  - [Keyboard shortcuts](#keyboard-shortcuts)
- [Calibration](#calibration)
  - [Stick calibration](#stick-calibration)
  - [Trigger calibration](#trigger-calibration)
  - [Button mapping](#button-mapping)
  - [Tuning knobs](#tuning-knobs)
- [Remapping](#remapping)
- [Profiles](#profiles)
- [Stream viewer & skins](#stream-viewer--skins)
  - [Skin format](#skin-format)
- [Speedrun stats & LiveSplit](#speedrun-stats--livesplit)
- [Exports](#exports)
- [Files on disk](#files-on-disk)
- [Source layout](#source-layout)

---

## What it does

- **Recalibrates worn sticks.** A guided wizard captures the controller's real
  centre and its eight physical gate notches, then builds a per-sector affine
  correction that maps the worn geometry back onto an ideal octagon.
- **Remaps to a virtual controller.** While remapping, the physical device is
  grabbed exclusively and a `uinput` mirror publishes the corrected output, so
  every game sees clean values without per-game configuration.
- **Rescales analog triggers** so a calibrated rest-to-full squeeze spans the
  full output range, with an adjustable bottom deadzone.
- **Visualises the sticks live** — raw vs. remapped dots, the measured notch
  polygon, the ideal octagon, and a fading input trail.
- **Watches for drift.** During normal use it passively compares live behaviour
  against the saved calibration and nudges you to recalibrate once a stick wears
  past it.
- **Drives a stream overlay.** A skinnable viewer (in-window or as a separate,
  size-locked window for OBS) renders button presses, stick positions and
  trigger fills over a custom background.
- **Tracks input statistics**, including per-run button counts and APM, wired to
  dusklight's LiveSplit timer so each speedrun is logged automatically.

---

## How it works

The program is one binary that runs in a few different roles. Input handling is
decoupled from rendering so remapping keeps working even when the GUI window is
hidden or throttled by the compositor.

```
                    ┌──────────────────────────────────────────┐
  physical adapter  │  engine.c                                 │
  /dev/input/eventN │   • libevdev read + exclusive grab        │
        │           │   • per-sector affine notch correction    │   uinput
        └──────────▶│   • libevdev-uinput mirror (remap output) │──────────▶ games
                    │   • input stats + passive drift watch     │
                    │   ── background IO thread (~2 kHz) ──      │
                    └──────────────────────────────────────────┘
                         ▲                         ▲
            engine API   │                         │   skin API
                    ┌────┴─────────┐        ┌──────┴───────┐
                    │  ui.c        │        │  skin.c      │
                    │  raylib +    │        │  XML-defined │
                    │  raygui GUI  │        │  overlays    │
                    └────┬─────────┘        └──────────────┘
                         │ TCP 127.0.0.1:16834
                    ┌────┴─────────┐
                    │ livesplit.c  │  LiveSplit Server listener (run timing)
                    └──────────────┘
```

- **engine.c** owns the device, the calibration math, the uinput mirror, the
  statistics and the drift estimators. A background thread pumps the input loop
  independently of any render loop; a single recursive mutex serialises every
  public entry point.
- **ui.c** is the raylib/raygui front-end: stick plots, panels, the calibration
  modals, the stats overlay, and `main()`. It also hosts the headless daemon and
  the standalone viewer roles.
- **skin.c** loads and draws XML-defined overlays.
- **livesplit.c** stands up a minimal LiveSplit Server listener so a run timer
  can drive start/end of stats capture.

---

## Building

Dependencies (development headers):

- `gcc`, `make`, `pkg-config`
- [`libevdev`](https://www.freedesktop.org/wiki/Software/libevdev/)
- [`raylib`](https://www.raylib.com/) (5.x) — bundles `raygui.h` is vendored
- `libxml-2.0`
- a C math library + pthreads (linked automatically)

On Arch:

```sh
sudo pacman -S base-devel libevdev raylib libxml2
```

Build:

```sh
make            # -> build/gcc-notch-ui
make clean
```

The UI looks best with **JetBrainsMono Nerd Font** (Medium + Bold) installed at
`/usr/share/fonts/TTF/`; without it the program falls back to raylib's built-in
font.

---

## Installing

```sh
sudo make install                 # PREFIX defaults to /usr/local
sudo make install PREFIX=/usr     # or pick your own prefix
```

This installs the binary as `gcc-notch-ui` and a `.desktop` launcher. Set
`DESTDIR` for staged/packaged installs.

---

## Permissions

Remapping needs:

- **read** access to the adapter's `/dev/input/event*` node, and
- **write** access to `/dev/uinput` to create the virtual mirror.

The simplest setup is a udev rule plus group membership, e.g.:

```sh
# /etc/udev/rules.d/99-gcc-notch.rules
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

Add yourself to the `input` group (`sudo usermod -aG input "$USER"`) and re-login.
Without these, calibration and the live viewer still work, but **Start Remap**
will fail to grab the device or create the mirror.

---

## Running

Launch the GUI:

```sh
gcc-notch-ui                 # auto-detects the adapter
gcc-notch-ui /dev/input/event7   # or point it at a specific node
```

On a tiling compositor the editor maximises to fill the screen; its layout is a
fixed design canvas scaled to fit, so it stays usable at any window size.

### Command-line flags

| Flag                | Effect                                                                 |
| ------------------- | ---------------------------------------------------------------------- |
| *(none)*            | Full editor GUI.                                                       |
| `<devpath>`         | Open a specific `/dev/input/eventN` instead of auto-detecting.        |
| `--remap`           | Start remapping immediately on launch (once calibrated & connected).  |
| `--daemon`, `-d`    | Headless: load the saved profile, remap, and run with no window. Needs an existing calibration. Stop with `Ctrl-C`. |
| `--viewer`          | Run as a standalone, size-locked overlay window (see [stream viewer](#stream-viewer--skins)). |

### Keyboard shortcuts

**Editor:**

| Key     | Action                                  |
| ------- | --------------------------------------- |
| `Space` | Start / stop remap                      |
| `C`     | Calibrate sticks                        |
| `T`     | Calibrate triggers                      |
| `S`     | Open input statistics                   |
| `R`     | Reload the active profile from disk     |
| `V`     | Toggle the in-window stream viewer      |
| `Esc`   | Cancel the open modal, or quit          |

**Stream viewer** (in-window or standalone process):

| Key       | Action                                            |
| --------- | ------------------------------------------------- |
| `V` / `Esc` | Exit the viewer                                 |
| `K`       | Cycle to the next skin                            |
| `R`       | Reload skins from disk                            |
| `B`       | Cycle background: black → chroma green → magenta  |
| `N`       | Toggle the numeric stick value readout            |
| `F`       | Toggle borderless (undecorated) window            |
| `H`       | Pin the keybind hint bar on screen                |

---

## Calibration

All wizards run as modal overlays in the editor and write to the active profile
on completion.

### Stick calibration

Press **Calibrate Sticks** (or `C`). For the control stick, then the C-stick:

1. **Spin** the stick around its full gate so the tool can discover which two
   axes move (it picks the two with the largest travel).
2. **Orient** — push and hold fully *right* so it can tell the X axis from the Y
   axis and their sign.
3. **Centre** — let the stick rest, then capture the resting position.
4. **Notches** — hold each of the eight gate notches in turn (8 captures).

From those samples it sorts the notches by angle, matches them to the canonical
octagon directions, and builds a per-sector affine map. The map is rebuilt and
saved immediately, and the drift baseline is reset.

### Trigger calibration

Press **Calibrate Triggers** (or `T`):

1. Let both triggers **rest** (fully released).
2. **Squeeze** both fully down.

Every analog L/R axis with real range is then rescaled so rest→full spans the
axis output. Digital-only triggers are left alone.

### Button mapping

Press **Map Buttons**. Press each prompted GameCube button (A, B, X, Y, Z, L, R,
Start) once; **Skip** any your controller lacks. The D-pad hat axes are detected
automatically afterwards. The mapping lets the stats panel and the stream viewer
label inputs by their GameCube names instead of raw evdev codes.

### Tuning knobs

- **Diagonal (per stick):** `0.30–1.00`, how far the diagonal notches sit
  relative to the cardinals. Slider under each stick plot.
- **Deadzone:** `0.00–0.30`, a radial centre deadzone (snap-to-centre inside,
  magnitude ramp outside).
- **Trigger deadzone:** `0.00–0.40`, a bottom deadzone applied after trigger
  rescaling.

The remap also applies a radial clamp so an overshoot or imperfect calibration
can never push the output past full-stick radius.

---

## Remapping

**Start Remap** (or `Space`) grabs the controller exclusively and creates a
`uinput` mirror named *GCC Notch Remap* carrying the corrected sticks and the
rescaled triggers; all other events pass through untouched. A pulsing **REMAP
ACTIVE** badge shows in the header.

The intent persists across disconnects and port switches: unplug/replug or use
the **Port** button to cycle adapters, and remapping resumes automatically once
the device returns. Because the input loop runs on its own thread, remapping
keeps running even if the window is hidden or the compositor throttles it.

For a no-GUI setup (e.g. launched from a session autostart), use `--daemon`.

---

## Profiles

Calibration lives in named profiles under `profiles/<name>.conf`; the selected
one is remembered in `active`. From the editor's Controls panel you can:

- pick a profile from the dropdown (selecting one loads it),
- **New** — save the current calibration under a new name,
- **Delete** — remove a profile,
- **Reload cfg** (`R`) — re-read the active profile from disk,
- **Reset Cal.** — wipe calibration in the active profile.

A legacy single-file `calib.conf` is migrated into `profiles/default.conf` once,
automatically, the first time no profiles exist.

---

## Stream viewer & skins

The viewer renders a skin — a background plus overlay images that light up or
move with live input — scaled and centred to the window.

Two ways to run it:

- **In-window** (`V` in the editor): replaces the editor view.
- **Standalone window** ("Open Viewer Window", or launch with `--viewer`): a
  separate process whose window is **locked to the skin's exact pixel size** with
  no borders, so OBS sees only the skin with no background bleed. It floats well
  under a Hyprland rule matched on the window class `GCC Notch Viewer`.

The control process publishes which device the viewer should read (the physical
controller when idle, the virtual remap mirror while remapping) so the overlay
always reflects what games receive.

### Skin format

Drop a folder into `~/.config/gcc-notch/skins/<name>/` containing a `skin.xml`
and its images. Coordinates and sizes are in **background-pixel space**; the
whole skin is scaled uniformly to the window.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<skin name="Tron Style with D-Pad" author="Aliensqueakytoy" type="gamecube">

    <background image="Background.png" />

    <!-- Buttons: image drawn only while the input is pressed. -->
    <button name="a"     image="A fill.png" x="669" y="133" width="117" height="117" />
    <button name="start" image="Start.png"  x="507" y="39"  width="58"  height="58"  />
    <button name="up"    image="Dpad top.png" x="483" y="136" width="38" height="51" />

    <!-- Triggers can show both a digital press and an analog fill. -->
    <button name="r"     image="Trigger.png" x="231" y="27" width="208" height="39" />
    <analog name="trig_r" image="Trigger.png" x="231" y="27" width="208" height="39"
            direction="left" reverse="false" />

    <!-- Sticks: image translates with the (corrected) stick position. -->
    <stick xname="lstick_x" yname="lstick_y" image="L stick.png"
           x="59"  y="120" width="113" height="113" xrange="56" yrange="56" />
    <stick xname="cstick_x" yname="cstick_y" image="C stick.png"
           x="293" y="138" width="80"  height="80"  xrange="50" yrange="50" />
</skin>
```

Element reference:

| Element       | Key attributes | Meaning |
| ------------- | -------------- | ------- |
| `<background>`| `image` | Base layer; defines the skin's native pixel size. Required. |
| `<button>`    | `name`, `image`, `x y width height` | Drawn only while pressed. `name` ∈ A,B,X,Y,Z,L,R,Start, or D-pad `up/down/left/right`. |
| `<stick>`     | `xname` (`lstick_x` / `cstick_x`), `image`, `x y width height`, `xrange yrange` | Image translates with the stick; `xrange`/`yrange` are the max pixel travel from centre. |
| `<analog>`    | `name` (`trig_l` / `trig_r`), `image`, `x y width height`, `direction` (`right`/`left`/`up`/`down`), `reverse` | Progressive reveal proportional to trigger pull; `direction` is the fill anchor, `reverse` inverts the fraction. |

Up to 32 skins with 64 elements each are loaded; only skins with a valid
background are kept. The selection persists in the `skin` file. A working
reference skin ("Tron V2 with D-Pad") is included under `skin_examples/` (not
tracked in git) — copy it into the skins directory to try it.

---

## Speedrun stats & LiveSplit

The engine counts button press edges, input events and D-pad direction edges
globally (persisted, resettable). The **Stats** overlay (`S`) breaks these down
per GameCube button, shows lifetime totals, the current/last run, and an
aggregate run summary (best/avg/median/worst presses, avg/best APM) plus a rolling
3-second APM readout.

Per-run tracking is driven by **dusklight's LiveSplit integration**. The program
listens as a LiveSplit Server on `127.0.0.1:16834` and interprets the commands
dusklight pushes:

- `starttimer` — begins a run (snapshots the press counters),
- `setgametime H:MM:SS.mmm` — the authoritative live game time,
- run **end** — inferred when the `setgametime` stream goes quiet (~2 s) or the
  next `starttimer` arrives (there is no explicit stop command).

On each finished run the press delta and final game time are banked, appended to
`runs.log`, and added to the in-memory history (last 200 runs, preloaded on
start so history survives restarts).

---

## Exports

From the Stats overlay:

- **Export CSV** — a full report: summary header comments plus one row per run.
- **Run CSV** — the selected run as a single row, plus a per-button breakdown for
  the just-finished run.
- **Run PNG** — a shareable, supersampled result card for the selected run
  (rendered with high-res fonts outside the normal draw pass for crisp text).

All exports land in the config directory, named by timestamp.

---

## Files on disk

Everything lives under `~/.config/gcc-notch/`:

| Path                      | Contents |
| ------------------------- | -------- |
| `profiles/<name>.conf`    | Calibration profiles (axes, centre, notches, diag, deadzones, triggers, button map, D-pad). |
| `active`                  | Name of the selected profile. |
| `calib.conf`              | Legacy single-file config; migrated to `profiles/default.conf` once. |
| `stats.conf`              | Lifetime input statistics. |
| `runs.log`                | One finished run per line: `unix_time \t game_ms \t presses`. |
| `skins/<dir>/skin.xml`    | Installed skins (+ their images). |
| `skin`                    | Selected skin directory name. |
| `viewer`                  | Viewer prefs: `background borderless values pin`. |
| `viewer_src`              | Devnode the viewer process should read (written by the control process). |
| `stats-*.csv`, `run-*.csv`, `run-*.png` | Timestamped exports. |

Config files are written atomically (temp file + `rename`) where it matters.

---

## Source layout

| File           | Responsibility |
| -------------- | -------------- |
| `engine.c/.h`  | Device I/O, calibration math, uinput remap mirror, stats, drift watch, profiles, the background input thread. |
| `ui.c`         | raylib/raygui editor, calibration/trigger/button-map modals, stats overlay & exports, daemon + standalone-viewer roles, `main()`. |
| `skin.c/.h`    | XML skin loading and drawing. |
| `livesplit.c/.h` | LiveSplit Server listener and run-state parsing. |
| `raygui.h`     | Vendored immediate-mode GUI (third-party). |
| `Makefile`     | Build + install. |
| `skin_examples/` | Reference skin. |
