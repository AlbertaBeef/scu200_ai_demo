# SCU200 AI Demo — `scu200_ai_demo01` / `02` / `03`

A rotating-disc handwritten-digit (MNIST) recognition demo on the **AMD/Xilinx SCU200**
(Spartan UltraScale+ `xcsu200p-sbva1024-2-e-es1`, ES1), MicroBlaze bare-metal, Vivado + Vitis
2025.2.

A DC gearmotor spins a disc printed with digits; a fabric **encoder-decode + angle strobe**
(`disc_strobe`) fires the camera (OmniVision OV9281 global-shutter) at fixed disc angles so the
image freezes; a custom INT8 CNN classifies the digit; results + telemetry are drawn over HDMI,
and captured ROIs can be recorded to retrain the classifier.

## Designs

Three designs, one git branch each (`scu200_ai_demo01/02/03`); each builds on the one before and
the older ones still build with their own `TARGET`.

| Target | AI cores | What it adds |
|---|---|---|
| `scu200_ai_demo01` | `mbpre` (ROI crop → 28×28) + `mbdpu01` (28×28 CNN) | digit recognition; MicroBlaze draws the overlay |
| `scu200_ai_demo02` | `mbpre` + `mbdpu02` | dx/dy digit-position head, auto-target (`A`), position gauges |
| `scu200_ai_demo03` | `mbdpu03` (camera crop fused in, 112×112 CNN, URAM; no `mbpre`) + `mbpost` (HLS overlay renderer) | 4× input resolution, overlay 42 ms → 1.14 ms, OV9281 pure-snapshot trigger: **412 RPM with every strobe inferred** |
| `scu200_ai_demo03_fcapz` | demo03 + fpgacapZero event-timeline ELA (JTAG USER3) | pipeline profiling (see `docs/profiling/`) |

Inference per capture, measured on the board: demo01/02 PRE 0.6 + DPU 3.2 ms; demo03 DPU 1.0 ms.
Block diagrams, cycle budgets and resources of every core: `docs/diagrams/scu200_ai_cores.html`.

The stages below use `TARGET=scu200_ai_demo01`; substitute any target from the table. Each
target's extra steps (training, background art) are in its own section further down.

---

## Prerequisites

- **Vivado 2025.2** and **Vitis 2025.2** installed; environment sourced so `vivado` and
  `vitis-run` are on `PATH` and `$XILINX_VIVADO` is set:
  ```bash
  source /path/to/Xilinx/2025.2/Vivado/settings64.sh
  source /path/to/Xilinx/2025.2/Vitis/settings64.sh
  ```
- **Beta device enabled.** `xcsu200p…-es1` is a 2025.2 beta device. The HLS build calls
  `enable_beta_device` and `get_part.tcl` hard-codes the part (the SCU200 has no board file), but
  your Vivado install must have the beta device family enabled (check the startup line
  `NN Beta devices … enabled`). If needed, add to `~/.Xilinx/Vivado/Vivado_init.tcl`:
  `enable_beta_device xcsu200*`.
- `make`, and network access on the first IP build if the board store needs refreshing (not
  required for SCU200 — the part is hard-coded).
- **Python environment** for the host tools (`tools/`, `scripts/fcapz_*.py`) and model retraining
  (`Vivado/ip/mbdpu0*/train_*.py`). Create a repo-local venv (gitignored):
  ```bash
  python3 -m venv venv                       # base Python 3.12
  venv/bin/pip install --upgrade pip
  venv/bin/pip install -r requirements.txt    # CPU-only torch/torchvision + numpy + pillow
  ```
  Then run the tools with `venv/bin/python3 …` (or `source venv/bin/activate` first). If you only
  need the host tools, `venv/bin/pip install pillow numpy` is enough — `png_to_rgb888.py` needs
  just Pillow; `torch`/`torchvision` are only for retraining. See `requirements.txt` for the
  pinned, known-good versions.

---

## Repository layout

