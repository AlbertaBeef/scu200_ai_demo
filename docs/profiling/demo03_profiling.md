# scu200_ai_demo03 pipeline profiling

**Goal:** raise the maximum disc speed while keeping two guarantees:
- every strobe is captured and inferred;
- the HDMI output never tears.

All timings come from **fcapz** event-timeline captures on the `scu200_ai_demo03_fcapz` target, unless a row says otherwise. Overlay breakdowns come from the `B` console key. Measured 2026-11-25 to 2026-11-26.

**Where things are:**
- Captures: [`captures/`](captures/), gzipped fcapz JSON.
- Commands to re-derive every number: [How to reproduce](#how-to-reproduce).

## Result

| | First fcapz build (07af226) | mbpost (d7308dd) | **Now: V1 snapshot trigger** |
|---|---|---|---|
| Max RPM, every frame inferred (N=8) | ~110 | 358 (verified) | **412 verified** (motor 90 %, the top tested setting) |
| Camera-chain limit (N=8) | ~145 | ~380 | **~700** (sensor needs ~10.7 ms per frame) |
| Frames from the sensor per FSIN | 2 (wake + good) | 2 (wake dropped in the PL) | **1** |
| Good frame starts after FSIN | 26.05 ms | 10.96 ms | **1.86 ms** |
| Frame ready after FSIN | ~34.9 ms | 19.8 ms | **10.7 ms** |
| `mbdpu03_infer()` | 1.00 ms | 1.00 ms | 1.00 ms |
| Overlay draw | **66.1 ms** (MicroBlaze) | 1.14 ms (mbpost HLS) | 1.15 ms |
| Worker done after FSIN | 86–102 ms | 22.0 ms | **12.96 ms** |
| Display collisions (tearing) | 103 in 10 s | 0 | 0 |

**The motor is now the limit, not the pipeline.** At 412 RPM (18.2 ms strobe period) the pipeline is busy 12.96 ms of each cycle, 71 %.

**What changed (2026-11-26):** the OV9281 is now in the datasheet's pure external-trigger snapshot mode. Register 0x3823 is 0x00 instead of 0x30. Its bit 4, r_init_man, was what produced the dim wake frame. See [Camera: FSIN trigger configuration](#camera-fsin-trigger-configuration).

## Pipeline, per strobe (now, cap23 at 412 RPM)

| Stage | Start after FSIN | Duration |
|---|---|---|
| FSIN strobe (`disc_strobe`) | 0 | 10 µs |
| OV9281 wake-up and exposure (snapshot mode) | 0 | 1.86 ms |
| Frame: SOF, then `frmbuf_wr` writes the ring slot | 1.86 ms | 8.83 ms |
| `frmbuf_wr` IRQ → ISR (`FrmbufWrCallback`) | 10.69 ms | 6 µs |
| Worker takes frame; display slot held | +0.01 ms | — |
| `mbdpu03_infer()` (includes the camera-crop read burst, 0.162 ms) | 10.79 ms | 1.00 ms |
| mbpost overlay (preview, bars, DX/DY gauges) + HUD values | 11.81 ms | 1.15 ms |
| Worker done, background buffer flipped | 12.96 ms | — |
| Idle until the next strobe | — | 5.25 ms |

**Share of the latency:**
- The camera is 84 % (1.86 + 8.83 ms). The frame write is the sensor reading out the frame at about 18.4 µs per line.
- The firmware is 2.24 ms: inference + overlay.

**Per-frame limits.** The ring buffer overlaps the worker with the next frame, so the 12.96 ms latency is not a speed limit:

| Path | Time per frame | RPM it allows at N=8 |
|---|---|---|
| Sensor (wake + readout) | ~10.7 ms | ~700 |
| Firmware (inference + overlay) | 2.24 ms | far higher |

**Remaining stall:** once per revolution the auto-target step (UART print plus target-gauge redraw) delays the worker:
- up to 9.2 ms before inference starts, or 3.5 ms before the overlay;
- worker done at worst 22.08 ms, past the next strobe at 18.2 ms.

It cost no frames at 412 RPM, because the next frame only lands 10.7 ms after its strobe. Above ~480 RPM it would start skipping one frame per revolution.

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
| trig_v0…v3 | trigger-variant sweep (`K`), VTS 544, PL drop off, N=8, ~77 RPM | ~97.7 ms | see [the variant table](#camera-fsin-trigger-configuration) | 1.1 ms | — | 0 | Only V1 (0x3823=0x00) gives one frame per FSIN |
| cap23 | **V1 snapshot trigger** default, PL drop off, N=8, **412 RPM** (motor 90 %) | 18.2 ms | 100 % (1/strobe) | 1.15 ms | **12.96 ms** | 0 | 192 frames inferred, digits in order; frame at 1.86 ms |

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

## Camera: FSIN trigger configuration

Datasheet §4.8.3: in external-trigger snapshot mode a FSIN rising edge wakes the OV9281. It integrates, reads out {0x303F} frames, then goes back to sleep. Our trigger config was taken from Arducam's `ov9281_external_trigger.c`, which also sets 0x3823=0x30 (ext_vs_en + r_init_man). That produced a dim wake frame 1.06 ms after FSIN and the good frame one VTS later.

The `K` key cycles four variants. The sweep below was run at ~77 RPM, N=8, VTS 544, with the PL drop off; captures are `trig_v0…v3`.

| Variant | 0x3823 | Other writes | Frames per FSIN | First SOF after FSIN | Brightness | Result |
|---|---|---|---|---|---|---|
| V0 snapshot + ext_vs (old default) | 0x30 | 4F00=01 3030=04 303F=01 0100=00 | 2 | 1.06 ms (dim), 10.96 ms (good) | 21 / ~195 | needs the PL drop |
| **V1 pure snapshot (default)** | **0x00** | same | **1** (132/132) | **1.86 ms** | 191–200 | **one good frame** |
| V2 snapshot + r_init_man | 0x10 | same | 2 | 1.06 / 10.96 ms | 21 / ~199 | same as V0 |
| V3 ext_vs only, streaming, VTS 0xFFFF | 0x30 | 4F00=00 3030=10 0100=01 | 0 or 1 (12/282) | ~70 ms | 251 (saturated) | fails |

**What the sweep shows:**
- **r_init_man (0x3823 bit 4) produces the wake frame.** V2 differs from V1 only in that bit and reproduces V0 exactly.
- **An earlier pure-snapshot attempt (demo01, before 1728b3e) failed only because it used 0x303F=0** (zero frames). With 0x303F=1, snapshot behaves as the datasheet describes.
- **V1 was only measured at VTS 544.** In snapshot mode the SOF should follow the wake-up and exposure, not VTS, so `q` should matter only for V0/V2. That hasn't been checked.
- **The image is exposed 9.1 ms earlier relative to the strobe angle.** Auto-target re-centres it within a few revolutions; the target moved from ~12 to ~27.

**PL drop (`fsin_frame_tag`):** now **off** by default (`Q` toggles it). V1 sends no wake frame, and the 6 ms drop window would discard its 1.86 ms frame. The core stays in the design for V0/V2 and as an FSIN → SOF timer; the `B` key prints its counters.

### Frame length (VTS) with the old V0 trigger

With V0, the good frame came one frame length after the wake frame at ~1.06 ms. Each line is ~18.2 µs (HTS 1456).

| VTS (lines) | Frame length | Good frame starts after FSIN | Strobes answered at a 46 ms period |
|---|---|---|---|
| 1373 (default) | ~25 ms | 26.05 ms | 50 % |
| 1024 | ~18.6 ms | 19.70 ms | 100 % |
| 800 | ~14.6 ms | 15.62 ms | 100 % |
| 640 | ~11.6 ms | 12.71 ms | 100 % |
| 544 (`q`×4) | ~9.9 ms | 10.96 ms | 100 % |

The PL drop window had to close between the wake frame and the good frame: 6010 µs at VTS 544.

**Next levers on the camera chain (not done):**
- **A sensor readout window.** The CNN only uses a 112×112 crop of the 640×480 frame, so fewer readout rows would shorten the 8.83 ms frame write.
- **A shorter HTS.**
- **Moving auto-target out of the worker path**, to remove the 22 ms once-per-revolution outlier.

## Motor setting vs RPM

Medians of ~15 000 console status lines. The first 3 lines after each speed change are skipped as spin-up. Strobe periods are at N=8.

| Motor | 10 % | 20 % | 30 % | 40 % | 50 % | 60 % | 70 % | 80 % | 90 % |
|---|---|---|---|---|---|---|---|---|---|
| RPM | 34 | 78 | 127 | 172 | 220 | 268 | 315 | 361 | 409 |
| Strobe period | 220 ms | 96 ms | 59 ms | 43.6 ms | 34.1 ms | 28.0 ms | 23.8 ms | 20.8 ms | 18.3 ms |

The relationship is close to linear, at about 47.5 RPM per 10 %. With the V1 trigger, 90 % (18.3 ms) runs clean (cap23). Under the old V0 trigger it wasn't reachable, because the camera chain needed 19.8 ms.

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
| cap23 (V1 trigger, 412 RPM) | https://claude.ai/artifact/KwqALoCpFhUu5MYxDWSZpH |
| cap23, first 50 ms | https://claude.ai/artifact/QXd7pGsF3kB1K8uMQgg494 |

## How to reproduce

```sh
# capture (board programmed with scu200_ai_demo03_fcapz; fpgacapZero venv)
source ~/.venvs/fcapz/bin/activate
fcapz --backend hw_server --port 3121 --tap xcsu200p --chain 3 capture \
  --probe-file Vivado/src/hdl/fcapz_evtcap.prob \
  --stor-qual-mode 1 --stor-qual-value 1 --stor-qual-mask 1 \
  --trigger-mode edge_detect --trigger-value 0x2 --trigger-mask 0x2 \
  --pretrigger 64 --posttrigger 4000 --timeout 60 --format json --out capN.json

python3 scripts/fcapz_profile.py docs/profiling/captures/cap23.json.gz        # per-stage table
python3 scripts/fcapz_timeline.py docs/profiling/captures/cap23.json.gz -o cap23.html
python3 scripts/fcapz_window.py docs/profiling/captures/cap23.json.gz w.json 0 100   # zoom
python3 scripts/fcapz_timeline.py w.json -o cap23_100ms.html
```

A capture is 4K change-only samples. That covers 3.6–12.8 s, depending on how busy the pipeline is.
