# SCU200 AI Demo — `scu200_ai_demo01`

A rotating-disc handwritten-digit (MNIST) recognition demo on the **AMD/Xilinx SCU200**
(Spartan UltraScale+ `xcsu200p-sbva1024-2-e-es1`, ES1), MicroBlaze bare-metal, Vivado + Vitis
2025.2.

A DC gearmotor spins a disc printed with digits; a fabric **encoder-decode + angle strobe**
(`disc_strobe`) fires the camera (OmniVision OV9281 global-shutter) at fixed disc angles so the
image freezes; a hardware preprocessor (`mbpre`) crops/downscales the ROI to 28×28; and a custom
INT8 CNN (`mbdpu01`) classifies the digit. Results + telemetry are drawn over HDMI, and captured
ROIs can be recorded to retrain the classifier.

> **Build model:** this repo is populated progressively. Each stage below is meant to be built and
> validated independently. **Stage 1 (FPGA hardware / Vivado) is present now**; later stages
> (Vitis app, programming, capture/training) will be added.

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

---

## Repository layout (current)

```
scu200_ai_demo/
├── README.md                      ← this file (build instructions)
├── CONSOLE_KEYS.md                UART key cheat-sheet (motor / strobe / crop / capture)
├── LICENSE                        MIT
├── .gitignore
├── Makefile                       top-level orchestration: `make bootimage` → Vitis app + boot zip
├── train/                         all training material
│   ├── captures/                  labeled ROI datasets (core-agnostic, shared across cores)
│   │   ├── mnist_take01/          session 1 (centered framing)
│   │   └── mnist_take02/          session 2 (tighter framing)
│   │       each session holds (row-for-row aligned):
│   │         captures_28x28.npz    28×28 INT8 0–127   (the model input)
│   │         captures_112x112.npz  112×112 uint8 0–255 (raw hi-res crop)
│   │         manifest.csv          per-sample metadata (+ a preview GIF)
│   └── mbdpu01/                    model OUTPUT for the mbdpu01 CNN (runtime blobs → DDR at program)
│       ├── README.md              what the blobs are + how to regenerate
│       ├── mnist_wblob.bin        weights (INT8) → DDR 0xB0000000
│       ├── mnist_pblob.bin        biases + requant params (INT32) → DDR 0xB0010000
│       └── mnist_tests.bin        20 verification samples (C-sim / HW spot-check)
├── Vitis/                         MicroBlaze bare-metal application (boot image)
│   ├── Makefile                   Vitis workspace + bootfile build (target: scu200_ai_demo01)
│   ├── py/                        build-vitis.py, make-boot.py, args.json (bd_name=scu200_ai_demo01)
│   └── common/src/                42 driver/app files — main.c, mbdpu01 (MNIST) driver, mnist_pre,
│                                  disc_strobe, ov9281, motor, HDMI/I2C/clocking, frmbuf, pipe …
├── scripts/                       JTAG program + hot-reload + gallery-dump (xsct .tcl)
│   ├── program.tcl               FPGA .pdi + app ELF + background + weight blobs → board
│   ├── reload_elf.tcl            hot-reload the ELF only (fast edit loop)
│   ├── reload_elf_bg.tcl         reload ELF + refresh background
│   ├── reload_weights.tcl        live weight reload (retrained model, no rebuild)
│   └── dump_gallery.tcl          dump the ROI capture gallery (DDR 0xBA000000) → .bin
├── tools/                         host-side Python
│   ├── roi_export.py             gallery .bin → labeled captures_{28x28,112x112}.npz
│   └── png_to_rgb888.py          background PNG → 1920×1080 RGB888 blob (DDR 0xB4000000)
├── assets/
│   └── scu200_ai_demo01_background.png   HDMI background art (source for the bg blob)
└── Vivado/
    ├── Makefile                   top-level Vivado build (target: scu200_ai_demo01)
    ├── scripts/
    │   ├── build.tcl              create project + block design
    │   └── xsa.tcl               synth / implement / export XSA+PDI
    ├── ip/
    │   ├── Makefile               HLS IP build (disc_strobe, mbpre, mbdpu01)
    │   ├── get_part.tcl           resolves the SCU200 part -> build/<board>/settings.tcl
    │   ├── disc_strobe/           encoder quadrature decode + angle-triggered FSIN strobe
    │   ├── mbpre/                 HW ROI crop/downscale to 28×28
    │   └── mbdpu01/              INT8 MNIST CNN — HLS source + training scripts
    └── src/
        ├── bd/
        │   ├── bd_scu200_ai_demo01.tcl   the block design
        │   └── mipi_locs.tcl             MIPI pin LOCs
        └── constraints/
            └── scu200_ai_demo01.xdc      pin constraints (camera, HDMI, motor/encoder, XVS)
```