```
scu200_ai_demo/
├── README.md                      ← this file (build instructions)
├── CONSOLE_KEYS.md                UART key cheat-sheet (motor / strobe / crop / capture / trigger)
├── CLAUDE.md                      working notes for AI-assisted development
├── LICENSE                        MIT
├── Makefile                       top-level orchestration: `make bootimage` → Vitis app + boot zip
├── requirements.txt               Python deps for tools/ + retraining (CPU torch, numpy, pillow)
├── train/                         all training material
│   ├── captures/                  labeled ROI datasets (core-agnostic, shared across cores)
│   │   ├── mnist_take01/          session 1 (centered framing)
│   │   ├── mnist_take02/          session 2 (tighter framing)
│   │   └── mnist_take03/          session 3
│   │       each session holds (row-for-row aligned):
│   │         captures_28x28.npz    28×28 INT8 0–127   (mbdpu01/02 input)
│   │         captures_112x112.npz  112×112 uint8 0–255 (raw hi-res crop; mbdpu02/03 training)
│   │         manifest.csv          per-sample metadata (+ a preview GIF)
│   ├── mbdpu01/                   model blobs for mbdpu01 (weights → DDR 0xB0000000, params → 0xB0010000)
│   ├── mbdpu02/                   … for mbdpu02 (+ expected outputs, auto-target run log)
│   └── mbdpu03/                   … for mbdpu03 (112×112 CNN + position head)
│       each holds README.md (model + how to regenerate), mnist_{w,p}blob.bin, mnist_tests*.bin
├── Vitis/                         MicroBlaze bare-metal application (boot image)
│   ├── Makefile                   Vitis workspace + bootfile build (TARGET=…)
│   ├── py/                        build-vitis.py, make-boot.py, args.json (target → bd_name map)
│   └── common/src/                42 driver/app files shared by all targets — main.c, mbdpu01.{c,h}
│                                  (mbdpu01/02/03 drivers), mnist_pre (MicroBlaze overlay),
│                                  disc_strobe, ov9281, motor, HDMI/I2C/clocking, frmbuf, fcapz_mark …
├── scripts/                       JTAG (xsct .tcl, optional TARGET argument) + profiling tools
│   ├── program.tcl               FPGA .pdi + app ELF + background + weight blobs → board
│   ├── target.tcl                resolves the target → .pdi/.elf/background/blobs (used by all .tcl)
│   ├── reload_elf.tcl            hot-reload the ELF only (fast edit loop)
│   ├── reload_elf_bg.tcl         reload ELF + refresh background
│   ├── reload_weights.tcl        live weight reload (retrained model, no rebuild)
│   ├── dump_gallery.tcl          dump the ROI capture gallery (DDR 0xBA000000) → .bin
│   ├── fcapz_profile.py          fcapz capture → per-stage table, frames/strobe, collisions, digits
│   ├── fcapz_timeline.py         fcapz capture → interactive HTML swimlanes
│   └── fcapz_window.py           cut a time window out of a capture (zoom)
├── tools/                         host-side Python
│   ├── roi_export.py             gallery .bin → labeled captures_{28x28,112x112}.npz
│   └── png_to_rgb888.py          background PNG → 1920×1080 RGB888 blob (DDR 0xB4000000)
├── assets/
│   └── scu200_ai_demo0{1,2,3}_background.png   HDMI background art per design (source for the bg blob)
├── docs/
│   ├── diagrams/                  data-path diagrams per design + AI-core diagrams (HTML = source;
│   │                              SVGs regenerated with make_svgs.py)
│   └── profiling/                 demo03_profiling.md + gzipped fcapz captures
└── Vivado/
    ├── Makefile                   top-level Vivado build (TARGET=…)
    ├── scripts/
    │   ├── build.tcl              create project + block design
    │   └── xsa.tcl               synth / implement / export XSA+PDI
    ├── ip/
    │   ├── Makefile               HLS IP build (all cores, every target)
    │   ├── get_part.tcl           resolves the SCU200 part -> build/<board>/settings.tcl
    │   ├── disc_strobe/           encoder quadrature decode + angle-triggered FSIN strobe
    │   ├── mbpre/                 HW ROI crop/downscale to 28×28 (demo01/02)
    │   ├── mbdpu01/               28×28 INT8 MNIST CNN — HLS source + training scripts
    │   ├── mbdpu02/               mbdpu01 + dx/dy position head — HLS + train_mnist_pos.py
    │   ├── mbdpu03/               112×112 CNN + position head, camera crop fused in, URAM —
    │   │                          HLS + train_mnist112.py + patch_uram.py
    │   └── mbpost/                HLS overlay renderer (preview, logit bars, DX/DY gauges → DDR)
    └── src/
        ├── bd/                    bd_<target>.tcl per target, fcapz_evtcap_bd.tcl, mipi_locs.tcl
        ├── constraints/           <target>.xdc per target (camera, HDMI, motor/encoder, XVS)
        └── hdl/                   fsin_frame_tag.v (FSIN → SOF timer / wake-frame drop),
                                   fcapz_evtcap.{v,prob} + fcapz/ (fpgacapZero ELA, fcapz target)
```

