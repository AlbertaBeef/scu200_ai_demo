# scu200_ai_demo03 pipeline profiling

**Goal:** raise the maximum disc speed while keeping two guarantees:
- every strobe is captured and inferred;
- the HDMI output never tears.

All timings come from **fcapz** event-timeline captures on the `scu200_ai_demo03_fcapz` target, unless a row says otherwise. Overlay breakdowns come from the `B` console key. Measured 2026-11-25 to 2026-11-26.

**Where things are:**
- Captures: [`captures/`](captures/), gzipped fcapz JSON.
- Commands to re-derive every number: [How to reproduce](#how-to-reproduce).

## Result

| | First fcapz build (07af226) | Now (d7308dd, mbpost) |
|---|---|---|
| Max RPM, every frame inferred (N=8) | ~110 | **358** (verified), ~380 (camera limit) |
| Max RPM, every strobe answered (N=8) | ~145 | ~380 |
| Good frame starts after FSIN | 26.05 ms | **10.96 ms** |
| Frame ready after FSIN | ~34.9 ms | **19.8 ms** |
| `mbdpu03_infer()` | 1.00 ms | 1.00 ms |
| Overlay draw | **66.1 ms** (MicroBlaze) | **1.14 ms** (mbpost HLS) |
| Worker done after FSIN | 86–102 ms | **22.0 ms** |
| Display collisions (tearing) | 103 in 10 s | **0** |

**The limit is now the camera chain.** A strobe period shorter than ~19.8 ms (above ~380 RPM at N=8, or about 84 % motor) can't complete a frame before the next FSIN. The worker is idle ~17 ms of every 21 ms cycle at 358 RPM.

## Pipeline, per strobe (now, cap22 at 358 RPM)

| Stage | Start after FSIN | Duration |
|---|---|---|
| FSIN strobe (`disc_strobe`) | 0 | 10 µs |
| OV9281 wake frame (dropped in the PL by `fsin_frame_tag`) | 1.06 ms | — |
| Good frame: SOF, then `frmbuf_wr` writes the ring slot | 10.96 ms | 8.83 ms |
| `frmbuf_wr` IRQ → ISR (`FrmbufWrCallback`) | 19.79 ms | 8–12 µs |
| Worker takes frame; display slot held | +0.02 ms | — |
| `mbdpu03_infer()` (includes the camera-crop read burst, 0.162 ms) | 19.87 ms | 1.00 ms |
| mbpost overlay (preview, bars, DX/DY gauges) + HUD values | 20.87 ms | 1.14 ms |
| Worker done, background buffer flipped | 22.0 ms | — |

**Remaining stall:** about once per revolution, the auto-target UART print plus the target-gauge redraw delays the worker by up to ~10 ms (max worker done 32 ms). It cost no frames at 358 RPM, but it will above ~370 RPM.

## Milestones and the captures behind them

RPM is computed as 60 000 / (FSIN period × N).

| Capture | Build / settings | FSIN period | Answered | Overlay | Worker done | Display collisions | Finding |
|---|---|---|---|---|---|---|---|
| cap1 | 07af226, VTS 1373, N=8, ~78 RPM | 96.6 ms | 100 % (2 frames/strobe) | 66.1 ms | 86 ms | **103** | Wake frame + good frame per strobe; ISR done-slot wrong 209/209 → tearing |
| cap2 | 43fe34e (PL wake-frame drop, slot tracking) | 100.3 ms | 100 % (1/strobe) | 66.3 ms | 102 ms | 0 | Tearing fixed; good frame at 26.05 ms |
| cap3 | 43fe34e, ~171 RPM | 43.8 ms | **50 %** | 66.2 ms | 104 ms | 0 | Camera re-arm: 2 × 25 ms sensor frames per strobe → every other strobe ignored |
| cap6 | 43fe34e, ~126 RPM | 59.3 ms | 100 % | 65.8 ms | 135 ms | 0 | Worker limit: 67 ms/frame → 12 % of frames skipped |
| cap7 | VTS 544, PL drop off, N=16 | 46.1 ms | 100 % (wake frames back) | 66.3 ms | 113 ms | 77 | Shorter VTS works; without the drop, wake frames overrun the ring |
| cap8 | VTS 544 + 6 ms drop window (d6fa43e work), N=16 | 49.5 ms | 100 % (1/strobe) | 66.2 ms | 120 ms | 2 | Good frame at 10.96 ms; worker now the only limit |
| cap9 | same, N=16, ~126 RPM | 29.7 ms | 100 % | 66.4 ms | 116 ms | 83 | Camera keeps up at 30 ms; stale displayed slot overwritten → tearing |
| cap12 | N sweep at motor 40 %: N=16, ~180 RPM | 20.8 ms | 100 % | 65.7 ms | 111 ms | 79 | Worker far behind: 80 of 259 frames displayed; digits out of order |
| cap16 | N sweep: N=6, ~173 RPM | 57.7 ms | 100 % | 65.9 ms | 121 ms | 0 | Fits, but every frame is late |
| cap19 | delta overlay (changed bars only), N=8, ~171 RPM | 43.9 ms | 100 % | 28.7 ms | 49.6 ms | 0 | Every strobe inferred, digits in sequence |
| cap20 | d6fa43e: delta, all 10 bars every frame, N=8, ~167 RPM | 45.0 ms | 100 % | 42.4 ms | 64.6 ms | 0 | Clean, but only 1.6 ms mean slack → ~173 RPM ceiling |
| cap22 | **d7308dd mbpost**, N=8, **357 RPM** (motor 80 %) | 21.0 ms | 100 % | **1.14 ms** | **22.0 ms** | 0 | 173 frames inferred, digits 5678 1234 … with 0 out of order |

The N-sweep captures cap13–cap15 (N=12, 10, 8) and cap21 (mbpost, 77 → 216 RPM ramp) aren't kept. They're consistent with their neighbours: 90, 70 and 6 display collisions for cap13–15, and 0 for cap21.

## Overlay draw cost (`B` key)

| Mode | Total | Preview | Bars | Live flush | DX/DY gauges | Gauge flush |
|---|---|---|---|---|---|---|
| Full redraw (MicroBlaze) | 66 ms | 13.0 | 24.9 | 8.7 | 13.7 | 5.9 |
| Delta, changed bars only | 26 ms (fcapz: 28.7) | — | — | — | — | — |
| Delta, all 10 bars full width (d6fa43e default) | 42.3 ms | 13.0 | 23.9 | 2.1 | 2.9 | 0.3 |
| **mbpost HLS** (d7308dd default) | **1.13 ms** | in core | 1.127 (whole core) | 0 | 0.006 (HUD text) | 0 |

**Why the MicroBlaze is slow:**
- It runs at 100 MHz with a 32 KB write-back D-cache and 16 B lines.
- The overlay touches ~190 KB of the 1920×1080 RGB888 frame and misses on nearly every line.
- So the cost scales with bytes touched, not with drawing logic.

**mbpost** composes each row span over the pristine background (`BG_IMG_ORIG`) and burst-writes it to the back buffer, bypassing the CPU cache.
- HLS estimate: 0.53–0.88 ms.
- Post-synthesis: 5.3k LUT, 12 BRAM, 3 DSP.
- `u` cycles mbpost → full → delta for A/B comparisons.

## Camera: frame length (VTS) vs good-frame arrival

The OV9281 sends a wake frame ~1.06 ms after FSIN, then the good frame one frame length later. Each line is ~18.2 µs (HTS 1456).

| VTS (lines) | Frame length | Good frame starts after FSIN | Strobes answered at a 46 ms period |
|---|---|---|---|
| 1373 (default) | ~25 ms | 26.05 ms | 50 % |
| 1024 | ~18.6 ms | 19.70 ms | 100 % |
| 800 | ~14.6 ms | 15.62 ms | 100 % |
| 640 | ~11.6 ms | 12.71 ms | 100 % |
| 544 (current, `q`×4) | ~9.9 ms | 10.96 ms | 100 % |

The PL drop window must close between the wake frame and the good frame. At VTS 544 it is 6010 µs, set from the console with `Q`.

**Next levers on the camera chain (not done):**
- fewer readout rows (e.g. 448 instead of 496);
- a shorter HTS;
- getting the sensor to send one frame per FSIN instead of a wake frame plus a good frame.

## Motor setting vs RPM

Medians of ~15 000 console status lines. The first 3 lines after each speed change are skipped as spin-up. Strobe periods are at N=8.

| Motor | 10 % | 20 % | 30 % | 40 % | 50 % | 60 % | 70 % | 80 % | 90 % |
|---|---|---|---|---|---|---|---|---|---|
| RPM | 34 | 78 | 127 | 172 | 220 | 268 | 315 | 361 | 409 |
| Strobe period | 220 ms | 96 ms | 59 ms | 43.6 ms | 34.1 ms | 28.0 ms | 23.8 ms | 20.8 ms | 18.3 ms |

The relationship is close to linear, at about 47.5 RPM per 10 %. 90 % (18.3 ms) is below the 19.8 ms camera chain, so it isn't reachable yet.

## Published timelines

These are private claude.ai artifacts: interactive swimlanes with wheel/pinch to zoom.

| Capture | Timeline |
|---|---|
| cap1 (before the tearing fix) | https://claude.ai/artifact/6jTNKfSmCT8HwrLQjSx6VP |
| cap2 (after the fix, ~75 RPM) | https://claude.ai/artifact/9xurB8QC9uj8H6Yih1FxsR |
| cap3 (171 RPM, every other strobe) | https://claude.ai/artifact/6Ba2C8N9zBmMN1ZujjRdU4 |
| cap6 (worker limit) | https://claude.ai/artifact/6A93tJFkhN5Vn5PEQQoezJ |
| cap7 (VTS 544, drop off) | https://claude.ai/artifact/2xEefj4N4psSdn7xinH2Fk |
| cap8 (VTS 544 + drop) | https://claude.ai/artifact/MmLnWLzVwDjrJ4ZWLD11h8 |
| cap20 (42 ms overlay, 167 RPM) | https://claude.ai/artifact/Cv7hcoiSc86yBzuLyrc486 |
| cap20, 0–75 ms zoom | https://claude.ai/artifact/YNbRddqjBf3J25uNJLTtuN |
| cap22 (mbpost, 358 RPM) | https://claude.ai/artifact/89eWcN8rxbm7GcjLVQPyFJ |
| cap22, first 100 ms | https://claude.ai/artifact/9Ubv8fWufMa8MwJJyUjF9q |

## How to reproduce

```sh
# capture (board programmed with scu200_ai_demo03_fcapz; fpgacapZero venv)
source ~/.venvs/fcapz/bin/activate
fcapz --backend hw_server --port 3121 --tap xcsu200p --chain 3 capture \
  --probe-file Vivado/src/hdl/fcapz_evtcap.prob \
  --stor-qual-mode 1 --stor-qual-value 1 --stor-qual-mask 1 \
  --trigger-mode edge_detect --trigger-value 0x2 --trigger-mask 0x2 \
  --pretrigger 64 --posttrigger 4000 --timeout 60 --format json --out capN.json

python3 scripts/fcapz_profile.py docs/profiling/captures/cap22.json.gz        # per-stage table
python3 scripts/fcapz_timeline.py docs/profiling/captures/cap22.json.gz -o cap22.html
python3 scripts/fcapz_window.py docs/profiling/captures/cap22.json.gz w.json 0 100   # zoom
python3 scripts/fcapz_timeline.py w.json -o cap22_100ms.html
```

A capture is 4K change-only samples. That covers 3.6–12.8 s, depending on how busy the pipeline is.
