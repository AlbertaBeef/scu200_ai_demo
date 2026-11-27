# CLAUDE.md: scu200_ai_demo

The SCU200 (xcsu200p) runs angle-triggered capture with an OV9281 camera, then INT8 CNN digit inference in HLS cores, then an HDMI overlay. The firmware runs bare-metal on a MicroBlaze. The repo layout, build stages and console keys are in `README.md` and `CONSOLE_KEYS.md`.

## Branches and targets
- **One branch per design:** `scu200_ai_demo01`, `scu200_ai_demo02`, `scu200_ai_demo03`. Each builds on the one before. Work on the branch whose design you're changing.
- **Commit messages** start with the branch name, e.g. `scu200_ai_demo03: ...`.
- **Make targets:** `scu200_ai_demo01/02/03` and `scu200_ai_demo03_fcapz`. The fcapz target is demo03 plus an fpgacapZero event-timeline ELA on BSCANE2 USER3.
- **`Vitis/common/src/` is shared by all targets**, including `ov9281.c`. A change there reaches every design at its next build.

## Build
- **Vivado:** `make xsa TARGET=<t> JOBS=8` (builds the HLS IP first if needed).
  - A killed build leaves `Vivado/.<t>.lock` behind, and the next make then silently does nothing. Delete the lock first.
  - The "Critical warnings found" message for 12-4739 and 12-1790 is known and harmless.
- **Firmware:** `make bootimage TARGET=<t> JOBS=8`.
  - On its own this does **not** rebuild after a source change. Run `make -C Vitis clean TARGET=<t> && make clean TARGET=<t>` first.
- **Program the board** (after a power-cycle): `xsct scripts/program.tcl <t>`. This loads the PDI, ELF, background and weights.
  - Prefer it over `reload_elf.tcl`.
  - Don't halt the MicroBlaze over JTAG while it runs.
- **UART console:** `/dev/ttyUSB1`, 115200 8N1.
- **HLS on this beta part rejects `impl=uram`.** Patch the exported RAM RTL instead.

## OV9281 FSIN trigger (important)
In external-trigger mode the sensor is configured in `ov9281.c`. The default is the **V1 pure snapshot**:
- registers: `0x4F00=01 0x3030=04 0x303F=01 0x302C=00 0x302F=7F 0x3823=00 0x0100=00`;
- result: one frame per FSIN, starting 1.86 ms after the strobe.

Things to know:
- **Don't set 0x3823 bit 4 (r_init_man).** Arducam's reference uses `0x3823=0x30`, and that bit makes the sensor send a dim wake frame at 1.06 ms, with the good frame one frame length (VTS) later.
- **0x303F must be ≥ 1.** 0 means no frames.
- **The `K` key cycles the variants V0–V3**, for comparison only.
- **The PL wake-frame drop (`fsin_frame_tag`, the `Q` key) is off by default.** Turning it on with V1 would discard every frame. The core is still useful as an FSIN → SOF timer, which the `B` key prints.

## Rig etiquette (hardware in the loop)
- **Never change rig settings unasked:** N, VTS (`q`), drop (`Q`), trigger variant (`K`), target (`T`/`G`), motor speed, auto-target (`A`), HUD (`h`).
  - State every key you send.
  - Restore any test settings afterwards.
- **After reprogramming, re-apply only the standard setup:** `S`, `F`, `q`×4 (VTS 544), `A`.

## Profiling
- **Write-up:** `docs/profiling/demo03_profiling.md`, with gzipped captures in `docs/profiling/captures/`.
  - Captures are profiling records, **not** training data, so they never go in `train/`.
- **Tools:**
  - `scripts/fcapz_profile.py`: per-stage table, frames per strobe, collisions, digit order;
  - `scripts/fcapz_timeline.py`: interactive HTML swimlanes;
  - `scripts/fcapz_window.py`: cuts a time window out of a capture, for zooming.
- **The capture command** is in the profiling doc's "How to reproduce" section.
- **Current demo03 numbers:** 412 RPM (motor 90 %) with every strobe inferred; worker done 12.96 ms after FSIN. The motor is the limit; the sensor allows about 700 RPM at N=8.

## Docs
- **Data-path and core diagrams** are in `docs/diagrams/`.
  - The HTML pages are the source of truth.
  - Regenerate the SVGs with `docs/diagrams/make_svgs.py`.
  - No PNGs.

## Don't commit
- `train/mbdpu01/*.bin` (local modifications);
- `scripts/verify_weights.tcl`;
- demo01 build logs and screenshots in the repo root;
- `train/captures/mnist_takeNN/`;
- `assets/scu200_ai_demo03_background_new.png`.

Ask before adding any other large binary.