Each block design instantiates its custom HLS cores (see **Designs**) plus the stock
camera/HDMI/DDR pipeline. The trained model blobs live under **`train/<core>/`** — runtime data,
not read by the Vivado build (only by the HLS C-sim), loaded to DDR by the programming step, and
regenerable from the training scripts in `Vivado/ip/<core>/` (they are specific to each core's
topology + quantization, hence the per-core sub-dir). See `train/<core>/README.md`.

---

## Stage 1 — Build the FPGA hardware (Vivado)

All commands run from the `Vivado/` directory. Pass `TARGET=` explicitly (`make help` lists the
targets).

### 1a. Build the HLS IP cores
```bash
cd Vivado
make ip TARGET=scu200_ai_demo01
```
This runs `get_part.tcl` (writes `ip/build/scu200/settings.tcl` with the SCU200 part), then
HLS C-sim (where a testbench exists) + C-synth + IP export for **all** cores — `disc_strobe`,
`mbpre`, `mbdpu01`, `mbdpu02`, `mbdpu03`, `mbpost` — whatever the target (they share
`ip/build/scu200/`). `mbdpu03`'s build also runs `patch_uram.py` on the exported RTL (see *Known
issues*).
**Expected:** `ip/build/scu200/<core>.prj/sol1/impl/ip/*.zip` for each core and
`ip/build/scu200/ip_done.txt`. A checkout whose IP was built before a newer core existed needs
`make ip` re-run (delete `ip_done.txt`, or `make clean` the target).

