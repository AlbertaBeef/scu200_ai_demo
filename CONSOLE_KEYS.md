# Console keys (UART cheat-sheet)

The app of every design (`scu200_ai_demo01`, `02`, `03`, `03_fcapz`) is driven interactively over
the board's serial console. Most keys are common to all designs; the ones that exist on only some
are marked. After programming, the standard setup is `S`, `F`, `q`×4 (VTS 544) and, on demo02/03,
`A`.

**Serial:** `115200` baud, `8N1`, no flow control (the design's `axi_uartlite`; `/dev/ttyUSB1` on
the dev host). Keys are
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
| `T` / `G` | target position ± 5 (clamped to `0 … cpr/N`, default mid-point; shown on the HDMI **TGT** gauge under the ROI preview; on demo03 the background's static **Target Offset** gauge) |
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
| `q` | trigger-mode frame length (VTS): cycles **1373 / 1024 / 800 / 640 / 544** lines (~18.2 µs per line; boot default 1373, standard setup 544). Applies while `F` is on; free-run always uses 1373. Also moves the PL drop window |
| `Q` | PL wake-frame drop (`fsin_frame_tag`, demo03 designs) on/off — **default off**. Only for the V0/V2 trigger variants, which send a dim wake frame; with the default V1 it would discard every frame |
| `K` | OV9281 FSIN trigger variant, `F` on only — **V1 pure snapshot (default: one frame per FSIN, SOF 1.86 ms)** → V2 snapshot + r_init_man → V3 ext_vs only, VTS max → V0 snapshot + ext_vs (old default: dim wake frame at 1.06 ms, good frame one VTS later). For comparison only; V0 and V2 need `Q` |

## Overlay / display
| Key | Action |
|-----|--------|
| `h` | HUD on/off: stats panel, ROI box, and the numeric DX/DY and target values (gauges and labels always shown; default off). On demo03 the DX/DY, 0–9 and "Target Offset" labels are part of the background image; the target value sits right of its gauge track |
| `u` | Overlay renderer. demo03 cycles **mbpost → full → delta** and boots in **mbpost** (the HLS core draws the preview, logit bars and DX/DY gauges straight into DDR, ~1.1 ms). demo01/02 toggle **full ↔ delta** (MicroBlaze paths, default full). `B` prints per-stage overlay timing (in mbpost mode `bars` = the mbpost run, `pos` = HUD value text) |
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

## Digit position (demo02 and demo03)
The `mbdpu02` and `mbdpu03` CNNs also output where the digit sits in the ROI (0 = centred;
**dx** left −/right +, **dy** top +/bottom −). Units are model pixels: 28-px units on demo02,
112-px units on demo03 (4× finer, so the same offset reads 4× larger). On HDMI, a teal **DX**
gauge runs under the ROI preview and a **DY** gauge down its left side, scaled so each marker lines
up with the digit centre on the preview; the values print below (`DX +1.2`, `DY -0.4`) when the HUD
(`h`) is on.

| Key | Action |
|-----|--------|
| `A` | auto-target on/off — once per revolution, the mean **dx** of that revolution steps the strobe `target` toward centring (1 count per 0.5 px on demo02 / 2 px on demo03, max 5, deadband one such unit; dx > 0 raises target, wrapping around 0..max). Can only be turned on while `S` is armed and `F` is on, and switches itself off if either is turned off. The **TGT** gauge reads **AUTO** while it runs (demo01/02; demo03's gauge label is the static "Target Offset", so check `AUTO=` on the verbose line). |

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
| `B` | test probe: frame-dones vs strobe revs (extra-frame ratio) + brightness min/max + trigger variant; per-stage overlay timing; on demo03 also the `fsin_frame_tag` counters (FSIN → SOF timer). For a clean overlay reading: `B`, wait ~7 s, `B` |

The trigger variant (`K`), frame length (`q`) and wake-frame drop (`Q`) are under **Camera**.