The block design instantiates three custom HLS cores — **`disc_strobe`, `mbpre`, `mbdpu01`** —
plus the stock camera/HDMI/DDR pipeline. The trained model blobs live under **`train/<core>/`**
(e.g. `train/mbdpu01/`) — runtime data, not read by the Vivado build, loaded to DDR by the
programming step, and regenerable from the training scripts in `Vivado/ip/mbdpu01/` (they are
specific to each core's topology + quantization, hence the per-core sub-dir). See
`train/mbdpu01/README.md`.

---

## Stage 1 — Build the FPGA hardware (Vivado)

All commands run from the `Vivado/` directory. The default `TARGET` is `scu200_ai_demo01`.

### 1a. Build the HLS IP cores
```bash
cd Vivado
make ip TARGET=scu200_ai_demo01
```
This runs `get_part.tcl` (writes `ip/build/scu200/settings.tcl` with the SCU200 part), then
HLS C-synth + IP export for `disc_strobe`, `mbpre`, `mbdpu01`.
**Expected:** `ip/build/scu200/{disc_strobe,mbpre,mbdpu01}.prj/sol1/impl/ip/*.zip` and
`ip/build/scu200/ip_done.txt`.

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

**Expected result:** timing **met** (positive WNS/WHS). The build gate (`check_warnings`)
tolerates exactly two benign critical warnings on this design and fails on any other:
- `Place 30-894` — SCU200 ES1 DDR-clock delay matching in the tight X0Y3 region (timing still met).
- `Vivado 12-1790` — HDMI evaluation-IP license notice.

### Notes
- `phys_opt_design` is deliberately disabled for the SCU200 (`board_name == scu200`) in
  `build.tcl` — it perturbs the ES1 clock placement and trips `Place 30-837`. Skipping it keeps
  `place_design`'s valid clock placement.
- Clean a build: `make clean TARGET=scu200_ai_demo01` (removes the project + thOKis target's IP).

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

### Notes
- `Vitis/py/args.json` sets `bd_name = scu200_ai_demo01` (matches the Stage-1 wrapper/`.hwh`) and
  `app_name = test_app`; `common/src/` is the single source of truth (copied into the workspace).
- **No `data.json` needed** — the target is passed explicitly (`DATA_JSON = none`), so the repo is
  self-contained.
- The MNIST classifier driver is `mbdpu01.{c,h}` (`mbdpu01_infer`), keyed off `XPAR_MBDPU01_*` from
  the block design's `mbdpu01` core. The unused milestone-1 loopback core was dropped.

---

## Stage 3 — Program the board & the capture → retrain loop

All host/JTAG scripts use paths relative to the repo, so run them from the repo root with the
board connected. **Power-cycle the board before re-programming** (the ES1 part can wedge otherwise).

### Program (first boot)
Generate the background blob once (it's gitignored — regenerated from the committed PNG), then
program the FPGA + app + DDR data over JTAG:
```bash
python3 tools/png_to_rgb888.py      # assets/…background.png → …_background_rgb.bin (1920×1080 RGB888)
xsct scripts/program.tcl            # .pdi + test_app.elf + bg (0xB4000000) + weights (0xB0000000/0xB0010000)
```
You should get the boot banner on UART, HDMI up with the background, the live camera window, and
the motor/strobe running. Drive the demo over the serial console (`115200 8N1`); press `?` for the
key list, or see **[`CONSOLE_KEYS.md`](CONSOLE_KEYS.md)** (motor, angle strobe, ROI crop, capture,
the `cpr` drift-trim, …).

### Fast iteration (no re-program)
| Script | Reloads | Use when |
|--------|---------|----------|
| `xsct scripts/reload_elf.tcl`     | app ELF only          | you rebuilt `test_app.elf` |
| `xsct scripts/reload_elf_bg.tcl`  | app ELF + background   | …and changed the background art |
| `xsct scripts/reload_weights.tcl` | weight/param blobs     | you retrained the model (below) |

### Capture → label → retrain → deploy
The `mbdpu01` core is **weight-agnostic** (reads the blobs from DDR each inference), so a retrained
model deploys with a weight reload alone — no bitstream or ELF rebuild.
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
+ `captures_112x112.npz` (raw crop), row-for-row aligned with `manifest.csv`.

---

## Known issues

- **Triggered-capture drift (motor encoder).** The angle strobe is driven by the motor's
  quadrature encoder, which sits *before* the 20.4:1 gearbox — so a fixed encoder count is not an
  exact disc angle (the gearbox ratio is non-integer, plus backlash). The `disc_strobe` core uses
  a Q16 fractional phase accumulator and `cpr` is calibrated to **979.6** counts/disc-revolution
  (live-trimmable over UART with the `C`/`V` keys). This reduces the creep to near-zero over short
  runs, but it is **not perfect** — a small residual remains and can accumulate over long runs.
  A guaranteed zero-drift fix needs a disc-axis (post-gearbox) once-per-revolution index sensor to
  re-anchor the angle each turn.

- **URAM not used / not available.** The target `xcsu200p-sbva1024-2-e-es1` is an **ES1** device on
  which Vitis HLS rejects URAM binding (`bind_storage impl=uram`) — no `enable_beta_device` or init
  flag unlocks it. All on-chip storage in the custom cores (`mbdpu01`, `mbpre`, `disc_strobe`) is
  therefore mapped to **BRAM/LUTRAM**, not URAM. On production silicon URAM would be available; the
  HLS sources would need re-binding to take advantage of it.

---

## License

MIT — see [`LICENSE`](LICENSE).