### 1b. Build the bitstream (project → synth → impl → XSA/PDI)
```bash
make xsa TARGET=scu200_ai_demo01 JOBS=8
```
(`make xsa` will trigger `make ip` first if the IP isn't built yet, so 1a is optional.)

This creates the Vivado project `scu200_ai_demo01/`, builds the block design from
`bd_scu200_ai_demo01.tcl`, applies `scu200_ai_demo01.xdc`, then synthesizes, implements, and
exports.

**Expected outputs:**
- `Vivado/scu200_ai_demo01/scu200_ai_demo01.runs/impl_1/scu200_ai_demo01_wrapper.pdi` — the programming image
- `Vivado/scu200_ai_demo01/scu200_ai_demo01.runs/impl_1/scu200_ai_demo01_wrapper.bit`
- `Vivado/scu200_ai_demo01/scu200_ai_demo01_wrapper.xsa` — hardware handoff (consumed by the Vitis stage later)

**Expected result:** timing **met** (positive WNS/WHS). The build reports "Critical warnings
found" for a few known, benign ones:
- `Place 30-894` — SCU200 ES1 DDR-clock delay matching in the tight X0Y3 region (timing still met).
- `Vivado 12-1790` — HDMI evaluation-IP license notice.
- `Vivado 12-4739` — known and harmless.

demo03 is the fullest design: about 85 % LUT (86.9 % for the fcapz variant), 4 URAM.

### Notes
- `phys_opt_design` is deliberately disabled for the SCU200 (`board_name == scu200`) in
  `build.tcl` — it perturbs the ES1 clock placement and trips `Place 30-837`. Skipping it keeps
  `place_design`'s valid clock placement.
- Clean a build: `make clean TARGET=scu200_ai_demo01` (removes the project + this target's IP).
- A killed build leaves `Vivado/.<TARGET>.lock` behind, and the next `make` then silently does
  nothing. Delete the lock first.

---

## Stage 2 — Build the Vitis application + boot image

The MicroBlaze bare-metal app builds from `Vitis/common/src/` against the XSA exported by Stage 1.

### From the repo root (one shot)
```bash
make bootimage TARGET=scu200_ai_demo01 JOBS=8
```
Orchestrates: create the Vitis workspace/platform from the XSA → compile the app (`test_app.elf`) →
generate the boot bitstream → gather `bootimages/scu200_ai_demo_scu200_ai_demo01_standalone-2025-2.zip`.
(If the Stage-1 XSA isn't built yet, the dependency triggers the Vivado build first.)

### Or drive Vitis directly
```bash
cd Vitis
make workspace TARGET=scu200_ai_demo01   # workspace + BSP + platform from the XSA
make bootfile  TARGET=scu200_ai_demo01   # compile app -> boot/scu200_ai_demo01/scu200_ai_demo01.bit
```

**Expected outputs:**
- `Vitis/scu200_ai_demo01_workspace/test_app/build/test_app.elf`
- `Vitis/boot/scu200_ai_demo01/scu200_ai_demo01.bit`

(one workspace and boot dir per target)

### Notes
- **Rebuild after a source change:** `make bootimage` alone does not pick up edits in
  `common/src/`. Clean first: `make -C Vitis clean TARGET=<t> && make clean TARGET=<t>`.
- `Vitis/py/args.json` maps each target to its block design (`bd_names`, matching the Stage-1
  wrapper/`.hwh`); `app_name = test_app`. `common/src/` is the single source of truth for **all**
  targets (copied into each workspace), so a change there reaches every design at its next build.
- **No `data.json` needed** — the target is passed explicitly (`DATA_JSON = none`), so the repo is
  self-contained.
- The classifier drivers are in `mbdpu01.{c,h}` (`mbdpu01_infer`, `mbdpu02_infer`,
  `mbdpu03_infer`); `main.c` picks one from the `XPAR_MBDPU0*_*` defines of the core in the
  block design. demo03 also detects `mbpost` and boots with it as the overlay renderer.

---

## Stage 3 — Program the board & the capture → retrain loop

All host/JTAG scripts use paths relative to the repo, so run them from the repo root with the
board connected. Every `.tcl` takes the target as an optional first argument (default
`scu200_ai_demo01`); `scripts/target.tcl` maps it to the `.pdi`, ELF, background and
`train/<core>/` blobs. **Power-cycle the board before re-programming** (the ES1 part can wedge
otherwise), and prefer a full `program.tcl` over `reload_elf.tcl`; don't halt the MicroBlaze over
JTAG while it runs.

### Program (first boot)
Generate the background blob once (it's gitignored — regenerated from the committed PNG), then
program the FPGA + app + DDR data over JTAG:
```bash
python3 tools/png_to_rgb888.py      # assets/…background.png → …_background_rgb.bin (1920×1080 RGB888)
xsct scripts/program.tcl [target]   # .pdi + test_app.elf + bg (0xB4000000) + weights (0xB0000000/0xB0010000)
```
You should get the boot banner on UART, HDMI up with the background, the live camera window, and
the motor/strobe running. Drive the demo over the serial console (`115200 8N1`, `/dev/ttyUSB1` on
the dev host); standard setup after programming: `S`, `F`, `q`×4 (VTS 544), and on demo02/03 `A`.
Press `?` for the
key list, or see **[`CONSOLE_KEYS.md`](CONSOLE_KEYS.md)** (motor, angle strobe, ROI crop, capture,
the `cpr` drift-trim, …).

### Fast iteration (no re-program)
| Script | Reloads | Use when |
|--------|---------|----------|
| `xsct scripts/reload_elf.tcl`     | app ELF only          | you rebuilt `test_app.elf` |
| `xsct scripts/reload_elf_bg.tcl`  | app ELF + background   | …and changed the background art |
| `xsct scripts/reload_weights.tcl` | weight/param blobs     | you retrained the model (below) |

### Capture → label → retrain → deploy
The cores are **weight-agnostic** (mbdpu01/02 read the blobs from DDR on every inference; mbdpu03
copies them into URAM/BRAM on the first inference and whenever the param blob changes), so a
retrained model deploys with a weight reload alone — no bitstream or ELF rebuild. The flow below
is for mbdpu01; demo02/03 retrain with their own script (see their sections) and reload with
`xsct scripts/reload_weights.tcl <target>`.
```bash
# 1. On the board: press 'w' to arm capture, spin the disc until REC fills (e.g. 1024/1024).
# 2. Dump the on-DDR gallery to the host:
xsct scripts/dump_gallery.tcl                 # → train/captures/roi_gallery.bin
# 3. Auto-label by angular slot (disc layout 1..8) → the two aligned datasets:
python3 tools/roi_export.py --bin train/captures/roi_gallery.bin \
        --out train/captures/mnist_takeNN --map 1,2,3,4,5,6,7,8
# 4. Retrain + PTQ on inverted-MNIST mixed with the real captures → new INT8 blobs:
./venv/bin/python3 Vivado/ip/mbdpu01/train_mnist_real.py \
        --real train/captures/mnist_take01/captures_28x28.npz \
               train/captures/mnist_take02/captures_28x28.npz \
        --out train/mbdpu01
# 5. Live-deploy the retrained weights (no rebuild):
xsct scripts/reload_weights.tcl
```
`roi_export.py` auto-labels **circularly** (a sample completing just past the position wrap folds
back to the correct slot instead of landing in slot 0) and writes `captures_28x28.npz` (model input)
+ `captures_112x112.npz` (raw crop), row-for-row aligned with `manifest.csv`. On demo03 the
gallery keeps the same format: the 28×28 field is a 4×4 box average of mbdpu03's 112×112 input.

---

## `scu200_ai_demo02` — digit-position head + auto-target

`scu200_ai_demo02` is demo01 with the `mbdpu02` core in place of `mbdpu01`: the same CNN plus a
two-output head that regresses the digit's position in the ROI (**dx**, **dy** in model pixels).
dx can steer the strobe `target` automatically (`A` key); both are shown as gauges around the
ROI preview. See `train/mbdpu02/README.md` for the model and `CONSOLE_KEYS.md` for the keys.
demo01 is untouched and still builds with its own target.

```bash
# 1. (optional) retrain the CNN + position head -> train/mbdpu02/
venv/bin/python3 Vivado/ip/mbdpu02/train_mnist_pos.py --data data --out train/mbdpu02 \
    --real train/captures/mnist_take0{1,2,3}/captures_112x112.npz
# 2. hardware (builds mbdpu02 HLS IP incl. C-sim, then the bitstream). In a checkout whose IP
#    was built before mbdpu02 existed, rebuild the IP first: make -C Vivado/ip ip TARGET=scu200_ai_demo02
cd Vivado && make xsa TARGET=scu200_ai_demo02 JOBS=8 && cd ..
# 3. software + boot image
make bootimage TARGET=scu200_ai_demo02 JOBS=8
# 4. background blob (once) + program
venv/bin/python3 tools/png_to_rgb888.py --png assets/scu200_ai_demo02_background.png \
    --out assets/scu200_ai_demo02_background_rgb.bin
xsct scripts/program.tcl scu200_ai_demo02
```
All JTAG scripts take the target as an optional argument (default `scu200_ai_demo01`), e.g.
`xsct scripts/reload_elf.tcl scu200_ai_demo02`, `xsct scripts/reload_weights.tcl scu200_ai_demo02`.

## `scu200_ai_demo03` — 112×112 CNN, HLS overlay, snapshot trigger

`scu200_ai_demo03` replaces `mbpre` + `mbdpu02` with **`mbdpu03`** and adds **`mbpost`**:
- **`mbdpu03`** reads the ROI straight from the camera frame in DDR (crop/zoom fused in, read
  overlapped with conv1 as a dataflow stage), runs a 4-conv 112×112 mono INT8 CNN with the
  dx/dy position head (in 112-px units), and writes the 112×112 preview back to DDR. Weights stay
  on chip (URAM/BRAM). ~73k cycles in HLS; `mbdpu03_infer()` measures 1.00 ms on the board.
  Model and accuracy: `train/mbdpu03/README.md`.
- **`mbpost`** draws the preview, logit bars and DX/DY gauges straight into the DDR back buffer
  in 1.14 ms (the MicroBlaze took 42–66 ms). `u` cycles mbpost → full → delta.
- **OV9281 V1 pure-snapshot trigger** (`ov9281.c`, shared by all targets): one frame per FSIN,
  starting 1.86 ms after the strobe. `K` cycles the variants V0–V3 for comparison; the PL
  wake-frame drop (`fsin_frame_tag`, `Q`) is off by default — with V1 it would drop every frame.
- The DX/DY, 0–9 and "Target Offset" labels are part of `assets/scu200_ai_demo03_background.png`;
  the firmware only clears the gauge bands, value boxes and bars, which are black in the art.
  Keep those regions black if you edit it.

Result: **412 RPM** (motor 90 %, the top tested setting) with every strobe inferred and no
tearing; the worker is done 12.96 ms after FSIN. The motor is now the limit; the sensor would allow
about 700 RPM at N=8. Details: `docs/profiling/demo03_profiling.md`.

```bash
# 1. (optional) retrain the 112x112 CNN + position head -> train/mbdpu03/ (CPU, ~40 min on 32 cores)
venv/bin/python3 Vivado/ip/mbdpu03/train_mnist112.py --data data --out train/mbdpu03 \
    --real train/captures/mnist_take0{1,2,3}/captures_112x112.npz
# 2. hardware (re-run `make -C Vivado/ip ip TARGET=scu200_ai_demo03` first in an older checkout)
cd Vivado && make xsa TARGET=scu200_ai_demo03 JOBS=8 && cd ..
# 3. software + boot image
make bootimage TARGET=scu200_ai_demo03 JOBS=8
# 4. background blob (once) + program
venv/bin/python3 tools/png_to_rgb888.py --png assets/scu200_ai_demo03_background.png \
    --out assets/scu200_ai_demo03_background_rgb.bin
xsct scripts/program.tcl scu200_ai_demo03
```

### Profiling build: `scu200_ai_demo03_fcapz`
demo03 plus an fpgacapZero (`fcapz`, Apache-2.0, sources in `Vivado/src/hdl/fcapz/`) event-timeline ELA on BSCANE2 USER3; the firmware
marks pipeline stages through a GPIO (`fcapz_mark.h`). It uses demo03's weights and background.
Build and program it like demo03 with `TARGET=scu200_ai_demo03_fcapz`. The capture command is in
`docs/profiling/demo03_profiling.md` (*How to reproduce*); then:
```bash
python3 scripts/fcapz_profile.py capN.json            # per-stage table, frames/strobe, collisions
python3 scripts/fcapz_timeline.py capN.json -o capN.html   # interactive swimlanes
python3 scripts/fcapz_window.py capN.json w.json 0 100      # cut 0-100 ms out, to zoom
```

## Known issues

- **Triggered-capture drift (motor encoder).** The angle strobe is driven by the motor's
  quadrature encoder, which sits *before* the 20.4:1 gearbox — so a fixed encoder count is not an
  exact disc angle (the gearbox ratio is non-integer, plus backlash). The `disc_strobe` core uses
  a Q16 fractional phase accumulator and `cpr` is calibrated to **979.6** counts/disc-revolution
  (live-trimmable over UART with the `C`/`V` keys). This reduces the creep to near-zero over short
  runs, but it is **not perfect** — a small residual remains and can accumulate over long runs.
  A guaranteed zero-drift fix needs a disc-axis (post-gearbox) once-per-revolution index sensor to
  re-anchor the angle each turn.

- **HLS can't bind URAM on this beta part.** On `xcsu200p-sbva1024-2-e-es1`, Vitis HLS 2025.2
  rejects `bind_storage impl=uram` (HLS 214-370) — no `enable_beta_device` or init flag unlocks
  it — although the part has 64 URAM288 that Vivado can infer. `mbdpu01/02`, `mbpre` and
  `disc_strobe` therefore use BRAM/LUTRAM only. `mbdpu03` declares its `uram_*` arrays as BRAM and
  `Vivado/ip/mbdpu03/patch_uram.py` (run by its `run_hls.tcl`) rewrites the exported RTL so Vivado
  maps them to URAM (`ram_style "ultra"`, no `$readmemh`, no-change write ports).

- **Once-per-revolution stall (demo03).** The auto-target step (UART print + target-gauge redraw)
  delays the worker by up to ~9 ms once per revolution. It costs no frames at 412 RPM, but above
  ~480 RPM it would skip one frame per revolution.

- **Camera watchdog.** If the OV9281 stops sending frames (3 revolutions without one while
  strobing), the firmware resets and re-configures it (`cam: WATCHDOG #n` on the console).

---

## License

MIT — see [`LICENSE`](LICENSE).
