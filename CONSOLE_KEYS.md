# Console keys (UART cheat-sheet)

The `scu200_ai_demo01` app is driven interactively over the board's serial console.

**Serial:** `115200` baud, `8N1`, no flow control (the design's `axi_uartlite`). Keys are
single-press, case-sensitive, and take effect live. Press **`?`** at any time for the on-device
list; this file is the annotated version.

---

## Motor driver
| Key | Action |
|-----|--------|
| `1`–`9` | set speed 10 %–90 % duty |
| `+` / `-` | speed ± 5 % |
| `0` | stop |
| `r` | reverse direction |
| `e` / `d` | enable / disable the driver |
| `f` | read/clear fault |
| `p` | print motor status |

## Motor encoder
| Key | Action |
|-----|--------|
| `c` | encoder self-check (reports counts / transitions) |

## Angle strobe (disc sync)
| Key | Action |
|-----|--------|
| `S` | arm / disarm the strobe |
| `F` | fire an FSIN trigger |
| `N` | captures per revolution: cycles `1 / 2 / 4 / 8 / 16` |
| `T` / `G` | target position ± 5 (clamped to `0 … cpr/N`, default mid-point; shown on the HDMI **TGT** gauge under the ROI preview) |
| `C` / `V` | **`cpr` ± 0.1** — the drift-null trim (see below) |
| `Y` | strobe status |

> **`cpr` trim (`C`/`V`).** The strobe angle is derived from the pre-gearbox encoder, so a small
> residual drift remains. `cpr` (counts per disc-revolution, default **979.6**) nudges the phase
> accumulator to null it out live. Watch the captured frames "hold still", then leave `cpr` there.
> The trim is clamped to **959.6 – 999.6** (default ± 20, so the default sits mid-range); the console
> appends `(min)`/`(max)` at a limit (`Y` prints the current value).
> See the *Known issues → triggered-capture drift* note in `README.md`.

## ROI crop (what the CNN sees)
| Key | Action |
|-----|--------|
| `z` / `x` | zoom in (smaller crop) / out (bigger) |
| `j` / `l` | pan left / right |
| `i` / `k` | pan up / down |
| `o` | recenter |

## Camera
| Key | Action |
|-----|--------|
| `]` / `[` | gain ± 0.5× (floor 1.0×) |
| `.` / `,` | exposure ± 256 lines |

## Overlay / display
| Key | Action |
|-----|--------|
| `h` | HUD stats panel + ROI box on/off |
| `v` | verbose console (1 Hz stats line) on/off |
| `?` | print this key list |

## Capture (training data)
| Key | Action |
|-----|--------|
| `w` | record ROI training data on/off — fills the DDR gallery for JTAG dump |

**Capture workflow:** press `w` to start recording, arm the strobe (`S`) and fire (`F`), then spin
the disc until the on-screen `REC` counter fills (e.g. `1024/1024`). Press `w` to stop, then dump
and label off-board — see *Stage 3* in `README.md`
(`scripts/dump_gallery.tcl` → `tools/roi_export.py`).

## Digit position (`scu200_ai_demo02` only)
The `mbdpu02` CNN also outputs where the digit sits in the ROI, in 28×28 model pixels
(0 = centred; **dx** left −/right +, **dy** top +/bottom −). On HDMI, a teal **DX** gauge runs
under the ROI preview and a **DY** gauge down its left side, scaled so each marker lines up with
the digit centre on the preview; the values print below (`DX +1.2`, `DY -0.4`).

| Key | Action |
|-----|--------|
| `A` | auto-target on/off — once per revolution, the mean **dx** of that revolution steps the strobe `target` toward centring (1 count per 0.5 px, max 5, deadband 0.5 px; dx > 0 raises target, wrapping around 0..max). Can only be turned on while `S` is armed and `F` is on, and switches itself off if either is turned off. The **TGT** gauge reads **AUTO** while it runs. |

> **Camera watchdog.** With the strobe armed and FSIN on, if the disc turns 3 revolutions without a
> completed frame, the OV9281 is assumed locked up and is reset and re-configured automatically
> (`cam: WATCHDOG #n ...` on the console, with the last auto target change for diagnosis). Each
> auto-target step is logged as `auto: mean dx=... -> target a->b [WRAP] (rev r pos p)`.

If the loop walks `target` the wrong way (e.g. after `r` reversed the motor), flip
`DX_TARGET_SIGN` in `main.c`. The verbose line (`v`) adds `DX=… DY=… AUTO=…`; recorded
captures (`w`) carry dx/dy, exported by `roi_export.py` as `manifest.csv` columns.

## Diagnostics
| Key | Action |
|-----|--------|
| `B` | test probe: frame-dones vs strobe revs (extra-frame ratio) + brightness min/max |
